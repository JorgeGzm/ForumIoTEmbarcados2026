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

gen_key() {
    local key="$DIR/$1"
    if [ -f "$key" ]; then
        echo "Key already exists: $key"
    else
        python3 "$IMGTOOL" keygen -k "$key" -t ecdsa-p256
        echo "Generated demo key: $key"
    fi
}

gen_key demo-ecdsa-p256.pem
gen_key demo-encryption-p256.pem
