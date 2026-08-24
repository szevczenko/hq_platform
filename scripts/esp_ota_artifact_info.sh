#!/bin/bash
# Inspect an ESP OTA artifact without flashing or changing device state.

set -euo pipefail

usage() {
    echo "Usage: $0 <application.bin> [--keyfile <public-or-private-key.pem>]" >&2
}

if [[ $# -ne 1 && $# -ne 3 ]]; then
    usage
    exit 2
fi

artifact="$1"
keyfile=""
if [[ $# -eq 3 ]]; then
    if [[ "$2" != "--keyfile" ]]; then
        usage
        exit 2
    fi
    keyfile="$3"
fi

if [[ ! -f "$artifact" ]]; then
    echo "Error: artifact not found: $artifact" >&2
    exit 1
fi
if [[ -n "$keyfile" && ! -f "$keyfile" ]]; then
    echo "Error: key file not found: $keyfile" >&2
    exit 1
fi

artifact="$(realpath "$artifact")"
size="$(stat -c '%s' "$artifact")"
checksum="$(sha256sum "$artifact" | awk '{print $1}')"

echo "artifact=$artifact"
echo "fw_size=$size"
echo "fw_checksum_algorithm=SHA256"
echo "fw_checksum=$checksum"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "image_info=not_run (IDF_PATH is not set)"
    if [[ -n "$keyfile" ]]; then
        echo "Error: IDF_PATH is required for signature verification" >&2
        exit 1
    fi
    echo "signature=not_verified"
    exit 0
fi

esptool="$IDF_PATH/components/esptool_py/esptool/esptool.py"
espsecure="$IDF_PATH/components/esptool_py/esptool/espsecure.py"
python_bin="${PYTHON:-python}"

"$python_bin" "$esptool" image_info "$artifact"

if [[ -z "$keyfile" ]]; then
    echo "signature=not_verified (provide --keyfile)"
    exit 0
fi

"$python_bin" "$espsecure" verify_signature --version 2 \
    --keyfile "$keyfile" "$artifact"
echo "signature=valid"
