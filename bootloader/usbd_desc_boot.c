/* USB descriptors of the PureThermal DFU bootloader (0483:df11). */
#include <string.h>
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_conf.h"

#define BOOT_VID   0x0483
#define BOOT_PID   0xDF11
#define LANGID     0x0409

static uint8_t str_buf[USBD_MAX_STR_DESC_SIZ] __attribute__((aligned(4)));

static uint8_t dev_desc[USB_LEN_DEV_DESC] __attribute__((aligned(4))) = {
	0x12, USB_DESC_TYPE_DEVICE, 0x00, 0x02, 0x00, 0x00, 0x00, USB_MAX_EP0_SIZE,
	BOOT_VID & 0xFF, BOOT_VID >> 8, BOOT_PID & 0xFF, BOOT_PID >> 8,
	0x00, 0x02,                     /* bcdDevice 2.00 */
	USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR, 1
};

static uint8_t langid_desc[4] __attribute__((aligned(4))) = {
	4, USB_DESC_TYPE_STRING, LANGID & 0xFF, LANGID >> 8
};

static uint8_t *d_device(USBD_SpeedTypeDef s, uint16_t *len) { (void)s; *len = sizeof(dev_desc); return dev_desc; }
static uint8_t *d_langid(USBD_SpeedTypeDef s, uint16_t *len) { (void)s; *len = sizeof(langid_desc); return langid_desc; }
static uint8_t *str(const char *s, uint16_t *len) { USBD_GetString((uint8_t *)s, str_buf, len); return str_buf; }
static uint8_t *d_mfr(USBD_SpeedTypeDef s, uint16_t *len) { (void)s; return str("GroupGets", len); }
static uint8_t *d_product(USBD_SpeedTypeDef s, uint16_t *len) { (void)s; return str("PureThermal DFU bootloader", len); }
static uint8_t *d_config(USBD_SpeedTypeDef s, uint16_t *len) { (void)s; return str("DFU", len); }
static uint8_t *d_iface(USBD_SpeedTypeDef s, uint16_t *len) { (void)s; return str("PureThermal application @0x08008000", len); }

/* Same format as the application's serial number, so tools can pair them. */
static uint8_t *d_serial(USBD_SpeedTypeDef s, uint16_t *len)
{
	static const char hex[] = "0123456789abcdef";
	const uint16_t *id = (const uint16_t *)0x1FFF7A10;
	char out[37];
	/* "%04x%04x-%04x-%04x-%04x-%04x00000000" */
	static const uint8_t layout[6] = { 0, 4, 9, 14, 19, 24 };
	(void)s;
	memset(out, '-', sizeof(out));
	for (int w = 0; w < 6; w++)
		for (int n = 0; n < 4; n++)
			out[layout[w] + n] = hex[(id[w] >> (12 - 4 * n)) & 0xF];
	memcpy(out + 28, "00000000", 8);
	out[36] = 0;
	return str(out, len);
}

USBD_DescriptorsTypeDef DFU_Desc = {
	d_device, d_langid, d_mfr, d_product, d_serial, d_config, d_iface,
#if (USBD_LPM_ENABLED == 1)
	NULL,
#endif
};
