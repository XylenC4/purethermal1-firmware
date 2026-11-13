/*
 * pt_debug.c - USB debug channel + DFU trigger (see Inc/pt_debug.h).
 */
#include <stdint.h>
#include <string.h>
#include "stm32f4xx_hal.h"
#include "usb_device.h"
#include "usbd_uvc.h"
#include "ptboot.h"
#include "pt_debug.h"
#include "dbg_counters.h"
#include "version.h"

extern pt_board_t pt_board;
extern unsigned int pt_hse_value;
extern USBD_HandleTypeDef hUsbDeviceFS;

static uint32_t peek_addr = PT_FLASH_BASE;
static volatile uint32_t pending_request;
static volatile uint32_t pending_since;
static uint8_t stable_cleared;
static uint8_t log_lost;

static int have_bootloader(void)
{
	return SCB->VTOR == PTAPP_BASE;
}

/* ------------------------------------------------------------------ */
/* log ring                                                            */
/* ------------------------------------------------------------------ */

int pt_debug_log_write(const char *buf, int len)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	uint32_t head = ni->log_head;
	for (int i = 0; i < len; i++)
		ni->log[(head + i) % PT_LOG_SIZE] = buf[i];
	ni->log_head = head + len;
	return len;
}

#if defined(USB_DEBUG) && !defined(USART_DEBUG)
/* newlib stdout -> USB log ring (USART_DEBUG builds tee in main.c's _write). */
int _write(int file, char *ptr, int len)
{
	(void)file;
	return pt_debug_log_write(ptr, len);
}
#endif

static uint16_t log_read_chunk(uint8_t *out, uint16_t max)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	uint32_t head = ni->log_head;
	uint32_t rd = ni->log_read;
	if (head - rd > PT_LOG_SIZE) {         /* writer lapped us */
		rd = head - PT_LOG_SIZE;
		log_lost = 1;
	}
	uint16_t n = 0;
	while (rd != head && n < max)
		out[n++] = ni->log[rd++ % PT_LOG_SIZE];
	ni->log_read = rd;
	return n;
}

/* ------------------------------------------------------------------ */
/* init / poll                                                         */
/* ------------------------------------------------------------------ */

void pt_debug_init(void)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	pt_noinit_validate();
	ni->boot_count++;
	if (!have_bootloader()) {
		/* no bootloader captured the reset cause for us */
		ni->reset_flags = RCC->CSR;
		ni->bootloader_version = 0;
		RCC->CSR |= RCC_CSR_RMVF;
	}
	ni->boot_request = 0;
}

void pt_debug_poll(void)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	uint32_t now = HAL_GetTick();

	/* Running for 5 s without a crash: the boot-loop guard can stand down. */
	if (!stable_cleared && now > 5000) {
		ni->boot_attempts = 0;
		stable_cleared = 1;
	}

	/* Deferred reboot: give the host time to finish the control transfer. */
	if (pending_request && (now - pending_since) > 50) {
		uint32_t req = pending_request;
		pending_request = 0;
		ni->boot_request = (req == PTBOOT_REQ_DFU) ? PTBOOT_REQ_DFU : 0;
		USBD_Stop(&hUsbDeviceFS);   /* drop off the bus so the host re-enumerates */
		HAL_Delay(20);
		__disable_irq();
		NVIC_SystemReset();
	}
}

/* ------------------------------------------------------------------ */
/* crash capture (called from HardFault_Handler)                       */
/* ------------------------------------------------------------------ */

void pt_fault_capture(uint32_t *frame, uint32_t exc_return)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	volatile pt_fault_t *f = &ni->fault;
	uint32_t count = (f->magic == PT_FAULT_MAGIC) ? f->count : 0;
	f->r0 = frame[0]; f->r1 = frame[1]; f->r2 = frame[2]; f->r3 = frame[3];
	f->r12 = frame[4]; f->lr = frame[5]; f->pc = frame[6]; f->xpsr = frame[7];
	f->cfsr = SCB->CFSR; f->hfsr = SCB->HFSR;
	f->mmfar = SCB->MMFAR; f->bfar = SCB->BFAR;
	f->exc_return = exc_return;
	f->uptime_ms = HAL_GetTick();
	f->count = count + 1;
	f->magic = PT_FAULT_MAGIC;
	NVIC_SystemReset();
}

/* ------------------------------------------------------------------ */
/* memory peek                                                         */
/* ------------------------------------------------------------------ */

/* Readable regions [start, end). A peek may start anywhere inside one and is
 * zero-padded past its end, so small regions (UID, option bytes) can be read. */
static const uint32_t peek_regions[][2] = {
	{ 0x08000000u, 0x08080000u },   /* flash                         */
	{ 0x20000000u, 0x20020000u },   /* SRAM                          */
	{ 0x1FFF0000u, 0x1FFF7A24u },   /* system memory, OTP, UID, size */
	{ 0x1FFFC000u, 0x1FFFC010u },   /* option bytes                  */
	{ 0x40000000u, 0x40008000u },   /* APB1                          */
	{ 0x40010000u, 0x40015800u },   /* APB2                          */
	{ 0x40020000u, 0x40026800u },   /* AHB1 (GPIO, RCC, DMA)         */
	{ 0xE000E000u, 0xE000F000u },   /* SCS (NVIC, SCB)               */
};

/* End of the readable region containing addr, or 0 if addr is not readable. */
static uint32_t peek_region_end(uint32_t addr)
{
	for (unsigned i = 0; i < sizeof(peek_regions) / sizeof(peek_regions[0]); i++)
		if (addr >= peek_regions[i][0] && addr < peek_regions[i][1])
			return peek_regions[i][1];
	return 0;
}

/* ------------------------------------------------------------------ */
/* extension-unit controls                                             */
/* ------------------------------------------------------------------ */

static uint16_t control_len(uint8_t cs)
{
	switch (cs - 1) {
	case CUST_CONTROL_DBG_INFO:      return PT_DBG_INFO_LEN;
	case CUST_CONTROL_DBG_LOG:       return PT_DBG_LOG_LEN;
	case CUST_CONTROL_DBG_COUNTERS:  return PT_DBG_COUNTERS_LEN;
	case CUST_CONTROL_DBG_FAULT:     return PT_DBG_FAULT_LEN;
	case CUST_CONTROL_DBG_PEEK_ADDR: return PT_DBG_PEEK_ADDR_LEN;
	case CUST_CONTROL_DBG_PEEK:      return PT_DBG_PEEK_LEN;
	case CUST_CONTROL_DBG_BOOT:      return PT_DBG_BOOT_LEN;
	default:                         return 0;
	}
}

static void fill_info(pt_dbg_info_t *info)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	memset(info, 0, sizeof(*info));
	info->magic = PT_DBG_INFO_MAGIC;
	info->proto = PT_DBG_PROTO_VERSION;
	info->board = (pt_board == PT_BOARD_PT1) ? 1 : 2;
	info->flags = (have_bootloader() ? PT_DBG_FLAG_BOOTLOADER : 0)
#ifdef USB_DEBUG
	            | PT_DBG_FLAG_USB_LOG
#endif
#ifdef USART_DEBUG
	            | PT_DBG_FLAG_UART_LOG
#endif
	            | (ni->fault.magic == PT_FAULT_MAGIC ? PT_DBG_FLAG_FAULT : 0);
	strncpy(info->fw_version, FW_VERSION "-dfu", sizeof(info->fw_version));
	info->hse_hz = pt_hse_value;
	info->sysclk_hz = HAL_RCC_GetSysClockFreq();
	info->uptime_ms = HAL_GetTick();
	info->boot_count = ni->boot_count;
	info->reset_flags = ni->reset_flags;
	info->log_head = ni->log_head;
	info->log_read = ni->log_read;
	info->bootloader_version = have_bootloader() ? ni->bootloader_version : 0;
	info->fault_count = (ni->fault.magic == PT_FAULT_MAGIC) ? ni->fault.count : 0;
	memcpy(info->git_sha, BUILD_GIT_HASH, sizeof(info->git_sha));
}

int pt_debug_xu_get(uint8_t cmd, uint8_t cs, uint8_t *pbuf, uint16_t length, int *status)
{
	uint16_t len = control_len(cs);
	if (len == 0)
		return 0;                       /* not ours */

	*status = 0;
	switch (cmd) {
	case UVC_GET_LEN:
		pbuf[0] = len & 0xFF;
		pbuf[1] = len >> 8;
		return 1;
	case UVC_GET_INFO:
		pbuf[0] = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET;
		return 1;
	case UVC_GET_RES:
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_DEF:
		memset(pbuf, 0, length);
		return 1;
	case UVC_GET_CUR:
		break;
	default:
		*status = -1;
		return 1;
	}

	memset(pbuf, 0, length);
	switch (cs - 1) {
	case CUST_CONTROL_DBG_INFO: {
		pt_dbg_info_t info;
		fill_info(&info);
		memcpy(pbuf, &info, length < sizeof(info) ? length : sizeof(info));
		break;
	}
	case CUST_CONTROL_DBG_LOG:
		if (length < 2) { *status = -1; break; }
		pbuf[0] = (uint8_t)log_read_chunk(pbuf + 2, length - 2);
		pbuf[1] = log_lost;
		log_lost = 0;
		break;
	case CUST_CONTROL_DBG_COUNTERS: {
		uint16_t n = sizeof(g_dbg) < length ? sizeof(g_dbg) : length;
		memcpy(pbuf, (const void *)&g_dbg, n);
		break;
	}
	case CUST_CONTROL_DBG_FAULT: {
		volatile pt_noinit_t *ni = PT_NOINIT;
		uint16_t n = sizeof(pt_fault_t) < length ? sizeof(pt_fault_t) : length;
		memcpy(pbuf, (const void *)&ni->fault, n);
		break;
	}
	case CUST_CONTROL_DBG_PEEK_ADDR:
		memcpy(pbuf, &peek_addr, length < 4 ? length : 4);
		break;
	case CUST_CONTROL_DBG_PEEK:
	{
		uint32_t region_end = peek_region_end(peek_addr);
		if ((peek_addr & 3) || region_end == 0) {
			*status = -1;
			break;
		}
		for (uint16_t i = 0; i + 4 <= length && peek_addr + i + 4 <= region_end; i += 4) {
			uint32_t w = *(volatile uint32_t *)(peek_addr + i);
			memcpy(pbuf + i, &w, 4);
		}
		peek_addr += length;
	}
		break;
	default:
		*status = -1;
		break;
	}
	return 1;
}

int pt_debug_xu_set(uint8_t cmd, uint8_t cs, const uint8_t *pbuf, uint16_t length, int *status)
{
	uint16_t len = control_len(cs);
	if (len == 0)
		return 0;

	*status = -1;
	if (cmd != UVC_SET_CUR || length < 4)
		return 1;

	uint32_t v;
	memcpy(&v, pbuf, 4);
	switch (cs - 1) {
	case CUST_CONTROL_DBG_PEEK_ADDR:
		peek_addr = v & ~3u;
		*status = 0;
		break;
	case CUST_CONTROL_DBG_BOOT:
		if (v == PTBOOT_REQ_DFU && have_bootloader()) {
			pending_since = HAL_GetTick();
			pending_request = v;
			*status = 0;
		} else if (v == PT_DBG_REQ_RESET) {
			pending_since = HAL_GetTick();
			pending_request = v;
			*status = 0;
		} else if (v == PT_DBG_REQ_CLEAR_FAULT) {
			PT_NOINIT->fault.magic = 0;
			PT_NOINIT->fault.count = 0;
			*status = 0;
		}
		break;
	default:
		break;
	}
	return 1;
}

/* Fault handlers: grab the stacked frame (MSP or PSP) and hand it to
 * pt_fault_capture(), which records it in no-init RAM and resets. The
 * record is readable after the reboot via CUST_CONTROL_DBG_FAULT. */
#define PT_FAULT_TRAMPOLINE(name)                         \
	__attribute__((naked)) void name(void)                \
	{                                                     \
		__asm volatile(                                   \
			"tst lr, #4      \n"                          \
			"ite eq          \n"                          \
			"mrseq r0, msp   \n"                          \
			"mrsne r0, psp   \n"                          \
			"mov r1, lr      \n"                          \
			"b pt_fault_capture \n");                     \
	}

PT_FAULT_TRAMPOLINE(HardFault_Handler)
PT_FAULT_TRAMPOLINE(MemManage_Handler)
PT_FAULT_TRAMPOLINE(BusFault_Handler)
PT_FAULT_TRAMPOLINE(UsageFault_Handler)
