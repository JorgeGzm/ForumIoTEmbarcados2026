#!/usr/bin/env bash
# Environment-agnostic workspace setup:
#   - creates a virtualenv in .venv/ at the repository root
#   - installs west plus the Zephyr, MCUboot and Robot Python dependencies
#   - fetches the manifest projects (west init + update)
#   - exports Zephyr to CMake and generates the demo keys
#
# Safe to run again: finished steps are skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

VENV="$ROOT/.venv"
PYTHON="${PYTHON:-python3}"
FULL=0

usage() {
    cat <<'EOF'
Create .venv/ with the workspace dependencies, fetch the west projects and
generate the demo keys. Safe to run again (finished steps are skipped).

Usage:
  ./tools/setup/setup.sh          minimal dependencies: build, Twister,
                                  coverage, MCUboot/imgtool and Robot
  ./tools/setup/setup.sh --full   official full set: west packages pip --install

Environment:
  PYTHON   interpreter used to create the venv (default: python3)
EOF
}

for arg in "$@"; do
    case "$arg" in
        --full) FULL=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $arg (try --help)" >&2; exit 1 ;;
    esac
done

info() { echo "[setup] $*"; }

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "error: $PYTHON not found in PATH (use PYTHON=... to pick another)" >&2
    exit 1
fi

if [ ! -x "$VENV/bin/python3" ]; then
    info "creating virtualenv at $VENV ..."
    if ! "$PYTHON" -m venv "$VENV"; then
        echo "error: could not create the virtualenv." >&2
        echo "       On Debian/Ubuntu: sudo apt install python3-venv" >&2
        exit 1
    fi
else
    info "virtualenv already exists: $VENV"
fi

PIP="$VENV/bin/pip"
WEST="$VENV/bin/west"

info "installing/upgrading west ..."
"$PIP" install --quiet --upgrade pip west

# Required by 'west packages pip' and by every Python tool that looks for the
# active environment.
export VIRTUAL_ENV="$VENV"
export PATH="$VENV/bin:$PATH"

if [ ! -d "$ROOT/.west" ]; then
    info "initializing the west workspace (west init -l manifest) ..."
    "$WEST" init -l manifest
else
    info ".west/ already exists: skipping west init"
fi

info "fetching the manifest projects (west update) ..."
"$WEST" update

if [ "$FULL" = 1 ]; then
    info "installing the full Zephyr dependency set (west packages pip --install) ..."
    "$WEST" packages pip --install
else
    # The run-test extras (opencv, pyocd, python-can, spdx-tools...) are not
    # used by this demo; --full installs the official set when needed.
    info "installing the Python dependencies (Zephyr, MCUboot/imgtool, Robot) ..."
    "$PIP" install --quiet \
        -r "$ROOT/zephyr/scripts/requirements-base.txt" \
        -r "$ROOT/zephyr/scripts/requirements-build-test.txt" \
        natsort tabulate \
        -r "$ROOT/bootloader/mcuboot/zephyr/requirements.txt" \
        -r "$ROOT/tests/robot/requirements.txt"
fi

info "exporting Zephyr to CMake (west zephyr-export) ..."
"$WEST" zephyr-export

if [ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ] && ! ls -d "$HOME"/zephyr-sdk-* >/dev/null 2>&1; then
    echo
    echo "[setup] warning: no Zephyr SDK found (ZEPHYR_SDK_INSTALL_DIR or ~/zephyr-sdk-*)."
    echo "[setup]          install one before building for the board: $WEST sdk install"
    echo
fi

info "generating the demo keys (tools/keys/genkeys.sh) ..."
"$ROOT/tools/keys/genkeys.sh"

echo
info "done."
info "build:  west build -b weact_stm32h743 --sysbuild -p auto -s app -d app/build -- -DBOARD_ROOT=$ROOT"
info "shell:  source .venv/bin/activate"
info "re-run: ./tools/setup/setup.sh (skips finished steps)"
