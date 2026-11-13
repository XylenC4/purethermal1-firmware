/*
 * Minimal USB DFU 1.1 class - see usbd_dfu.h.
 */
#include <string.h>
#include "stm32f4xx_hal.h"
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
#include "usbd_dfu.h"
#include "ptboot.h"

/* DFU class requests */
#define DFU_DETACH     0
#define DFU_DNLOAD     1
#define DFU_UPLOAD     2
#define DFU_GETSTATUS  3
#define DFU_CLRSTATUS  4
#define DFU_GETSTATE   5
#define DFU_ABORT      6

/* DFU states */
enum {
	appIDLE = 0, appDETACH, dfuIDLE, dfuDNLOAD_SYNC, dfuDNBUSY, dfuDNLOAD_IDLE,
	dfuMANIFEST_SYNC, dfuMANIFEST, dfuMANIFEST_WAIT_RESET, dfuUPLOAD_IDLE, dfuERROR
};

/* DFU status codes */
enum {
	OK = 0, errTARGET, errFILE, errWRITE, errERASE, errCHECK_ERASED, errPROG,
	errVERIFY, errADDRESS, errNOTDONE, errFIRMWARE, errVENDOR, errUSBR, errPOR,
	errUNKNOWN, errSTALLEDPKT
};

enum { WORK_NONE = 0, WORK_WRITE, WORK_MANIFEST };

#define DFU_DESC_TYPE_FUNCTIONAL 0x21
#define DFU_ATTR_CAN_DNLOAD      0x01
#define DFU_ATTR_CAN_UPLOAD      0x02
#define DFU_ATTR_MANIFEST_TOL    0x04

/* F411 flash sectors */
static const uint32_t sector_addr[8] = {
	0x08000000, 0x08004000, 0x08008000, 0x0800C000,
	0x08010000, 0x08020000, 0x08040000, 0x08060000
};
static const uint32_t sector_size[8] = {
	0x4000, 0x4000, 0x4000, 0x4000, 0x10000, 0x20000, 0x20000, 0x20000
};
#define FIRST_APP_SECTOR 2

static struct {
	uint8_t  state;
	uint8_t  status;
	volatile uint8_t work;
	uint8_t  erased;          /* bitmask of sectors erased in this session */
	uint8_t  session;         /* a download is in progress */
	uint8_t  manifested;      /* a complete image was written */
	volatile uint8_t reboot;  /* reboot requested */
	uint16_t block;           /* block number of the pending block */
	uint16_t next_block;
	uint16_t length;          /* length of the pending block */
	uint32_t written;         /* bytes accepted so far */
	uint32_t head[2];         /* held-back first 8 bytes of the image */
	uint8_t  buf[DFU_XFER_SIZE] __attribute__((aligned(4)));
	uint8_t  status_buf[6];
} dfu;

/* ------------------------------------------------------------------ */
/* descriptors                                                         */
/* ------------------------------------------------------------------ */

#define CFG_LEN (9 + 9 + 9)
__ALIGN_BEGIN static uint8_t cfg_desc[CFG_LEN] __ALIGN_END = {
	/* configuration */
	9, USB_DESC_TYPE_CONFIGURATION, CFG_LEN, 0, 1, 1, 0, 0x80, 50,
	/* interface 0, DFU mode */
	9, USB_DESC_TYPE_INTERFACE, 0, 0, 0, 0xFE, 0x01, 0x02, USBD_IDX_INTERFACE_STR,
	/* DFU functional descriptor */
	9, DFU_DESC_TYPE_FUNCTIONAL,
	DFU_ATTR_CAN_DNLOAD | DFU_ATTR_CAN_UPLOAD | DFU_ATTR_MANIFEST_TOL,
	0xE8, 0x03,                                 /* wDetachTimeOut 1000 ms */
	DFU_XFER_SIZE & 0xFF, DFU_XFER_SIZE >> 8,   /* wTransferSize */
	0x10, 0x01                                  /* bcdDFUVersion 1.1 */
};

static uint8_t *get_cfg_desc(uint16_t *length)
{
	*length = sizeof(cfg_desc);
	return cfg_desc;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void set_error(uint8_t status)
{
	dfu.status = status;
	dfu.state = dfuERROR;
	dfu.work = WORK_NONE;
}

static void reset_session(void)
{
	dfu.session = 0;
	dfu.erased = 0;
	dfu.written = 0;
	dfu.next_block = 0;
	dfu.head[0] = dfu.head[1] = 0xFFFFFFFFu;
}

/* Milliseconds the host should wait before polling again for the pending block. */
static uint32_t pending_poll_ms(void)
{
	uint32_t start = PTAPP_BASE + (uint32_t)dfu.block * DFU_XFER_SIZE;
	uint32_t end = start + dfu.length;
	uint32_t ms = 20;
	for (int s = FIRST_APP_SECTOR; s < 8; s++) {
		if ((dfu.erased & (1u << s)) == 0 &&
		    start < sector_addr[s] + sector_size[s] && end > sector_addr[s])
			ms += sector_size[s] >= 0x20000 ? 2500 : sector_size[s] >= 0x10000 ? 1300 : 600;
	}
	return ms;
}

static int erase_needed_sectors(uint32_t start, uint32_t end)
{
	for (int s = FIRST_APP_SECTOR; s < 8; s++) {
		if (dfu.erased & (1u << s))
			continue;
		if (!(start < sector_addr[s] + sector_size[s] && end > sector_addr[s]))
			continue;
		FLASH_EraseInitTypeDef e = {
			.TypeErase = FLASH_TYPEERASE_SECTORS,
			.Sector = s,
			.NbSectors = 1,
			.VoltageRange = FLASH_VOLTAGE_RANGE_3,
		};
		uint32_t bad = 0;
		if (HAL_FLASHEx_Erase(&e, &bad) != HAL_OK)
			return -1;
		dfu.erased |= (1u << s);
	}
	return 0;
}

static int program(uint32_t addr, const uint8_t *data, uint32_t len)
{
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t w = 0xFFFFFFFFu;
		memcpy(&w, data + i, (len - i) < 4 ? (len - i) : 4);
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK)
			return -1;
		if (*(volatile uint32_t *)(addr + i) != w)
			return -2;
	}
	return 0;
}

static void do_write(void)
{
	uint32_t offset = (uint32_t)dfu.block * DFU_XFER_SIZE;
	uint32_t addr = PTAPP_BASE + offset;
	uint32_t len = dfu.length;
	const uint8_t *src = dfu.buf;
	int r;

	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
	                       FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	if (erase_needed_sectors(addr, addr + len) != 0) {
		HAL_FLASH_Lock();
		set_error(errERASE);
		return;
	}
	if (offset == 0) {
		/* hold back initial SP + reset vector until manifestation */
		memcpy(dfu.head, src, 8);
		addr += 8; src += 8; len -= 8;
	}
	r = program(addr, src, len);
	HAL_FLASH_Lock();
	if (r == -1) { set_error(errPROG); return; }
	if (r == -2) { set_error(errVERIFY); return; }

	dfu.written = offset + dfu.length;
	dfu.next_block = dfu.block + 1;
	dfu.state = dfuDNLOAD_IDLE;
}

static void do_manifest(void)
{
	int r;
	HAL_FLASH_Unlock();
	r = program(PTAPP_BASE, (const uint8_t *)dfu.head, 8);
	HAL_FLASH_Lock();
	if (r != 0) { set_error(errVERIFY); return; }
	dfu.manifested = 1;
	reset_session();
	dfu.state = dfuIDLE;      /* manifestation tolerant */
}

/* ------------------------------------------------------------------ */
/* class callbacks                                                     */
/* ------------------------------------------------------------------ */

static uint8_t dfu_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
	(void)cfgidx;
	pdev->pClassData = &dfu;  /* non-NULL so the core calls DeInit on bus reset */
	if (dfu.state != dfuERROR) {
		dfu.state = dfuIDLE;
		dfu.status = OK;
	}
	return USBD_OK;
}

static uint8_t dfu_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
	(void)cfgidx;
	pdev->pClassData = NULL;
	if (dfu.manifested)
		dfu.reboot = 1;       /* host reset us after a finished download */
	return USBD_OK;
}

static void handle_dnload(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
	if (dfu.state != dfuIDLE && dfu.state != dfuDNLOAD_IDLE) {
		if (req->wLength) USBD_CtlError(pdev, req);
		set_error(errSTALLEDPKT);
		return;
	}
	if (req->wLength == 0) {
		/* end of download */
		if (!dfu.session || dfu.written <= 8) {
			set_error(errNOTDONE);
			return;
		}
		dfu.state = dfuMANIFEST_SYNC;
		return;
	}
	if (req->wLength > DFU_XFER_SIZE) {
		USBD_CtlError(pdev, req);
		set_error(errSTALLEDPKT);
		return;
	}
	if (req->wValue == 0) {
		reset_session();
		dfu.session = 1;
		dfu.manifested = 0;
	} else if (!dfu.session || req->wValue != dfu.next_block) {
		USBD_CtlError(pdev, req);
		set_error(errADDRESS);
		return;
	}
	if ((uint32_t)req->wValue * DFU_XFER_SIZE + req->wLength > PTAPP_MAX_SIZE ||
	    (req->wValue == 0 && req->wLength < 8)) {
		USBD_CtlError(pdev, req);
		set_error(errADDRESS);
		return;
	}
	dfu.block = req->wValue;
	dfu.length = req->wLength;
	USBD_CtlPrepareRx(pdev, dfu.buf, req->wLength);
}

static void handle_upload(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
	if (dfu.state != dfuIDLE && dfu.state != dfuUPLOAD_IDLE) {
		USBD_CtlError(pdev, req);
		set_error(errSTALLEDPKT);
		return;
	}
	uint32_t offset = (uint32_t)req->wValue * req->wLength;
	uint32_t len = 0;
	if (offset < PTAPP_MAX_SIZE) {
		len = PTAPP_MAX_SIZE - offset;
		if (len > req->wLength) len = req->wLength;
	}
	dfu.state = (len == req->wLength) ? dfuUPLOAD_IDLE : dfuIDLE;
	USBD_CtlSendData(pdev, (uint8_t *)(PTAPP_BASE + offset), len);
}

static void handle_getstatus(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
	uint32_t poll = 0;
	switch (dfu.state) {
	case dfuDNLOAD_SYNC:
		poll = pending_poll_ms();
		dfu.state = dfuDNBUSY;
		dfu.work = WORK_WRITE;
		break;
	case dfuDNBUSY:
		poll = 100;           /* still erasing/programming */
		break;
	case dfuMANIFEST_SYNC:
		poll = 50;
		dfu.state = dfuMANIFEST;
		dfu.work = WORK_MANIFEST;
		break;
	case dfuMANIFEST:
		poll = 50;
		break;
	default:
		break;
	}
	dfu.status_buf[0] = dfu.status;
	dfu.status_buf[1] = poll & 0xFF;
	dfu.status_buf[2] = (poll >> 8) & 0xFF;
	dfu.status_buf[3] = (poll >> 16) & 0xFF;
	dfu.status_buf[4] = dfu.state;
	dfu.status_buf[5] = 0;
	USBD_CtlSendData(pdev, dfu.status_buf, req->wLength < 6 ? req->wLength : 6);
}

static uint8_t dfu_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
	static uint8_t one_byte;

	switch (req->bmRequest & USB_REQ_TYPE_MASK) {
	case USB_REQ_TYPE_CLASS:
		switch (req->bRequest) {
		case DFU_DNLOAD:    handle_dnload(pdev, req); break;
		case DFU_UPLOAD:    handle_upload(pdev, req); break;
		case DFU_GETSTATUS: handle_getstatus(pdev, req); break;
		case DFU_CLRSTATUS:
			if (dfu.state == dfuERROR) {
				dfu.status = OK;
				dfu.state = dfuIDLE;
				reset_session();
			}
			break;
		case DFU_GETSTATE:
			one_byte = dfu.state;
			USBD_CtlSendData(pdev, &one_byte, 1);
			break;
		case DFU_ABORT:
			if (dfu.state != dfuDNBUSY && dfu.state != dfuMANIFEST) {
				dfu.state = dfuIDLE;
				dfu.status = OK;
				reset_session();
			}
			break;
		case DFU_DETACH:
			dfu.reboot = 1;   /* leave DFU mode: reboot into the application */
			break;
		default:
			USBD_CtlError(pdev, req);
			break;
		}
		break;

	case USB_REQ_TYPE_STANDARD:
		switch (req->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			if ((req->wValue >> 8) == DFU_DESC_TYPE_FUNCTIONAL) {
				USBD_CtlSendData(pdev, cfg_desc + 18, req->wLength < 9 ? req->wLength : 9);
			} else {
				USBD_CtlError(pdev, req);
			}
			break;
		case USB_REQ_GET_INTERFACE:
			one_byte = 0;
			USBD_CtlSendData(pdev, &one_byte, 1);
			break;
		case USB_REQ_SET_INTERFACE:
			break;            /* only alt 0; the core sends the status stage */
		default:
			USBD_CtlError(pdev, req);
			break;
		}
		break;

	default:
		USBD_CtlError(pdev, req);
		break;
	}
	return USBD_OK;
}

static uint8_t dfu_ep0_rx_ready(USBD_HandleTypeDef *pdev)
{
	(void)pdev;
	if (dfu.state == dfuIDLE || dfu.state == dfuDNLOAD_IDLE)
		dfu.state = dfuDNLOAD_SYNC;   /* data in dfu.buf, written on GETSTATUS */
	return USBD_OK;
}

USBD_ClassTypeDef USBD_DFU = {
	.Init = dfu_init,
	.DeInit = dfu_deinit,
	.Setup = dfu_setup,
	.EP0_RxReady = dfu_ep0_rx_ready,
	.GetHSConfigDescriptor = get_cfg_desc,
	.GetFSConfigDescriptor = get_cfg_desc,
	.GetOtherSpeedConfigDescriptor = get_cfg_desc,
};

int dfu_poll(USBD_HandleTypeDef *pdev)
{
	(void)pdev;
	switch (dfu.work) {
	case WORK_WRITE:
		do_write();
		if (dfu.work == WORK_WRITE) dfu.work = WORK_NONE;
		break;
	case WORK_MANIFEST:
		do_manifest();
		if (dfu.work == WORK_MANIFEST) dfu.work = WORK_NONE;
		break;
	default:
		break;
	}
	return dfu.reboot;
}
