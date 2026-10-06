#!/usr/bin/env python3
"""Upload speed of MCUboot serial recovery, called by suites/upload_speed.robot.

  upload_speed.py --client mcumgr|smpclient --image <signed.bin> \\
      --uart <UART4> --console <app USB console>

Reboots the app into MCUboot serial recovery, uploads <image> to slot1 and
resets without "image test": nothing is marked for swap, so the board comes
back on the version it was running. Prints, for the Robot suite:

  UPLOAD_SECONDS=<s> UPLOAD_KBPS=<KB/s>

The reboot and recovery helpers come from tools/twister/flash_uart*.py, so
both clients follow exactly the path the Twister flash commands use.
"""

import argparse
import asyncio
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "flasher"))
sys.path.insert(0, os.path.join(ROOT, "tools", "twister"))

ATTEMPTS = 3
RECOVERY_WAIT_S = 30


def upload_mcumgr(args, data_len):
    from bootstrap import ensure_mcumgr
    from flash_uart import RECOVERY_BAUD, IMAGE_SLOT1, reboot_to_recovery, wait_recovery
    from serial_transport import SerialTransport

    transport = SerialTransport(ensure_mcumgr(), args.uart, RECOVERY_BAUD)
    for _ in range(ATTEMPTS):
        reboot_to_recovery(args.uart, args.console)
        if wait_recovery(transport, RECOVERY_WAIT_S):
            break
    else:
        sys.exit("MCUboot serial recovery not detected")

    start = time.monotonic()
    ok, _, err = transport.run(
        ["-t", "120", "-w", "5", "image", "upload", "-n", IMAGE_SLOT1, args.image],
        timeout=600)
    seconds = time.monotonic() - start
    transport.run(["-t", "5", "reset"], timeout=15)
    if not ok:
        sys.exit(f"upload failed: {err.strip()}")
    return seconds


def upload_smpclient(args, data):
    from bootstrap import ensure_smpclient
    ensure_smpclient()

    from flash_uart import reboot_to_recovery
    from flash_uart_smpclient import FRAME_SIZE, IMAGE_SLOT1, wait_recovery
    from smpclient.requests.os_management import ResetWrite

    async def run():
        client = None
        for _ in range(ATTEMPTS):
            reboot_to_recovery(args.uart, args.console)
            client = await wait_recovery(args.uart, FRAME_SIZE, RECOVERY_WAIT_S)
            if client:
                break
        if not client:
            sys.exit("MCUboot serial recovery not detected")
        try:
            start = time.monotonic()
            async for _ in client.upload(data, slot=IMAGE_SLOT1, first_timeout_s=60,
                                         subsequent_timeout_s=10):
                pass
            seconds = time.monotonic() - start
            try:
                await client.request(ResetWrite(), timeout_s=5)
            except Exception:  # noqa: BLE001 - MCUboot may reset before answering
                pass
            return seconds
        finally:
            await client.disconnect()

    return asyncio.run(run())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", choices=("mcumgr", "smpclient"), required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--uart", required=True)
    parser.add_argument("--console", required=True)
    args = parser.parse_args()

    data = open(args.image, "rb").read()
    if args.client == "mcumgr":
        seconds = upload_mcumgr(args, len(data))
    else:
        seconds = upload_smpclient(args, data)
    print(f"UPLOAD_SECONDS={seconds:.1f} UPLOAD_KBPS={len(data) / 1024 / seconds:.1f}",
          flush=True)


if __name__ == "__main__":
    main()
