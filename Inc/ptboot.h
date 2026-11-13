/*
 * ptboot.h - memory layout shared by the USB DFU bootloader and the application.
 *
 *   Flash 0x08000000 +32K  bootloader (sectors 0-1)           bootloader/
 *   Flash 0x08008000 +480K application (sectors 2-7)           main.bin (BOOTLOADER=1)
 *   RAM   0x20000000 +124K normal RAM (both images)
 *   RAM   0x2001F000 +4K   "no-init" block: survives a warm reset (not power-off).
 *                          Carries the DFU request, boot-loop guard, crash record
 *                          and the USB debug log ring from one boot to the next.
 *
 * Both linker scripts end RAM at PT_NOINIT_BASE so neither image ever touches it
 * except through this struct.
 */
#ifndef PTBOOT_H
#define PTBOOT_H

#include <stdint.h>

#define PT_FLASH_BASE        0x08000000u
#define PTBOOT_SIZE          0x00008000u                    /* 32 KB */
#define PTAPP_BASE           (PT_FLASH_BASE + PTBOOT_SIZE)  /* 0x08008000 */
#define PTAPP_MAX_SIZE       (512u * 1024u - PTBOOT_SIZE)   /* 480 KB */

#define PT_RAM_BASE          0x20000000u
#define PT_RAM_END           0x20020000u
#define PT_NOINIT_BASE       0x2001F000u
#define PT_NOINIT_SIZE       0x00001000u

#define PT_NOINIT_MAGIC      0x50544E49u  /* 'PTNI' - block contents are valid */
#define PTBOOT_REQ_DFU       0xDF00B007u  /* app -> bootloader: stay in DFU mode */
#define PTBOOT_BOOTLOOP_MAX  3u           /* failed app starts before forcing DFU */

#define PT_FAULT_MAGIC       0xFA017EADu
#define PT_LOG_SIZE          3072u

typedef enum {
	PT_BOARD_PT1,
	PT_BOARD_PT2
} pt_board_t;

typedef struct {
	uint32_t magic;        /* PT_FAULT_MAGIC when valid                       */
	uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;  /* stacked exception frame    */
	uint32_t cfsr, hfsr, mmfar, bfar;            /* SCB fault status registers */
	uint32_t exc_return;   /* LR on handler entry                             */
	uint32_t uptime_ms;    /* HAL tick at the time of the fault               */
	uint32_t count;        /* faults since power-on                           */
} pt_fault_t;              /* 18 words = 72 bytes */

typedef struct {
	uint32_t magic;        /* PT_NOINIT_MAGIC                                 */
	uint32_t magic_inv;    /* ~PT_NOINIT_MAGIC                                */
	uint32_t boot_request; /* PTBOOT_REQ_DFU or 0                             */
	uint32_t boot_attempts;/* incremented by bootloader, cleared by stable app */
	uint32_t boot_count;   /* app starts since power-on                       */
	uint32_t reset_flags;  /* RCC->CSR as seen by the bootloader / app        */
	uint32_t bootloader_version;
	uint32_t reserved[9];
	pt_fault_t fault;
	uint32_t log_head;     /* total bytes ever written (monotonic)            */
	uint32_t log_read;     /* total bytes already handed to the host          */
	char     log[PT_LOG_SIZE];
} pt_noinit_t;

#define PT_NOINIT ((volatile pt_noinit_t *)PT_NOINIT_BASE)

_Static_assert(sizeof(pt_noinit_t) <= PT_NOINIT_SIZE, "noinit block too large");

/* Prepare the no-init block after a power-on (contents random). Returns 1 if it was reset. */
static inline int pt_noinit_validate(void)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	if (ni->magic == PT_NOINIT_MAGIC && ni->magic_inv == ~PT_NOINIT_MAGIC)
		return 0;
	uint32_t *p = (uint32_t *)PT_NOINIT_BASE;
	for (unsigned i = 0; i < PT_NOINIT_SIZE / 4; i++)
		p[i] = 0;
	ni->magic = PT_NOINIT_MAGIC;
	ni->magic_inv = ~PT_NOINIT_MAGIC;
	return 1;
}

#endif /* PTBOOT_H */
