# PureThermal Firmware: USB DFU Bootloader + USB Debug

A fork of the [GroupGets PureThermal reference firmware](https://github.com/groupgets/purethermal1-firmware)
(1.3.1) for the PureThermal 1/2/Mini boards with a FLIR Lepton. It adds:

- **Firmware updates over USB on every board, including the PureThermal 1.** A small USB DFU
  bootloader replaces the STM32 ROM bootloader, which cannot do USB DFU with the PT1's crystal.
- **A USB debug channel** with no extra driver or cable: firmware log, counters, crash record,
  memory peek, reboot and "enter DFU", read with [`scripts/ptdbg.py`](scripts/ptdbg.py).
- **Support for button-less PT1 clones.** The power-button task is off by default; on clones
  it cut the Lepton power after 4 s.

The camera still works as a normal UVC webcam (`1e4e:0100`) with any viewer.

> **Warning:** In this fork `main.bin` is linked for **0x08008000**, behind the bootloader.
> Never write it to 0x08000000. The flashing guides for the original firmware, including
> `scripts/flash.sh` and the `.bat` files, assume 0x08000000. Use them only with a standalone
> build (`BOOTLOADER=0`, see [below](#standalone-build-original-layout)).

## Contents

- [What's different from upstream](#whats-different-from-upstream)
- [Status](#status)
- [Quick start](#quick-start)
- [Building](#building)
- [Updating and debugging over USB](#updating-and-debugging-over-usb)
- [Recovery](#recovery)
- [Standalone build (original layout)](#standalone-build-original-layout)
- [Debugging over SWD / UART](#debugging-over-swd--uart)
- [Important note (from GroupGets)](#important-note-from-groupgets)
- [Contributors](#contributors)

## What's different from upstream

| Area | Upstream 1.3.1 | This fork |
|---|---|---|
| Flash layout | App at 0x08000000 | Bootloader at 0x08000000 (32 KB), app at **0x08008000** (480 KB) |
| USB update | PT2/Mini only (ROM DFU via BOOT button) | **All boards**, started by software (`ptdbg.py flash`), no button needed |
| Failed update | Board bricked until reflashed | Reset vector is written last; an interrupted update stays in DFU mode |
| Boot-loop guard | none | 3 starts without 5 s of stable running → stays in DFU |
| Debug output | Semihosting (SWD) or UART | Also over **USB**: log ring (3 KB, survives reset), counters, crash record |
| PT1 button task | On | Off (`PT1_BUTTON_TASK=1` re-enables it) |
| Extension unit `bmControls` | Mask truncated to 8 bits | Fixed |
| Toolchain | Older GCC | Also builds with **GCC 14** |
| `make flash` / `make debug` | Writes to 0x08000000 | Writes to the app address of the build (0x08008000 by default) |

Details: [README-usb-dfu-debug.md](README-usb-dfu-debug.md) (flash layout, boot rules, debug
controls) and [`Inc/pt_debug.h`](Inc/pt_debug.h) (extension unit 0xFE, selectors 7-13).

## Status

Tested on a **PureThermal 1 clone** (STM32F411CE, 14.318 MHz crystal, no buttons) with a
**Lepton 3.5**:

| Item | Status |
|---|---|
| Bootloader + app, PT1 clock setup, Lepton streaming | ✅ Linux host (flashed via SWD and USB) |
| Update over USB (`ptdbg.py flash`) | ✅ Linux host, several times |
| `ptdbg.py` info / log / counters / peek / reset | ✅ Linux host |
| UVC streaming on Windows (WinUSB + [Lepton-GetThermal](https://github.com/XylenC4/Lepton-GetThermal)) | ✅ |
| `ptdbg.py` on Windows | not tested yet |
| PT2 / Mini boards (8 MHz crystal) | Bootloader clock code included, **not tested on hardware** |
| Crash record after a real HardFault | not triggered yet |

## Quick start

You need an **ST-Link** (SWD) **once**, to install the bootloader. All later updates go over USB.

```bash
# 1. Build (see "Building" for the toolchain)
make full                                         # main.bin + full.bin (bootloader + app)

# 2. Back up the firmware that is on the board now. Keep this file!
st-flash read backup.bin 0x08000000 0x80000

# 3. Install bootloader + app with the ST-Link
make flash-full                                   # = st-flash --reset write full.bin 0x08000000

# 4. From now on: update over USB, no ST-Link needed
scripts/ptdbg.py info
scripts/ptdbg.py flash main.bin
```

On Windows, install the WinUSB driver once before using `ptdbg.py` (see
[Windows](#windows)).

## Building

This is a Makefile project using the GCC ARM Embedded toolchain (`arm-none-eabi-gcc`).

**Toolchain**

| Platform | Install |
|---|---|
| Debian / Ubuntu | `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi stlink-tools python3-usb` |
| macOS | `brew install --cask gcc-arm-embedded` and `brew install stlink` |
| Windows | [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (`arm-none-eabi`), `make` (e.g. MSYS2 or WSL), [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html) for the ST-Link step |

**Build**

```bash
git clone https://github.com/XylenC4/purethermal1-firmware
cd purethermal1-firmware
make            # main.bin  - application for 0x08008000 (USB updates)
make full       # full.bin  - bootloader + application for 0x08000000 (ST-Link, once)
```

You can customize the build in `Inc/project_config.h`, and with these options
(`make OPTION=value`):

| Option | Default | Effect |
|---|---|---|
| `BOOTLOADER` | `1` | `1`: link the app behind the bootloader (0x08008000). `0`: classic standalone image at 0x08000000 |
| `USB_DEBUG` | `1` | `DEBUG_PRINTF` output goes to the USB log ring (read with `ptdbg.py log`) |
| `PT1_BUTTON_TASK` | `0` | `1`: re-enable the PT1 power-button task (only for boards that have the button) |
| `USART_DEBUG` | off | `1`: printf over UART (`USART_DEBUG_SPEED`, default 115200) |
| `GDB_SEMIHOSTING` | off | `1`: printf over SWD via GDB semihosting |

The firmware reports its version and the short git commit it was built from (`ptdbg.py info`,
e.g. `1.3.1-dfu (5e67)`).

## Updating and debugging over USB

`scripts/ptdbg.py` (Python 3 + pyusb) talks to the running firmware through the vendor UVC
extension unit, and to the bootloader (`0483:df11`) during updates:

```bash
scripts/ptdbg.py info                    # firmware, board, crystal, uptime, reset cause
scripts/ptdbg.py log -f                  # live firmware log
scripts/ptdbg.py counters                # frame / resync / recovery counters
scripts/ptdbg.py fault --elf main.out    # last crash: registers + source line
scripts/ptdbg.py peek g_dbg --elf main.out   # read memory (flash, RAM, peripherals)
scripts/ptdbg.py reset                   # reboot the camera
scripts/ptdbg.py dfu                     # reboot into the bootloader
scripts/ptdbg.py flash main.bin          # update: DFU -> write -> verify -> restart
scripts/ptdbg.py backup app.bin          # read the application region
```

`dfu-util -d 0483:df11 -D main.bin -R` also works while the bootloader is active.
`fault --elf` and `peek SYMBOL` need `arm-none-eabi-addr2line` / `nm` on the PATH; numeric
addresses work without them.

**When the bootloader stays in DFU mode** (USB `0483:df11`, LED blinks fast):

- the application asked for it (`ptdbg.py dfu`), or
- there is no valid application (for example after an interrupted update), or
- the application crashed or reset 3 times before running for 5 s.

In all three cases, flash a working `main.bin` with `ptdbg.py flash`. There's no rush, because
the board stays in DFU mode until then.

### Windows

The camera works as a normal webcam out of the box. `ptdbg.py` and libusb-based viewers need
the **WinUSB** driver instead:

1. Install [Zadig](https://zadig.akeo.ie), plug in the camera.
2. *Options → List All Devices*, untick *Ignore Hubs or Composite Parents*.
3. Select **PureThermal (Composite Parent)** (`1E4E 0100`), choose **WinUSB**, *Replace Driver*.
4. Run `scripts/ptdbg.py dfu` once. The camera re-appears as **PureThermal DFU bootloader**
   (`0483 DF11`); bind that to **WinUSB** as well.
5. `py -m pip install pyusb libusb-package`

WinUSB lets only one program open the camera at a time, so close the viewer before running
`ptdbg.py`. To get the webcam driver back: Device Manager → device → *Uninstall device*, tick
*Delete the driver*, replug.

## Recovery

With an ST-Link you can always recover, whatever state the flash is in:

| Goal | Command (`st-flash`, or STM32CubeProgrammer at the same address) |
|---|---|
| Back to the original firmware | `st-flash --reset write backup.bin 0x08000000` (your backup from the quick start) |
| Reinstall this fork | `make flash-full`, or `st-flash --reset write full.bin 0x08000000` |
| Upstream release | `st-flash --reset write pt1-vX.X.X.bin 0x08000000` from the [GroupGets releases](https://github.com/groupgets/purethermal1-firmware/releases) |

Never write `main.bin` (built with `BOOTLOADER=1`) to 0x08000000.

## Standalone build (original layout)

`make BOOTLOADER=0` builds the classic image for 0x08000000, without the bootloader and the
USB update path. The USB debug channel stays available (`USB_DEBUG=1`). All flashing methods
of the original firmware work with this image:

- **ST-Link:** `make BOOTLOADER=0 flash` (OpenOCD), or `st-flash --reset write main.bin 0x08000000`.
- **ROM DFU (PT2 / Mini only):** hold BOOT, press and release RST, release BOOT (the LED dims),
  then `dfu-util -a 0 -D main.bin -s 0x08000000` or `scripts/flash.sh`. On Windows use
  `scripts/flash_dfu.bat` with the [win32 DFU tools](https://files.groupgets.com/purethermal/win32_dfu.zip).

The PureThermal 1 has no working ROM USB DFU (its 14.318 MHz crystal isn't a whole MHz), so it
needs the ST-Link for standalone images. That limitation is why this fork adds its own
bootloader.

## Debugging over SWD / UART

The upstream debugging options are unchanged and work alongside the USB debug channel:

- **Semihosting** (printf over SWD with an ST-Link or J-Link): `make GDB_SEMIHOSTING=1 debug`.
  This flashes, starts the target and keeps OpenOCD attached; printf output appears in the
  console. Run `make clean` first if you switch options. In Eclipse/OpenSTM32 projects, add
  `monitor arm semihosting enable` to the debug configuration's initialization commands.
- **UART:** `make USART_DEBUG=1`.
- **Breakpoints:** GDB + OpenOCD with `PureThermal1.cfg`, or the included Eclipse project.

More background: the upstream [PureThermal 1 development wiki](https://github.com/groupgets/purethermal1-firmware/wiki).

## Important note (from GroupGets)

GroupGets does not provide coding, firmware, or software support of any kind and will not respond to related requests. All software and firmware provided by GroupGets are offered solely as examples or potential starting points. These repositories may be outdated and are not guaranteed to function as intended. Forking this project and making your own modifications to this firmware is encouraged.

We do not accept returns or offer replacements due to issues related to software, firmware, or code compatibility.

The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose and non-infringement. In no event shall the authors or copyright holders be liable for any claim, damages, or other liability, whether in an action of contract, tort, or otherwise, arising from, out of, or in connection with the software or the use or other dealings in the software. Additionally Teledyne FLIR-provided source code is being released by GroupGets to this project under the
express written consent of Teledyne FLIR, and covered by the project's license, with the expectation that no warranties or support of any kind will be provided by Teledyne FLIR. This firmware is licensed under The MIT License (MIT), see `LICENSE` file for details.

This fork is not affiliated with or supported by GroupGets or Teledyne FLIR. Use it at your
own risk, and keep a backup of your original firmware.

## Contributors

The original firmware is by GroupGets / GetLab and Pure Engineering. This fork is by
[XylenC4](https://github.com/XylenC4) together with Claude (Anthropic). See
[CONTRIBUTORS.md](CONTRIBUTORS.md).
