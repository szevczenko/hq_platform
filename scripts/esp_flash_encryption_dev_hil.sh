#!/bin/bash
# Explicitly gated flash-encryption Development-mode HIL workflow.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
EXAMPLE_DIR="$PROJECT_DIR/examples/esp/thingboard_fwu"
BUILD_DIR="$PROJECT_DIR/build_flash_encryption_dev"
PORT="/dev/ttyUSB1"
CONFIRMED=false

usage() {
    cat >&2 <<EOF
Usage: $0 [--port DEVICE] --confirm-irreversible

This workflow permanently changes ESP eFuses. It is not a normal flash helper.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            PORT="$2"
            shift 2
            ;;
        --confirm-irreversible)
            CONFIRMED=true
            shift
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ "$CONFIRMED" != true ]]; then
    echo "REFUSED: --confirm-irreversible is required." >&2
    echo "No device command was executed." >&2
    exit 2
fi
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "Error: activate ESP-IDF before running this workflow." >&2
    exit 1
fi
if [[ ! -c "$PORT" ]]; then
    echo "Error: serial device is unavailable: $PORT" >&2
    exit 1
fi
if [[ ! -f "$BUILD_DIR/config/sdkconfig.h" ]]; then
    echo "Error: build flash-encryption-dev first in $BUILD_DIR" >&2
    exit 1
fi
if ! grep -q '^#define CONFIG_SECURE_FLASH_ENC_ENABLED 1$' \
        "$BUILD_DIR/config/sdkconfig.h" ||
   ! grep -q '^#define CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT 1$' \
        "$BUILD_DIR/config/sdkconfig.h"; then
    echo "Error: build directory is not the flash-encryption-dev profile." >&2
    exit 1
fi

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
evidence_dir="$PROJECT_DIR/logs/security_hil/$stamp"
mkdir -p "$evidence_dir"

cat <<EOF
WARNING: THIS OPERATION IS IRREVERSIBLE.
Device: $PORT
Build:  $BUILD_DIR

It burns and protects a flash-encryption key, changes security/debug eFuses,
and consumes one of the finite Development-mode plaintext reflash cycles.
Hardware Secure Boot and anti-rollback are not enabled by this profile.
EOF

idf.py -p "$PORT" efuse-summary | tee "$evidence_dir/efuse-before.txt"
confirmation="ENABLE FLASH ENCRYPTION ON $PORT"
read -r -p "Type '$confirmation' to continue: " answer
if [[ "$answer" != "$confirmation" ]]; then
    echo "REFUSED: confirmation text did not match." >&2
    exit 2
fi

cp "$BUILD_DIR/thingboard_fwu.bin" "$evidence_dir/"
cp "$BUILD_DIR/partition_table/partition-table.bin" "$evidence_dir/"
cp "$BUILD_DIR/config/sdkconfig" "$evidence_dir/"
sha256sum "$evidence_dir"/*.bin > "$evidence_dir/SHA256SUMS"

cd "$EXAMPLE_DIR"
echo "Flashing with: idf.py -B $BUILD_DIR flash -p $PORT"
idf.py -B "$BUILD_DIR" flash -p "$PORT"

cat <<EOF
Flash completed. Do not interrupt power during the first encryption boot.
Monitor with:
  idf.py -B "$BUILD_DIR" monitor -p "$PORT"
After the encryption reboot is complete, press Enter to capture post-state.
EOF
read -r
idf.py -p "$PORT" efuse-summary | tee "$evidence_dir/efuse-after.txt"
echo "HIL evidence: $evidence_dir"
