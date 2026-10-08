"""Self-contained environment bootstrap for the GZM firmware updater.

Shared-machine friendly: on first run it creates a private virtualenv
next to this file, installs the Python dependencies into it and
re-launches itself. No system-wide pip installs; works on Linux,
macOS and Windows with a stock Python 3.8+.
"""

import os
import subprocess
import sys
from pathlib import Path

REQUIRED_PACKAGES = ["pyserial"]

VENV_DIR = Path(__file__).resolve().parent / ".venv"


def _venv_python():
    if os.name == "nt":
        return VENV_DIR / "Scripts" / "python.exe"
    return VENV_DIR / "bin" / "python"


def _in_our_venv():
    return Path(sys.prefix).resolve() == VENV_DIR.resolve()


def _deps_ok():
    try:
        import serial  # noqa: F401
        return True
    except ImportError:
        return False


def ensure_venv_and_relaunch():
    """Create the venv + deps if needed, then re-exec inside it."""
    if _in_our_venv() or _deps_ok():
        return

    py = _venv_python()
    if not py.exists():
        print(f"[setup] creating virtualenv at {VENV_DIR} ...")
        import venv
        venv.EnvBuilder(with_pip=True).create(VENV_DIR)

    print(f"[setup] installing dependencies: {', '.join(REQUIRED_PACKAGES)}")
    subprocess.check_call([str(py), "-m", "pip", "install", "--quiet",
                           *REQUIRED_PACKAGES])

    print("[setup] relaunching inside the virtualenv...\n")
    os.execv(str(py), [str(py), *sys.argv])


SMPCLIENT_PACKAGES = ["pyserial", "smpclient[serial]"]


def ensure_smpclient():
    """Make `import smpclient` work: install it into the private venv and
    re-exec there when the running Python does not have it."""
    try:
        import smpclient.transport.serial  # noqa: F401
        return
    except ImportError:
        pass

    if os.environ.get("GZM_SMPCLIENT_BOOTSTRAPPED"):
        sys.exit("error: smpclient still missing after installing it in "
                 f"{VENV_DIR}")

    py = _venv_python()
    if not py.exists():
        print(f"[setup] creating virtualenv at {VENV_DIR} ...")
        import venv
        venv.EnvBuilder(with_pip=True).create(VENV_DIR)

    print(f"[setup] installing dependencies: {', '.join(SMPCLIENT_PACKAGES)}")
    subprocess.check_call([str(py), "-m", "pip", "install", "--quiet",
                           *SMPCLIENT_PACKAGES])

    print("[setup] relaunching inside the virtualenv...\n")
    os.environ["GZM_SMPCLIENT_BOOTSTRAPPED"] = "1"
    os.execv(str(py), [str(py), *sys.argv])


def ensure_tk():
    """tkinter ships with Python but is a system package on Linux."""
    try:
        import tkinter  # noqa: F401
        return
    except ImportError:
        pass

    msg = {
        "linux": "sudo apt install python3-tk   (Debian/Ubuntu)\n"
                 "sudo dnf install python3-tkinter (Fedora)",
        "darwin": "brew install python-tk",
        "win32": "reinstall Python from python.org with the 'tcl/tk' option",
    }.get(sys.platform, "install the Tk bindings for your Python")
    sys.exit("error: tkinter is not available.\nTo install it:\n  " + msg)
