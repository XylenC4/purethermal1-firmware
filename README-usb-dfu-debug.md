# PureThermal firmware: USB DFU bootloader + USB debug channel

Branch `usb-dfu-debug`, based on GroupGets firmware 1.3.1.

## Why a custom bootloader
The STM32F411's ROM bootloader detects the crystal automatically and needs a whole-MHz value for USB DFU.
The PureThermal 1 uses a 14.318182 MHz crystal, so ROM DFU doesn't work on PT1 boards.
`bootloader/` (32 KB at 0x08000000) brings up the PLL exactly like the application does,
for both PT1 (14.318 MHz) and PT2 (8 MHz) boards, and implements plain USB DFU 1.1.

## Flash layout (Inc/ptboot.h)
| Region | Contents |
|---|---|
| 0x08000000 32K | bootloader. It can never be written over USB. |
| 0x08008000 480K | application (`main.bin`, built with `BOOTLOADER=1`, the default) |
| RAM 0x2001F000 4K | no-init block: DFU request, boot-loop guard, crash record, log ring |

The bootloader stays in DFU mode (USB `0483:df11`, LED blinks fast) when:
* the application requested it (`ptdbg.py dfu`), or
* there is no valid application (for example, an interrupted update), or
* the application crashed or reset 3 times before it had run 5 s.

Otherwise it jumps straight to the application.

## Changes to the application
* `PT_SCHEDULE(button_task(...))` is disabled. On clones without a button, PA4 reads low and the PT1 button
  task cuts the Lepton power after 4 s. Re-enable it with `make PT1_BUTTON_TASK=1`.
* New controls on the vendor extension unit 0xFE (selectors 7-13). See `Inc/pt_debug.h`:
  info, log ring (all `DEBUG_PRINTF` output, `USB_DEBUG=1` by default), `g_dbg` counters, crash record
  (HardFault/BusFault/... registers survive the reset), memory peek, reboot, enter DFU.
* Fixed the extension-unit `bmControls` mask, which was truncated to 8 bits.
* Builds with GCC 14: added two missing includes and one pointer cast.

## Build
```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi stlink-tools python3-usb
make            # main.bin (app @0x08008000)
make full       # full.bin = bootloader + app, for the one-time ST-Link flash
```

## One-time install (ST-Link)
```bash
st-flash read  backup.bin 0x08000000 0x80000      # back up first!
make flash-full                                     # = st-flash --reset write full.bin 0x08000000
```
To go back to the original firmware: `st-flash --reset write backup.bin 0x08000000`.

## After that, everything goes over USB
```bash
scripts/ptdbg.py info              # firmware, board, crystal, uptime, reset cause
scripts/ptdbg.py log -f            # live firmware log
scripts/ptdbg.py counters          # frame / resync / recovery counters
scripts/ptdbg.py fault --elf main.out   # last crash: registers + source line
scripts/ptdbg.py peek g_dbg --elf main.out
scripts/ptdbg.py flash main.bin    # update: reboot to DFU, write, verify, restart
scripts/ptdbg.py backup app.bin    # read the application region
```
`dfu-util -d 0483:df11 -D main.bin -R` works as well.
Windows: bind both `1e4e:0100` and `0483:df11` to WinUSB with Zadig once, then `pip install pyusb libusb-package`.

## Contributors
See [CONTRIBUTORS.md](CONTRIBUTORS.md). This fork was developed by XylenC4 together with Claude (Anthropic).
