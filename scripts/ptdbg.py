#!/usr/bin/env python3
"""
ptdbg - debug and update a PureThermal running the USB-DFU-enabled firmware.

Talks to the vendor UVC extension unit (0xFE) of the running firmware and to the
USB DFU bootloader (0483:df11). Needs pyusb + libusb
(Linux: python3-usb; Windows: pip install pyusb libusb-package, and bind the
camera 1e4e:0100 and the bootloader 0483:df11 to WinUSB with Zadig once).

  ptdbg.py info                     firmware/board/uptime/reset cause
  ptdbg.py log [-f]                 read the firmware log (-f: follow)
  ptdbg.py counters                 frame/resync/recovery counters (g_dbg)
  ptdbg.py fault [--clear]          last crash record (registers at HardFault)
  ptdbg.py peek ADDR|SYMBOL [LEN] [--elf main.out]
  ptdbg.py reset                    reboot the camera
  ptdbg.py dfu                      reboot into the USB DFU bootloader
  ptdbg.py flash main.bin           update the application over USB
  ptdbg.py backup app-backup.bin    read the application region over USB
"""
import argparse
import struct
import subprocess
import sys
import time

import usb.core
import usb.util

# Windows: pyusb needs a libusb DLL; the "libusb-package" wheel ships one.
try:
    import libusb_package
    _BACKEND = libusb_package.get_libusb1_backend()
except ImportError:
    _BACKEND = None

APP_VID, APP_PID = 0x1E4E, 0x0100
DFU_VID, DFU_PID = 0x0483, 0xDF11
XU_ID = 0xFE
VC_IFACE = 0

# extension-unit selectors (Inc/pt_debug.h)
CS_INFO, CS_LOG, CS_COUNTERS, CS_FAULT, CS_PEEK_ADDR, CS_PEEK, CS_BOOT = range(7, 14)
LEN = {CS_INFO: 64, CS_LOG: 64, CS_COUNTERS: 128, CS_FAULT: 64,
       CS_PEEK_ADDR: 4, CS_PEEK: 64, CS_BOOT: 4}

REQ_DFU = 0xDF00B007
REQ_RESET = 0x52534554
REQ_CLEAR_FAULT = 0x434C5246

UVC_SET_CUR, UVC_GET_CUR = 0x01, 0x81

DFU_DNLOAD, DFU_UPLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_GETSTATE, DFU_ABORT, DFU_DETACH = 1, 2, 3, 4, 5, 6, 0
DFU_STATES = ["appIDLE", "appDETACH", "dfuIDLE", "dfuDNLOAD-SYNC", "dfuDNBUSY", "dfuDNLOAD-IDLE",
              "dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET", "dfuUPLOAD-IDLE", "dfuERROR"]
DFU_XFER = 1024

COUNTER_NAMES = ["magic", "vsync_irqs", "transfers_done", "transfer_fails", "frames_completed",
                 "frames_dropped", "desync_events", "first_line_bad", "worst_desync_run",
                 "resync_entries", "resync_packets", "resync_giveups", "vsync_cfg_fails",
                 "hard_recoveries", "wedge_recoveries", "last_recovery_firings", "cs_stuck_low"]

RESET_FLAGS = [(31, "LPWR"), (30, "WWDG"), (29, "IWDG"), (28, "SOFTWARE"), (27, "POWER-ON"),
               (26, "PIN"), (25, "BROWN-OUT")]


def die(msg):
    print("error:", msg, file=sys.stderr)
    sys.exit(1)


def find(vid, pid, timeout=0.0):
    end = time.time() + timeout
    while True:
        dev = usb.core.find(idVendor=vid, idProduct=pid, backend=_BACKEND)
        if dev is not None or time.time() >= end:
            return dev
        time.sleep(0.2)


class App:
    """Running firmware: UVC extension-unit access on the VideoControl interface."""

    def __init__(self):
        self.dev = find(APP_VID, APP_PID, 5.0)
        if self.dev is None:
            die("PureThermal (1e4e:0100) not found")
        self.detached = False
        try:
            if self.dev.is_kernel_driver_active(VC_IFACE):
                self.dev.detach_kernel_driver(VC_IFACE)   # uvcvideo, re-attached on close
                self.detached = True
        except (NotImplementedError, usb.core.USBError):
            pass
        try:
            usb.util.claim_interface(self.dev, VC_IFACE)
        except usb.core.USBError:
            pass  # Windows/WinUSB: control transfers work without an explicit claim

    def close(self):
        try:
            usb.util.release_interface(self.dev, VC_IFACE)
            if self.detached:
                self.dev.attach_kernel_driver(VC_IFACE)
        except usb.core.USBError:
            pass
        usb.util.dispose_resources(self.dev)

    def get(self, cs):
        try:
            return bytes(self.dev.ctrl_transfer(0xA1, UVC_GET_CUR, cs << 8, (XU_ID << 8) | VC_IFACE,
                                                LEN[cs], timeout=1000))
        except usb.core.USBError as e:
            if cs == CS_INFO:
                die(f"debug controls not answered ({e}) - is the USB-debug firmware installed?")
            raise

    def set(self, cs, value):
        self.dev.ctrl_transfer(0x21, UVC_SET_CUR, cs << 8, (XU_ID << 8) | VC_IFACE,
                               struct.pack("<I", value), timeout=1000)


def decode_info(b):
    (magic, proto, board, flags, fw, hse, sysclk, uptime, boots, rflags, lhead, lread,
     blver, faults, sha) = struct.unpack("<IHBB16sIIIIIIIII4s", b[:64])
    if magic != 0x42445450:
        die(f"unexpected info magic 0x{magic:08x}")
    causes = [n for bit, n in RESET_FLAGS if rflags & (1 << bit)] or ["?"]
    return {
        "firmware": fw.rstrip(b"\0").decode(errors="replace") + f" ({sha.decode(errors='replace')})",
        "board": f"PureThermal {board}",
        "bootloader": f"v{blver >> 16}.{(blver >> 8) & 0xFF}.{blver & 0xFF}" if flags & 1 else "none (standalone image)",
        "crystal": f"{hse / 1e6:.6f} MHz",
        "sysclk": f"{sysclk / 1e6:.1f} MHz",
        "uptime": f"{uptime / 1000:.1f} s",
        "boots since power-on": boots,
        "last reset cause": f"0x{rflags:08x} " + "+".join(causes),
        "log": f"{lhead} bytes written, {lhead - lread} unread" + (" (USB log on)" if flags & 2 else " (USB log OFF)"),
        "crashes since power-on": faults,
        "crash record": "YES - run 'ptdbg.py fault'" if flags & 4 else "none",
    }


def cmd_info(a):
    app = App()
    try:
        for k, v in decode_info(app.get(CS_INFO)).items():
            print(f"{k:24s} {v}")
    finally:
        app.close()


def cmd_log(a):
    app = App()
    try:
        while True:
            got = False
            while True:
                b = app.get(CS_LOG)
                n, lost = b[0], b[1]
                if lost:
                    sys.stdout.write("\n[... log overflowed, older text lost ...]\n")
                if n == 0:
                    break
                got = True
                sys.stdout.write(b[2:2 + n].decode(errors="replace"))
            sys.stdout.flush()
            if not a.follow:
                break
            if not got:
                time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        app.close()


def cmd_counters(a):
    app = App()
    try:
        b = app.get(CS_COUNTERS)
    finally:
        app.close()
    vals = struct.unpack("<%dI" % len(COUNTER_NAMES), b[:4 * len(COUNTER_NAMES)])
    if vals[0] != 0xDBC0FFEE:
        print(f"warning: counter magic 0x{vals[0]:08x} (expected 0xdbc0ffee)")
    for name, v in zip(COUNTER_NAMES[1:], vals[1:]):
        print(f"{name:24s} {v}")


def cmd_fault(a):
    app = App()
    try:
        if a.clear:
            app.set(CS_BOOT, REQ_CLEAR_FAULT)
            print("crash record cleared")
            return
        b = app.get(CS_FAULT)
    finally:
        app.close()
    f = struct.unpack("<16I", b[:64])
    if f[0] != 0xFA017EAD:
        print("no crash recorded since power-on")
        return
    names = ["r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr", "cfsr", "hfsr", "mmfar", "bfar",
             "exc_return", "uptime_ms", "count"]
    for n, v in zip(names, f[1:]):
        print(f"{n:12s} 0x{v:08x}" + (f"  ({v})" if n in ("uptime_ms", "count") else ""))
    if a.elf:
        for reg, v in (("pc", f[7]), ("lr", f[6])):
            print(f"{reg} -> " + addr2line(a.elf, v & ~1))


def addr2line(elf, addr):
    try:
        out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-C", "-e", elf, hex(addr)],
                             capture_output=True, text=True, check=True).stdout.split("\n")
        return f"{out[0]} at {out[1]}"
    except (OSError, subprocess.CalledProcessError):
        return "(arm-none-eabi-addr2line not available)"


def resolve(sym, elf):
    try:
        return int(sym, 0), None
    except ValueError:
        pass
    if not elf:
        die("symbol names need --elf main.out")
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if p[-1] == sym:
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else None)
    die(f"symbol {sym} not found in {elf}")


def cmd_peek(a):
    addr, size = resolve(a.addr, a.elf)
    length = a.length or size or 64
    start = addr & ~3
    app = App()
    data = b""
    try:
        app.set(CS_PEEK_ADDR, start)
        while len(data) < (addr - start) + length:
            data += app.get(CS_PEEK)
    except usb.core.USBError:
        die(f"address 0x{start:08x} not readable (allowed: flash, SRAM, sysmem/OTP, peripherals, SCS)")
    finally:
        app.close()
    data = data[addr - start: addr - start + length]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        words = " ".join(f"{struct.unpack('<I', chunk[j:j + 4])[0]:08x}" for j in range(0, len(chunk) - 3, 4))
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"{addr + i:08x}  {words:36s} {text}")


def reboot(value, what):
    app = App()
    try:
        app.set(CS_BOOT, value)
    except usb.core.USBError as e:
        die(f"{what} request refused ({e})" + (" - firmware has no bootloader" if value == REQ_DFU else ""))
    finally:
        app.close()


def cmd_reset(a):
    reboot(REQ_RESET, "reset")
    print("rebooting ...")
    if find(APP_VID, APP_PID, 1.0) is not None:
        time.sleep(1.0)
    if find(APP_VID, APP_PID, 10.0) is None:
        die("camera did not come back")
    print("camera is back")


def enter_dfu():
    dev = find(DFU_VID, DFU_PID)
    if dev is not None:
        return dev
    print("asking the firmware to enter DFU mode ...")
    reboot(REQ_DFU, "DFU")
    dev = find(DFU_VID, DFU_PID, 10.0)
    if dev is None:
        die("bootloader (0483:df11) did not appear. On Windows bind it to WinUSB with Zadig once.")
    # Let enumeration settle (VM USB pass-through may re-attach the new device once).
    time.sleep(1.5)
    return find(DFU_VID, DFU_PID, 5.0) or dev


class Dfu:
    def __init__(self, dev):
        self.dev = dev
        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass
        try:
            usb.util.claim_interface(dev, 0)
        except usb.core.USBError:
            pass

    def status(self):
        s = bytes(self.dev.ctrl_transfer(0xA1, DFU_GETSTATUS, 0, 0, 6, timeout=10000))
        return s[0], s[1] | s[2] << 8 | s[3] << 16, s[4]

    def wait_idle(self, want):
        while True:
            st, poll, state = self.status()
            if st != 0 or state == 10:
                raise RuntimeError(f"DFU error status {st} in state {DFU_STATES[state]}")
            if state in want:
                return state
            time.sleep(max(poll, 5) / 1000.0)

    def clear(self):
        for attempt in range(5):
            try:
                st, _, state = self.status()
                if state == 10:
                    self.dev.ctrl_transfer(0x21, DFU_CLRSTATUS, 0, 0, None)
                elif state != 2:
                    self.dev.ctrl_transfer(0x21, DFU_ABORT, 0, 0, None)
                return
            except usb.core.USBError:
                if attempt == 4:
                    raise
                time.sleep(1.0)
                self.__init__(find(DFU_VID, DFU_PID, 5.0) or self.dev)

    def download(self, image):
        self.clear()
        blocks = (len(image) + DFU_XFER - 1) // DFU_XFER
        for blk in range(blocks):
            chunk = image[blk * DFU_XFER:(blk + 1) * DFU_XFER]
            self.dev.ctrl_transfer(0x21, DFU_DNLOAD, blk, 0, chunk, timeout=5000)
            self.wait_idle({5})
            done = (blk + 1) * 100 // blocks
            sys.stdout.write(f"\r  writing {done:3d}% ({min(len(image), (blk + 1) * DFU_XFER)}/{len(image)} bytes)")
            sys.stdout.flush()
        print()
        self.dev.ctrl_transfer(0x21, DFU_DNLOAD, blocks, 0, None, timeout=5000)
        self.wait_idle({2})   # manifestation tolerant -> back to dfuIDLE

    def upload(self, size):
        self.clear()
        out = b""
        blk = 0
        while len(out) < size:
            d = bytes(self.dev.ctrl_transfer(0xA1, DFU_UPLOAD, blk, 0, DFU_XFER, timeout=5000))
            out += d
            blk += 1
            if len(d) < DFU_XFER:
                break
        self.dev.ctrl_transfer(0x21, DFU_ABORT, 0, 0, None)
        return out[:size]

    def leave(self):
        try:
            self.dev.ctrl_transfer(0x21, DFU_DETACH, 1000, 0, None, timeout=1000)
        except usb.core.USBError:
            pass  # the device may drop off the bus before the status stage
        usb.util.dispose_resources(self.dev)


def cmd_dfu(a):
    enter_dfu()
    print("bootloader ready (0483:df11). Flash with: ptdbg.py flash main.bin")


def cmd_flash(a):
    image = open(a.file, "rb").read()
    if len(image) < 8:
        die("image too small")
    sp, pc = struct.unpack("<II", image[:8])
    if not (0x20000000 < sp <= 0x20020000 and 0x08008000 < pc < 0x08080000):
        die(f"{a.file} is not linked for 0x08008000 (SP=0x{sp:08x} PC=0x{pc:08x}). "
            "Build with BOOTLOADER=1 (default) and use main.bin, not full.bin.")
    dfu = Dfu(enter_dfu())
    print(f"flashing {a.file} ({len(image)} bytes) to 0x08008000")
    dfu.download(image)
    print("verifying ...")
    if dfu.upload(len(image)) != image:
        die("verify FAILED - the device stays in DFU mode, try again")
    print("verify OK, starting new firmware")
    dfu.leave()
    if find(APP_VID, APP_PID, 15.0) is None:
        die("camera did not re-enumerate as 1e4e:0100")
    time.sleep(2.0)   # let enumeration settle before talking to it
    cmd_info(a)


def cmd_backup(a):
    dfu = Dfu(enter_dfu())
    data = dfu.upload(a.size)
    end = len(data.rstrip(b"\xff"))
    data = data[:(end + 3) & ~3]
    open(a.file, "wb").write(data)
    print(f"saved {len(data)} bytes of the application region to {a.file}")
    dfu.leave()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info").set_defaults(fn=cmd_info)
    s = sub.add_parser("log"); s.add_argument("-f", "--follow", action="store_true"); s.set_defaults(fn=cmd_log)
    sub.add_parser("counters").set_defaults(fn=cmd_counters)
    s = sub.add_parser("fault"); s.add_argument("--clear", action="store_true"); s.add_argument("--elf")
    s.set_defaults(fn=cmd_fault)
    s = sub.add_parser("peek"); s.add_argument("addr"); s.add_argument("length", nargs="?", type=lambda x: int(x, 0))
    s.add_argument("--elf"); s.set_defaults(fn=cmd_peek)
    sub.add_parser("reset").set_defaults(fn=cmd_reset)
    sub.add_parser("dfu").set_defaults(fn=cmd_dfu)
    s = sub.add_parser("flash"); s.add_argument("file"); s.set_defaults(fn=cmd_flash)
    s = sub.add_parser("backup"); s.add_argument("file"); s.add_argument("--size", type=int, default=480 * 1024)
    s.set_defaults(fn=cmd_backup)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
