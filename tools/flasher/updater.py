"""Core firmware-update flow over UART (MCUboot serial recovery, SMP).

Shared by the CLI (fw_update.py) and the GUI (fw_update_gui.py).

Flow:
  1. (optional) ask the app to reboot via its console port
  2. wait for MCUboot serial recovery (5 s DFU window after reset)
  3. image upload (slot 2)      -> slot1 (external QSPI NOR)
  4. image list                 -> the new image hash
  5. image test <hash>          -> mark pending (swap on next boot)
  6. reset                      -> MCUboot validates, swaps and boots

SMP client: smpclient (Python), which sends whole frames: ~30 KB/s.
"""

import asyncio
import os
import time

IMAGE_SLOT1 = 2  # MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD: image=2 -> slot1
# An image left in test mode is rolled back by MCUboot (~30 s on this board)
# before the recovery window opens: wait for that too.
RECOVERY_WAIT_S = 60
# Decoded SMP frame for smpclient: its base64 lines must fit MCUboot's line
# buffers (CONFIG_BOOT_LINE_BUFS x CONFIG_BOOT_MAX_LINE_INPUT_LEN = 128 x 128 B).
SMP_FRAME_SIZE = 8192

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


def request_app_reboot(console_port):
    """Send 'boot reboot' on the application console (USB CDC ACM)."""
    import serial

    with serial.Serial(console_port, 115200, timeout=0.3) as s:
        s.write(b'\r\n')
        time.sleep(0.2)
        s.write(b'boot reboot\r\n')
        time.sleep(0.5)


def run_update(port, image, baud="921600",
               log=print, progress=None, status=None,
               console_port=None, cancel=None,
               stage=None, transfer=None):
    """Run the whole update. Raises UpdateError on failure.

    log(str): one log line
    progress(float): upload progress 0..100
    status(str): short state description
    console_port: app console to auto-send 'boot reboot' (optional)
    cancel(): returns True to abort between steps
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

    asyncio.run(_update_smpclient(port, int(baud), image, log, progress,
                                  set_status, cancelled, set_stage, transfer))

    elapsed = time.monotonic() - start
    set_status(f"Done in {elapsed:.1f}s")
    log("If the new image works, run 'boot confirm' on the device shell;")
    log("otherwise the next reboot rolls back automatically.")
    return elapsed


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
            async for offset in client.upload(data, slot=IMAGE_SLOT1,
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


async def _reset_recovery(port, baud, timeout_s):
    from smpclient.requests.os_management import ResetWrite

    client = await smp_wait_recovery(port, baud, timeout_s)
    if not client:
        raise UpdateError("MCUboot serial recovery not detected")
    try:
        await client.request(ResetWrite(), timeout_s=5)
    except Exception:  # noqa: BLE001 - MCUboot may reset before answering
        pass
    finally:
        await client.disconnect()


def reset_recovery(port, baud="921600", timeout_s=10):
    """Leave MCUboot serial recovery (it stays there after a failed update)
    and boot the confirmed image again. Raises UpdateError."""
    asyncio.run(_reset_recovery(port, int(baud), timeout_s))
