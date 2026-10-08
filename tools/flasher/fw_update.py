#!/usr/bin/env python3
"""Minimal CLI firmware updater over UART (MCUboot serial recovery, SMP).

See updater.py for the flow; fw_update_gui.py is the graphical version.
"""

import argparse
import sys

from bootstrap import ensure_smpclient, ensure_venv_and_relaunch

ensure_venv_and_relaunch()
ensure_smpclient()

from updater import UpdateError, reset_recovery, run_update  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", nargs="?",
                        help="signed (+encrypted) image, e.g. zephyr.signed.encrypted.bin")
    parser.add_argument("-p", "--port", required=True, help="UART4 serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("-b", "--baud", default="921600", help="baud rate (default: 921600)")
    parser.add_argument("-c", "--console", help="app console port (e.g. /dev/ttyACM0) "
                                                "to auto-send 'boot reboot'")
    parser.add_argument("--reset", action="store_true",
                        help="only leave MCUboot serial recovery (it stays there "
                             "after a failed update) and boot the app again")
    args = parser.parse_args()
    if not args.reset and not args.image:
        parser.error("the image is required (or use --reset)")

    try:
        if args.reset:
            reset_recovery(args.port, args.baud)
        else:
            run_update(args.port, args.image, baud=args.baud, console_port=args.console)
    except UpdateError as exc:
        sys.exit(f"error: {exc}")


if __name__ == "__main__":
    main()
