# Contributors

## PureThermal reference firmware (GroupGets / GetLab)

The original firmware this fork is based on, in order of commits
(from the git history of [groupgets/purethermal1-firmware](https://github.com/groupgets/purethermal1-firmware)):

- Kurt Kiefer
- Sheyne Anderson
- Pure Engineering (incl. Sashi Ono)
- Griffin Covert
- Matthew Navarro
- Gregor Mittag
- faa
- Regi

## This fork: USB DFU bootloader + USB debug channel

See [README-usb-dfu-debug.md](README-usb-dfu-debug.md).

- [XylenC4](https://github.com/XylenC4): maintainer, hardware bring-up and testing
  on a PureThermal 1 clone with a Lepton 3.5
- Claude (Anthropic): AI pair programmer; co-authored the bootloader, the
  debug channel, `scripts/ptdbg.py` and the fixes in this fork

FLIR Lepton SDK sources are provided by Teledyne FLIR; STM32 HAL and USB
middleware by STMicroelectronics (see the license headers in `Drivers/` and
`Middlewares/`).
