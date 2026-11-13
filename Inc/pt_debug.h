/*
 * pt_debug.h - debugging and firmware-update control over the existing UVC link.
 *
 * Everything is exposed as extra controls on the vendor extension unit
 * VC_CONTROL_XU_LEP_CUST_ID (0xFE, GUID "pt1-lep-cust0000"), so no extra USB
 * interface or driver is needed: any host that can send UVC extension-unit
 * requests (libusb, Linux UVCIOC_CTRL_QUERY, Windows KS) can use it.
 *
 *  selector  control                 dir  len  meaning
 *  --------  ----------------------  ---  ---  -------------------------------------
 *     7      CUST_CONTROL_DBG_INFO    GET  64  pt_dbg_info_t (version, board, uptime ...)
 *     8      CUST_CONTROL_DBG_LOG     GET  64  [n][flags][n bytes of log text], consumes
 *     9      CUST_CONTROL_DBG_COUNTERS GET 128 struct dbg_counters (g_dbg), zero padded
 *    10      CUST_CONTROL_DBG_FAULT   GET  72  pt_fault_t from the last crash (if any)
 *    11      CUST_CONTROL_DBG_PEEK_ADDR SET/GET 4  address for the next PEEK (4-aligned)
 *    12      CUST_CONTROL_DBG_PEEK    GET  64  64 bytes at PEEK_ADDR, then addr += 64
 *    13      CUST_CONTROL_DBG_BOOT    SET   4  PTBOOT_REQ_DFU -> reboot into USB DFU
 *                                               PT_DBG_REQ_RESET -> plain reboot
 *                                               PT_DBG_REQ_CLEAR_FAULT -> forget crash record
 */
#ifndef PT_DEBUG_H
#define PT_DEBUG_H

#include <stdint.h>

#define PT_DBG_INFO_MAGIC       0x42445450u /* 'PTDB' */
#define PT_DBG_PROTO_VERSION    1u
#define PT_DBG_REQ_RESET        0x52534554u /* 'TESR' little endian "RSET" */
#define PT_DBG_REQ_CLEAR_FAULT  0x434C5246u

#define PT_DBG_INFO_LEN      64u
#define PT_DBG_LOG_LEN       64u
#define PT_DBG_COUNTERS_LEN  128u
#define PT_DBG_FAULT_LEN     72u
#define PT_DBG_PEEK_ADDR_LEN 4u
#define PT_DBG_PEEK_LEN      64u
#define PT_DBG_BOOT_LEN      4u

#define PT_DBG_FLAG_BOOTLOADER 0x01u  /* app runs behind the USB DFU bootloader */
#define PT_DBG_FLAG_USB_LOG    0x02u  /* DEBUG_PRINTF goes to the USB log ring  */
#define PT_DBG_FLAG_FAULT      0x04u  /* a crash record is available           */
#define PT_DBG_FLAG_UART_LOG   0x08u  /* DEBUG_PRINTF also goes to USART2       */

typedef struct __attribute__((packed)) {
	uint32_t magic;            /*  0 PT_DBG_INFO_MAGIC                  */
	uint16_t proto;            /*  4 PT_DBG_PROTO_VERSION               */
	uint8_t  board;            /*  6 1 = PureThermal 1, 2 = PureThermal 2 */
	uint8_t  flags;            /*  7 PT_DBG_FLAG_*                      */
	char     fw_version[16];   /*  8 e.g. "1.3.1-dfu"                   */
	uint32_t hse_hz;           /* 24 crystal frequency in use           */
	uint32_t sysclk_hz;        /* 28                                     */
	uint32_t uptime_ms;        /* 32                                     */
	uint32_t boot_count;       /* 36 app starts since power-on          */
	uint32_t reset_flags;      /* 40 RCC->CSR at boot (reset cause)     */
	uint32_t log_head;         /* 44 bytes ever logged                  */
	uint32_t log_read;         /* 48 bytes already read by the host     */
	uint32_t bootloader_version; /* 52 0 if no bootloader               */
	uint32_t fault_count;      /* 56 crashes since power-on             */
	char     git_sha[4];       /* 60 first 4 chars of the build git sha */
} pt_dbg_info_t;

void pt_debug_init(void);
void pt_debug_poll(void);
int  pt_debug_log_write(const char *buf, int len);

/* Extension-unit hooks. Return 1 if the selector belongs to the debug block
 * (result in *status: 0 ok, -1 stall), 0 to let the normal Lepton path run. */
int pt_debug_xu_get(uint8_t cmd, uint8_t cs, uint8_t *pbuf, uint16_t length, int *status);
int pt_debug_xu_set(uint8_t cmd, uint8_t cs, const uint8_t *pbuf, uint16_t length, int *status);

#endif /* PT_DEBUG_H */
