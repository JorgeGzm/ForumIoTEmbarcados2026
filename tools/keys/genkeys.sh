#!/usr/bin/env bash
# Generate the DEMO keys used by sysbuild/imgtool:
#   - demo-ecdsa-p256.pem      signature key (ECDSA P-256)
#   - demo-encryption-p256.pem encryption key (ECIES P-256)
# Never use this flow for production keys: real keys live in an HSM/vault.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
IMGTOOL="$DIR/../../bootloader/mcuboot/scripts/imgtool.py"

if [ ! -f "$IMGTOOL" ]; then
    echo "imgtool not found at $IMGTOOL. Run 'west update' first." >&2
    exit 1
fi

# Prefer the active virtualenv, then the repository one (.venv/, created by
# tools/setup/setup.sh), then whatever python3 is on the PATH.
PYTHON=python3
if [ -n "${VIRTUAL_ENV:-}" ] && [ -x "$VIRTUAL_ENV/bin/python3" ]; then
    PYTHON="$VIRTUAL_ENV/bin/python3"
elif [ -x "$DIR/../../.venv/bin/python3" ]; then
    PYTHON="$DIR/../../.venv/bin/python3"
fi

# imgtool imports cbor2 and cryptography (see
# bootloader/mcuboot/scripts/requirements.txt).
if ! "$PYTHON" -c "import cbor2, cryptography, intelhex, click" 2>/dev/null; then
    echo "error: $PYTHON is missing the imgtool dependencies (cbor2, ...)." >&2
    echo "       Run ./tools/setup/setup.sh (creates .venv/ and installs them)," >&2
    echo "       or activate a virtualenv and run:" >&2
    echo "       pip install -r bootloader/mcuboot/zephyr/requirements.txt" >&2
    exit 1
fi

gen_key() {
    local key="$DIR/$1"
    if [ -f "$key" ]; then
        echo "Key already exists: $key"
    else
        "$PYTHON" "$IMGTOOL" keygen -k "$key" -t ecdsa-p256
        echo "Generated demo key: $key"
    fi
}

gen_key demo-ecdsa-p256.pem
gen_key demo-encryption-p256.pem
