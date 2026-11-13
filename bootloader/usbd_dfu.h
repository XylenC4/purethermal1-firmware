/*
 * Minimal USB DFU 1.1 class (plain DFU, not DfuSe) for the PureThermal bootloader.
 *
 * Download writes the image sequentially to PTAPP_BASE (0x08008000); the
 * bootloader's own sectors can never be written over USB. The first 8 bytes
 * of the image (initial SP + reset vector) are held back and only written in
 * the manifestation phase, so an interrupted download leaves no bootable app
 * and the device simply stays in DFU mode.
 *
 * Upload reads the application region (backup).
 *
 *   dfu-util -d 0483:df11 -D main.bin -R      # flash application, then reboot
 *   dfu-util -d 0483:df11 -U backup.bin       # read application region
 */
#ifndef USBD_DFU_H
#define USBD_DFU_H

#include "usbd_def.h"

#define DFU_XFER_SIZE        1024u

extern USBD_ClassTypeDef USBD_DFU;

/* Run deferred flash work. Call from the main loop. Returns 1 when the
 * bootloader should reboot (DETACH / bus reset after a finished download). */
int dfu_poll(USBD_HandleTypeDef *pdev);

#endif
