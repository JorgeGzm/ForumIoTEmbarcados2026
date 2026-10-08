#!/usr/bin/env python3
"""Twister flash command: program a test image through MCUboot over UART.

Used with:
  west twister ... --device-testing --device-serial <UART4 by-id path> \\
      --flash-before --flash-command tools/twister/flash_uart.py

No SWD probe needed. The board is rebooted by software into the MCUboot
serial-recovery window, the signed (+encrypted) test image is uploaded to
slot1 with smpclient and confirmed, then MCUboot swaps it in. Twister
attaches to the same UART afterwards (--flash-before) to read the ztest
output.

Reboot paths tried, in order (whichever image is running answers one):
  - demo app  : "boot reboot" on its USB console (--console, optional)
  - test image: "kernel reboot cold" on the UART4 shell at 115200

smpclient[serial] is installed on first use into tools/flasher/.venv.
"""

import argparse
import asyncio
import glob
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "flasher"))

from bootstrap import ensure_smpclient  # noqa: E402

ensure_smpclient()

import serial  # noqa: E402
from smpclient.generics import success  # noqa: E402
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite  # noqa: E402
from smpclient.requests.os_management import ResetWrite  # noqa: E402

from updater import smp_wait_recovery  # noqa: E402

RECOVERY_BAUD = 921600
TEST_CONSOLE_BAUD = 115200
IMAGE_SLOT1 = 2


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


def slot1_hash(states):
    for image in states.images:
        if image.slot == 1 and image.hash:
            return bytes(image.hash)
    return None


async def flash(args):
    image = find_image(args.build_dir)
    data = open(image, "rb").read()

    client = None
    if args.no_reboot:
        log(f"waiting up to {args.wait}s for a manual reset...")
        client = await smp_wait_recovery(args.uart, RECOVERY_BAUD, args.wait)
    else:
        for attempt in range(1, 4):
            log(f"rebooting into MCUboot recovery (attempt {attempt})")
            reboot_to_recovery(args.uart, args.console)
            client = await smp_wait_recovery(args.uart, RECOVERY_BAUD, args.wait)
            if client:
                break
    if not client:
        sys.exit("[flash_uart] MCUboot serial recovery not detected")

    try:
        log(f"uploading {os.path.relpath(image, args.build_dir)} ({len(data)} B)")
        start = time.monotonic()
        async for _ in client.upload(data, slot=IMAGE_SLOT1, first_timeout_s=60,
                                     subsequent_timeout_s=10):
            pass
        log(f"uploaded in {time.monotonic() - start:.1f} s")

        states = await client.request(ImageStatesRead(), timeout_s=60)
        image_hash = slot1_hash(states) if success(states) else None
        if not image_hash:
            sys.exit("[flash_uart] uploaded image not found in slot1")

        # confirm = permanent swap: the test image must survive later resets
        rsp = await client.request(ImageStatesWrite(hash=image_hash, confirm=True),
                                   timeout_s=60)
        if not success(rsp):
            sys.exit(f"[flash_uart] image confirm failed: {rsp}")

        try:
            await client.request(ResetWrite(), timeout_s=5)
        except Exception:  # noqa: BLE001 - MCUboot may reset before answering
            pass
        log("image confirmed; MCUboot will swap and boot it")
    finally:
        await client.disconnect()


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
    asyncio.run(flash(parser.parse_args()))


if __name__ == "__main__":
    main()
