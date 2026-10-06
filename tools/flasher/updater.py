"""Core firmware-update flow over UART (MCUboot serial recovery, SMP).

Shared by the CLI (fw_update.py) and the GUI (fw_update_gui.py).

Flow:
  1. (optional) ask the app to reboot via its console port
  2. wait for MCUboot serial recovery (5 s DFU window after reset)
  3. image upload -n 2 <image>   -> slot1 (external QSPI NOR)
  4. image list                  -> the new image hash
  5. image test <hash>           -> mark pending (swap on next boot)
  6. reset                       -> MCUboot validates, swaps and boots

Two SMP clients do the same steps:
  - "mcumgr": the Go CLI. It sleeps 20 ms between every 124-byte line it
    sends, so an upload runs at ~4.5 KB/s.
  - "smpclient": the Python library, which sends whole frames: ~30 KB/s.
"""

import asyncio
import os
import re
import time

from bootstrap import ensure_mcumgr
from serial_transport import SerialTransport

IMAGE_SLOT1 = "2"  # MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD: image=2 -> slot1
# An image left in test mode is rolled back by MCUboot (~30 s on this board)
# before the recovery window opens: wait for that too.
RECOVERY_WAIT_S = 60
# Decoded SMP frame for smpclient: its base64 lines must fit MCUboot's line
# buffers (CONFIG_BOOT_LINE_BUFS x CONFIG_BOOT_MAX_LINE_INPUT_LEN = 128 x 128 B).
SMP_FRAME_SIZE = 8192

PROGRESS_RE = re.compile(r'(\d+\.?\d*)%')
# mcumgr upload line: "49.00 KiB / 364.21 KiB   13.45% 4.14 KiB/s 01m16s"
TRANSFER_RE = re.compile(r'([\d.]+) (B|KiB|MiB) / ([\d.]+) (B|KiB|MiB)\s+[\d.]+%'
                         r'(?:\s+([\d.]+) (B|KiB|MiB)/s)?')
UNIT = {"B": 1, "KiB": 1024, "MiB": 1024 * 1024}

# Steps reported through run_update(stage=...), in order: (id, label)
STAGES = [
    ("start", "Start"),
    ("recovery", "Recovery"),
    ("upload", "Upload"),
    ("validate", "Validate"),
    ("test", "Test mode"),
    ("reboot", "Reboot"),
]


class UpdateError(Exception):
    pass


def find_mcumgr(override=None, log=print):
    if override:
        if os.path.isfile(override) and os.access(override, os.X_OK):
            return override
        raise UpdateError(f"mcumgr not usable: {override}")
    try:
        return ensure_mcumgr(log=log)
    except RuntimeError as exc:
        raise UpdateError(str(exc)) from exc


def request_app_reboot(console_port):
    """Send 'boot reboot' on the application console (USB CDC ACM)."""
    import serial

    with serial.Serial(console_port, 115200, timeout=0.3) as s:
        s.write(b'\r\n')
        time.sleep(0.2)
        s.write(b'boot reboot\r\n')
        time.sleep(0.5)


def run_update(port, image, mcumgr=None, baud="921600",
               log=print, progress=None, status=None,
               console_port=None, cancel=None, client="mcumgr",
               stage=None, transfer=None):
    """Run the whole update. Raises UpdateError on failure.

    log(str): one log line
    progress(float): upload progress 0..100
    status(str): short state description
    console_port: app console to auto-send 'boot reboot' (optional)
    cancel(): returns True to abort between steps
    client: SMP client: "mcumgr" (Go CLI) or "smpclient"
    stage(str): a step of STAGES starts (its id)
    transfer(sent, total, bytes_per_s): upload counters (bytes_per_s may be None)
    """
    def set_status(text):
        if status:
            status(text)
        log(f"== {text}")

    def cancelled():
        return cancel() if cancel else False

    def set_stage(name):
        if stage:
            stage(name)

    if not os.path.isfile(image):
        raise UpdateError(f"image not found: {image}")
    if client not in ("mcumgr", "smpclient"):
        raise UpdateError(f"unknown SMP client: {client}")

    if client == "mcumgr":
        transport = SerialTransport(find_mcumgr(mcumgr, log=log), port, baud)
    start = time.monotonic()

    # 1. reboot into the DFU window (manual reset if no console port)
    set_stage("start")
    if console_port:
        set_status("Rebooting the application...")
        try:
            request_app_reboot(console_port)
        except Exception as exc:
            log(f"  warning: reboot request failed ({exc}); reset manually")
    else:
        log("Reset the board now (MCUboot waits 5 s for DFU after boot)...")

    if client == "mcumgr":
        _update_mcumgr(transport, image, log, progress, set_status, cancelled,
                       set_stage, transfer)
    else:
        asyncio.run(_update_smpclient(port, int(baud), image, log, progress,
                                      set_status, cancelled, set_stage, transfer))

    elapsed = time.monotonic() - start
    set_status(f"Done in {elapsed:.1f}s")
    log("If the new image works, run 'boot confirm' on the device shell;")
    log("otherwise the next reboot rolls back automatically.")
    return elapsed


def _update_mcumgr(transport, image, log, progress, set_status, cancelled,
                   set_stage, transfer):
    def on_line(line, is_progress):
        if is_progress and progress:
            match = PROGRESS_RE.search(line)
            if match:
                progress(float(match.group(1)))
        if is_progress and transfer:
            match = TRANSFER_RE.search(line)
            if match:
                sent = float(match.group(1)) * UNIT[match.group(2)]
                total = float(match.group(3)) * UNIT[match.group(4)]
                rate = (float(match.group(5)) * UNIT[match.group(6)]
                        if match.group(5) else None)
                transfer(int(sent), int(total), rate)
        log(f"  {line}")

    # 2. catch the serial recovery window
    set_stage("recovery")
    set_status("Waiting for MCUboot serial recovery...")
    poll_start = time.monotonic()
    detected = False
    while (time.monotonic() - poll_start) < RECOVERY_WAIT_S and not cancelled():
        # never kill mcumgr mid-frame: its own -t must expire first
        ok, _, _ = transport.run(["-t", "2", "echo", "hello"], timeout=5)
        if ok:
            detected = True
            break
        time.sleep(0.5)
    if cancelled():
        raise UpdateError("cancelled")
    if not detected:
        raise UpdateError(
            "MCUboot serial recovery not detected.\n"
            "Check the UART wiring/port and reset the board again.")
    log("  MCUboot serial recovery detected!")

    # 3. upload to slot1 (external NOR)
    set_stage("upload")
    set_status(f"Uploading {os.path.basename(image)} to slot1...")
    ok, _, err = transport.run(
        ["-t", "120", "-w", "5", "image", "upload", "-n", IMAGE_SLOT1, image],
        timeout=600, on_line=on_line)
    if not ok:
        raise UpdateError(f"upload failed: {err.strip()}")

    # 4. list images, grab the hash of the pending image in slot 1
    set_stage("validate")
    set_status("Validating images (MCUboot reads back the whole image)...")
    ok, stdout, err = transport.run(["-t", "60", "image", "list"],
                                    timeout=90, on_line=on_line)
    if not ok:
        raise UpdateError(f"image list failed: {err.strip()}")

    image_hash = SerialTransport.parse_hash(stdout)
    if not image_hash:
        raise UpdateError("could not find the uploaded image hash in slot 1")

    # 5. mark it for test swap (rollback-safe: confirm happens on the app shell)
    set_stage("test")
    set_status(f"Marking image {image_hash[:16]}... for test swap")
    ok, _, err = transport.run(["-t", "60", "image", "test", image_hash],
                               timeout=90, on_line=on_line)
    if not ok:
        raise UpdateError(f"image test failed: {err.strip()}")

    # 6. reboot into the new image (the reset ACK may never arrive)
    set_stage("reboot")
    set_status("Rebooting (MCUboot will swap and boot the new image)...")
    transport.run(["-t", "5", "reset"], timeout=15)


async def smp_wait_recovery(port, baud=921600, timeout_s=RECOVERY_WAIT_S,
                            frame=SMP_FRAME_SIZE, cancelled=None):
    """Poll with "image list" until MCUboot answers; return a connected client.

    The port is opened once and kept open while the board reboots: only the
    request is repeated, so no attempt is lost reopening it.
    """
    import logging

    from smpclient import SMPClient
    from smpclient.generics import success
    from smpclient.requests.image_management import ImageStatesRead
    from smpclient.transport.serial import BufferSize, SMPSerialTransport

    # While the app reboots every request times out, and MCUboot never
    # answers "mcumgr params" (read on connect): smpclient logs each one.
    logging.getLogger("smpclient").setLevel(logging.CRITICAL)

    deadline = time.monotonic() + timeout_s
    client = None
    while time.monotonic() < deadline and not (cancelled and cancelled()):
        try:
            if client is None:
                client = SMPClient(SMPSerialTransport(BufferSize(frame), baudrate=baud),
                                   port, timeout_s=1.0)
                await client.connect(connect_timeout_s=2.0)
            if success(await client.request(ImageStatesRead(), timeout_s=1.0)):
                return client
        except TimeoutError:
            pass
        except Exception:  # noqa: BLE001 - port missing or busy: reopen it
            if client is not None:
                await client.disconnect()
                client = None
            await asyncio.sleep(0.5)
    if client is not None:
        await client.disconnect()
    return None


async def _update_smpclient(port, baud, image, log, progress, set_status, cancelled,
                            set_stage, transfer):
    from smpclient.generics import success
    from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
    from smpclient.requests.os_management import ResetWrite

    data = open(image, "rb").read()

    # 2. catch the serial recovery window
    set_stage("recovery")
    set_status("Waiting for MCUboot serial recovery...")
    client = await smp_wait_recovery(port, baud, cancelled=cancelled)
    if cancelled():
        if client:
            await client.disconnect()
        raise UpdateError("cancelled")
    if not client:
        raise UpdateError(
            "MCUboot serial recovery not detected.\n"
            "Check the UART wiring/port and reset the board again.")
    log("  MCUboot serial recovery detected!")

    try:
        # 3. upload to slot1 (external NOR)
        set_stage("upload")
        set_status(f"Uploading {os.path.basename(image)} to slot1...")
        upload_start = time.monotonic()
        try:
            async for offset in client.upload(data, slot=int(IMAGE_SLOT1),
                                              first_timeout_s=60,
                                              subsequent_timeout_s=10):
                if progress:
                    progress(100.0 * offset / len(data))
                if transfer:
                    elapsed = time.monotonic() - upload_start
                    transfer(offset, len(data), offset / elapsed if elapsed > 0 else None)
                if cancelled():
                    raise UpdateError("cancelled")
        except UpdateError:
            raise
        except Exception as exc:
            raise UpdateError(f"upload failed: {exc}") from exc
        seconds = time.monotonic() - upload_start
        log(f"  {len(data)} B in {seconds:.1f}s ({len(data) / 1024 / seconds:.1f} KB/s)")

        # 4. list images, grab the hash of the pending image in slot 1
        set_stage("validate")
        set_status("Validating images (MCUboot reads back the whole image)...")
        states = await client.request(ImageStatesRead(), timeout_s=60)
        if not success(states):
            raise UpdateError(f"image list failed: {states}")
        for img in states.images:
            log(f"  slot={img.slot} version={img.version} "
                f"hash={bytes(img.hash).hex() if img.hash else '-'}")
        image_hash = next((bytes(img.hash) for img in states.images
                           if img.slot == 1 and img.hash), None)
        if not image_hash:
            raise UpdateError("could not find the uploaded image hash in slot 1")

        # 5. mark it for test swap (rollback-safe: confirm happens on the app shell)
        set_stage("test")
        set_status(f"Marking image {image_hash.hex()[:16]}... for test swap")
        rsp = await client.request(ImageStatesWrite(hash=image_hash, confirm=False),
                                   timeout_s=60)
        if not success(rsp):
            raise UpdateError(f"image test failed: {rsp}")

        # 6. reboot into the new image (the reset ACK may never arrive)
        set_stage("reboot")
        set_status("Rebooting (MCUboot will swap and boot the new image)...")
        try:
            await client.request(ResetWrite(), timeout_s=5)
        except Exception:  # noqa: BLE001
            pass
    finally:
        await client.disconnect()
