#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Quick rerun profile:
# - do not restart docker stack unless explicitly requested
# - shorter demo runtime
# - smaller firmware payload for faster transfer
# - unique firmware version per run
export TB_FW_START_STACK="${TB_FW_START_STACK:-0}"
export TB_FW_RUN_SECONDS="${TB_FW_RUN_SECONDS:-20}"
export TB_FW_SIZE_BYTES="${TB_FW_SIZE_BYTES:-4096}"
export TB_FW_VERSION="${TB_FW_VERSION:-quick_$(date +%Y%m%d_%H%M%S)}"

exec bash "$SCRIPT_DIR/thingboard_firmware_update_test.sh"