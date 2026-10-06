#!/usr/bin/env python3
"""Twister flash command: program a test image through MCUboot over UART.

Used with:
  west twister ... --device-testing --device-serial <UART4 by-id path> \\
      --flash-before --flash-command tools/twister/flash_uart.py

No SWD probe needed. The board is rebooted by software into the MCUboot
serial-recovery window, the signed (+encrypted) test image is uploaded to
slot1 and confirmed, then MCUboot swaps it in. Twister attaches to the
same UART afterwards (--flash-before) to read the ztest output.

Reboot paths tried, in order (whichever image is running answers one):
  - demo app  : "boot reboot" on its USB console (--console, optional)
  - test image: "kernel reboot cold" on the UART4 shell at 115200
"""

import argparse
import glob
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "flasher"))

import serial  # noqa: E402

from bootstrap import ensure_mcumgr  # noqa: E402
from serial_transport import SerialTransport  # noqa: E402

RECOVERY_BAUD = "921600"
TEST_CONSOLE_BAUD = 115200
IMAGE_SLOT1 = "2"


def default_port(pattern, fallback=None):
    """Stable /dev/serial/by-id path: ttyACM numbers swap whenever the
    demo's USB console re-enumerates after a reboot."""
    for path in sorted(glob.glob("/dev/serial/by-id/*")):
        if pattern in path:
            return path
    return f"/dev/{fallback}" if fallback else None


def log(msg):
    print(f"[flash_uart] {msg}", flush=True)


def find_image(build_dir):
    candidates = [
        p for p in glob.glob(os.path.join(build_dir, "**", "zephyr",
                                          "zephyr.signed.encrypted.bin"),
                             recursive=True)
        if f"{os.sep}mcuboot{os.sep}" not in p
    ]
    if not candidates:
        sys.exit(f"[flash_uart] no signed image under {build_dir} "
                 "(is the scenario built with sysbuild: true?)")
    return candidates[0]


def send_line(port, baud, line):
    try:
        with serial.Serial(port, baud, timeout=0.3) as s:
            s.write(b"\r")
            time.sleep(0.2)
            # type slowly: the shell drops bytes on a burst right after boot
            for ch in line.encode() + b"\r":
                s.write(bytes([ch]))
                time.sleep(0.01)
            time.sleep(0.3)
    except (serial.SerialException, OSError):
        pass


def reboot_to_recovery(uart, console):
    if console and os.path.exists(console):
        send_line(console, 115200, "boot reboot")
    send_line(uart, TEST_CONSOLE_BAUD, "kernel reboot cold")


def wait_recovery(transport, timeout_s=30):
    # MCUboot listens for only 5 s after reset: poll every ~2 s
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        # never kill mcumgr mid-frame: its own -t must expire first
        ok, _, _ = transport.run(["-t", "2", "echo", "hello"], timeout=5)
        if ok:
            return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--board-id")
    parser.add_argument("--uart", default=default_port("1a86", "ttyUSB0"),
                        help="UART4 adapter (MCUboot recovery + test console)")
    parser.add_argument("--console", default=default_port("Do_Codigo_ao_Campo"),
                        help="demo app USB console (used to reboot the app)")
    parser.add_argument("--no-reboot", action="store_true",
                        help="do not reboot by software; wait for a manual reset")
    parser.add_argument("--wait", type=int, default=30,
                        help="seconds to wait for the MCUboot recovery window")
    args = parser.parse_args()

    image = find_image(args.build_dir)
    transport = SerialTransport(ensure_mcumgr(log=log), args.uart, RECOVERY_BAUD)

    if args.no_reboot:
        log(f"waiting up to {args.wait}s for a manual reset...")
        if not wait_recovery(transport, args.wait):
            sys.exit("[flash_uart] MCUboot serial recovery not detected")
    else:
        for attempt in range(1, 4):
            log(f"rebooting into MCUboot recovery (attempt {attempt})")
            reboot_to_recovery(args.uart, args.console)
            if wait_recovery(transport, args.wait):
                break
        else:
            sys.exit("[flash_uart] MCUboot serial recovery not detected")

    log(f"uploading {os.path.relpath(image, args.build_dir)}")
    ok, _, err = transport.run(
        ["-t", "120", "-w", "5", "image", "upload", "-n", IMAGE_SLOT1, image],
        timeout=600)
    if not ok:
        sys.exit(f"[flash_uart] upload failed: {err.strip()}")

    ok, out, err = transport.run(["-t", "60", "image", "list"], timeout=90)
    image_hash = SerialTransport.parse_hash(out) if ok else None
    if not image_hash:
        sys.exit(f"[flash_uart] uploaded image not found in slot1 {err.strip()}")

    # confirm = permanent swap: the test image must survive later resets
    ok, _, err = transport.run(["-t", "60", "image", "confirm", image_hash],
                               timeout=90)
    if not ok:
        sys.exit(f"[flash_uart] image confirm failed: {err.strip()}")

    transport.run(["-t", "5", "reset"], timeout=15)
    log("image confirmed; MCUboot will swap and boot it")


if __name__ == "__main__":
    main()
