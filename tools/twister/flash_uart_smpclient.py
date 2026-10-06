#!/usr/bin/env python3
"""Twister flash command: flash_uart.py with smpclient instead of mcumgr.

Same arguments and the same steps as flash_uart.py (reboot into MCUboot
serial recovery, upload to slot1, confirm, reset), so either one can be
given to --flash-command:

  west twister ... --device-testing --device-serial <UART4 by-id path> \\
      --flash-before --flash-command tools/twister/flash_uart_smpclient.py

Why: the mcumgr Go CLI sleeps 20 ms between every 124-byte line it sends
(newtmgr nmxact/nmserial/serial_xport.go), which caps an upload at about
4.5 KB/s. smpclient sends a whole frame at once and fills MCUboot's line
buffers: the same 373 KB image goes from ~85 s to ~12 s at 921600 baud.

smpclient[serial] is installed on first use into tools/flasher/.venv.
"""

import argparse
import asyncio
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "flasher"))

from bootstrap import ensure_smpclient  # noqa: E402

ensure_smpclient()

from smpclient.generics import success  # noqa: E402
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite  # noqa: E402
from smpclient.requests.os_management import ResetWrite  # noqa: E402

from flash_uart import (  # noqa: E402
    RECOVERY_BAUD,
    default_port,
    find_image,
    reboot_to_recovery,
)

from updater import SMP_FRAME_SIZE as FRAME_SIZE, smp_wait_recovery  # noqa: E402

IMAGE_SLOT1 = 2


def log(msg):
    print(f"[flash_uart_smpclient] {msg}", flush=True)


async def wait_recovery(uart, frame, timeout_s):
    """Poll until MCUboot answers; return a connected client (or None)."""
    return await smp_wait_recovery(uart, int(RECOVERY_BAUD), timeout_s, frame)


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
        client = await wait_recovery(args.uart, args.frame, args.wait)
    else:
        for attempt in range(1, 4):
            log(f"rebooting into MCUboot recovery (attempt {attempt})")
            reboot_to_recovery(args.uart, args.console)
            client = await wait_recovery(args.uart, args.frame, args.wait)
            if client:
                break
    if not client:
        sys.exit("[flash_uart_smpclient] MCUboot serial recovery not detected")

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
            sys.exit("[flash_uart_smpclient] uploaded image not found in slot1")

        # confirm = permanent swap: the test image must survive later resets
        rsp = await client.request(ImageStatesWrite(hash=image_hash, confirm=True),
                                   timeout_s=60)
        if not success(rsp):
            sys.exit(f"[flash_uart_smpclient] image confirm failed: {rsp}")

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
    parser.add_argument("--frame", type=int, default=FRAME_SIZE,
                        help="SMP frame size in bytes (default: %(default)s)")
    asyncio.run(flash(parser.parse_args()))


if __name__ == "__main__":
    main()
