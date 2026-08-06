#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

TB_URL="${TB_URL:-http://127.0.0.1:8080}"
TB_API_KEY_FILE="${TB_API_KEY_FILE:-$PROJECT_DIR/docker/thingboard/api_key}"
TB_USERNAME="${TB_USERNAME:-}"
TB_PASSWORD="${TB_PASSWORD:-}"
BUILD_DIR="${TB_FW_BUILD_DIR:-build_posix}"

DEVICE_NAME="${TB_FW_DEVICE_NAME:-fw_demo_device_01}"
DEVICE_TYPE="${TB_FW_DEVICE_TYPE:-default}"
FW_TITLE="${TB_FW_TITLE:-hq_platform.bin}"
FW_VERSION="${TB_FW_VERSION:-$(date +%Y.%m.%d.%H%M%S)}"
FW_SIZE_BYTES="${TB_FW_SIZE_BYTES:-8192}"

MQTT_URL="${TB_FW_MQTT_URL:-mqtt://localhost:1883}"
CHECK_PERIOD_MS="${TB_FW_CHECK_PERIOD_MS:-3000}"
RECONNECT_DELAY_MS="${TB_FW_RECONNECT_DELAY_MS:-2000}"
CHUNK_SIZE="${TB_FW_CHUNK_SIZE:-1024}"

RUN_SECONDS="${TB_FW_RUN_SECONDS:-45}"
START_STACK="${TB_FW_START_STACK:-1}"

OUTPUT_ROOT="${TB_FW_OUTPUT_ROOT:-$SCRIPT_DIR/output/firmware_update}"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$OUTPUT_ROOT/$RUN_ID"
LOG_FILE="$OUT_DIR/run.log"
JSON_DIR="$OUT_DIR/json"

mkdir -p "$OUT_DIR" "$JSON_DIR"

log() {
    local msg="$1"
    echo "[$(date +%H:%M:%S)] $msg" | tee -a "$LOG_FILE"
}

fail() {
    local msg="$1"
    log "ERROR: $msg"
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "Required command not found: $1"
}

need_cmd curl
need_cmd python3
need_cmd sha256sum
need_cmd head

AUTH_HEADER=""
if [[ -f "$TB_API_KEY_FILE" ]]; then
    TB_API_KEY="$(tr -d '[:space:]' < "$TB_API_KEY_FILE")"
    if [[ -n "$TB_API_KEY" ]]; then
        AUTH_HEADER="X-Authorization: ApiKey $TB_API_KEY"
    fi
fi

if [[ -z "$AUTH_HEADER" && ( -z "$TB_USERNAME" || -z "$TB_PASSWORD" ) ]]; then
    fail "Provide either a non-empty API key file at $TB_API_KEY_FILE or TB_USERNAME/TB_PASSWORD for tenant-admin login."
fi

set_auth_bearer_from_login() {
    if [[ -z "$TB_USERNAME" || -z "$TB_PASSWORD" ]]; then
        return 1
    fi

    local login_body
    login_body="$(cat <<EOF
{"username":"$TB_USERNAME","password":"$TB_PASSWORD"}
EOF
)"

    local login_json
    login_json="$(curl -sS --fail -X POST \
        -H "Content-Type: application/json" \
        -d "$login_body" \
        "$TB_URL/api/auth/login")" || return 1

    local token
    token="$(printf '%s' "$login_json" | python3 -c '
import json
import sys
try:
    data = json.load(sys.stdin)
    print(data.get("token", ""))
except Exception:
    print("")
'
)"

    if [[ -z "$token" ]]; then
        return 1
    fi

    AUTH_HEADER="X-Authorization: Bearer $token"
    return 0
}

api_get() {
    local path="$1"
    curl -sS --fail -H "$AUTH_HEADER" "$TB_URL$path"
}

api_post_json() {
    local path="$1"
    local body="$2"
    curl -sS --fail -X POST \
        -H "$AUTH_HEADER" \
        -H "Content-Type: application/json" \
        -d "$body" \
        "$TB_URL$path"
}

api_post_multipart() {
    local path="$1"
    local file_path="$2"
    curl -sS --fail -X POST \
        -H "$AUTH_HEADER" \
        -F "file=@$file_path" \
        "$TB_URL$path"
}

json_get() {
    local path_expr="$1"
    python3 -c '
import json
import sys
path = sys.argv[1]
parts = [p for p in path.split(".") if p]
data = json.load(sys.stdin)
cur = data
for p in parts:
    if isinstance(cur, dict) and p in cur:
        cur = cur[p]
    else:
        print("")
        sys.exit(0)
if isinstance(cur, (dict, list)):
    print(json.dumps(cur, separators=(",", ":")))
else:
    print(str(cur))
' "$path_expr"
}

json_build_device_save_payload() {
    local ota_id="$1"
    python3 -c '
import json
import sys
ota_id = sys.argv[1]
src = json.load(sys.stdin)
payload = {
    "id": src.get("id"),
    "name": src.get("name"),
    "type": src.get("type"),
    "label": src.get("label"),
    "deviceProfileId": src.get("deviceProfileId"),
    "additionalInfo": src.get("additionalInfo") or {},
    "firmwareId": {"entityType": "OTA_PACKAGE", "id": ota_id},
}
customer_id = src.get("customerId")
if customer_id:
    payload["customerId"] = customer_id
print(json.dumps(payload, separators=(",", ":")))
' "$ota_id"
}

wait_for_tb() {
    local attempts=60
    local delay=2

    for ((i=1; i<=attempts; i++)); do
        if [[ -n "$AUTH_HEADER" ]]; then
            if curl -sS --fail -H "$AUTH_HEADER" "$TB_URL/api/auth/user" >/dev/null 2>&1; then
                log "ThingsBoard API is reachable"
                return 0
            fi
        else
            if curl -sS "$TB_URL" >/dev/null 2>&1; then
                log "ThingsBoard endpoint is reachable"
                return 0
            fi
        fi
        sleep "$delay"
    done

    fail "ThingsBoard API did not become ready at $TB_URL"
}

ensure_tenant_admin_auth() {
    local user_json
    user_json="$(api_get "/api/auth/user")" || fail "Cannot read /api/auth/user with current auth"
    printf '%s\n' "$user_json" > "$JSON_DIR/auth_user.json"

    local authority
    authority="$(printf '%s' "$user_json" | json_get "authority")"
    log "Authenticated authority: ${authority:-unknown}"

    if [[ "$authority" == "TENANT_ADMIN" ]]; then
        return 0
    fi

    if set_auth_bearer_from_login; then
        log "Switched to JWT auth using TB_USERNAME/TB_PASSWORD"
        user_json="$(api_get "/api/auth/user")" || fail "JWT auth failed after login"
        printf '%s\n' "$user_json" > "$JSON_DIR/auth_user_jwt.json"
        authority="$(printf '%s' "$user_json" | json_get "authority")"
        log "JWT authority: ${authority:-unknown}"
        if [[ "$authority" == "TENANT_ADMIN" ]]; then
            return 0
        fi
    fi

    fail "Current auth authority is ${authority:-unknown}. This workflow needs TENANT_ADMIN. Provide tenant-admin API key, or set TB_USERNAME/TB_PASSWORD for a tenant-admin account."
}

start_stack_if_needed() {
    if [[ "$START_STACK" != "1" ]]; then
        log "Skipping docker compose start (TB_FW_START_STACK=$START_STACK)"
        return
    fi

    log "Starting ThingsBoard stack from docker/thingboard"
    (
        cd "$PROJECT_DIR/docker/thingboard"
        docker compose up -d
    ) | tee -a "$LOG_FILE"
}

log "Output directory: $OUT_DIR"
start_stack_if_needed
wait_for_tb
ensure_tenant_admin_auth

log "Reading default device profile"
default_profile_json="$(api_get "/api/deviceProfileInfo/default")"
printf '%s\n' "$default_profile_json" > "$JSON_DIR/default_device_profile.json"
device_profile_id="$(printf '%s' "$default_profile_json" | json_get "id.id")"
[[ -n "$device_profile_id" ]] || fail "Cannot resolve default device profile id"
log "Default device profile id: $device_profile_id"

log "Checking if device exists: $DEVICE_NAME"
device_json="$(api_get "/api/tenant/devices?deviceName=$DEVICE_NAME" || true)"
device_id=""
if [[ -n "$device_json" ]]; then
    device_id="$(printf '%s' "$device_json" | json_get "id.id" || true)"
fi

if [[ -z "$device_id" ]]; then
    log "Creating device: $DEVICE_NAME"
    create_payload="$(cat <<EOF
{"name":"$DEVICE_NAME","type":"$DEVICE_TYPE","deviceProfileId":{"entityType":"DEVICE_PROFILE","id":"$device_profile_id"}}
EOF
)"
    device_json="$(api_post_json "/api/device" "$create_payload")"
    device_id="$(printf '%s' "$device_json" | json_get "id.id")"
fi

[[ -n "$device_id" ]] || fail "Cannot resolve device id"
printf '%s\n' "$device_json" > "$JSON_DIR/device.json"
log "Device id: $device_id"

log "Reading device credentials"
device_creds_json="$(api_get "/api/device/$device_id/credentials")"
printf '%s\n' "$device_creds_json" > "$JSON_DIR/device_credentials.json"
device_token="$(printf '%s' "$device_creds_json" | json_get "credentialsId")"
[[ -n "$device_token" ]] || fail "Device token is empty"
log "Device token resolved"

fw_file="$OUT_DIR/$FW_TITLE"
log "Generating dummy firmware file: $fw_file ($FW_SIZE_BYTES bytes)"
head -c "$FW_SIZE_BYTES" /dev/urandom > "$fw_file"
fw_checksum="$(sha256sum "$fw_file" | awk '{print $1}')"
log "Firmware checksum SHA256: $fw_checksum"

log "Creating OTA package info"
ota_payload="$(cat <<EOF
{"type":"FIRMWARE","title":"$FW_TITLE","version":"$FW_VERSION","deviceProfileId":{"entityType":"DEVICE_PROFILE","id":"$device_profile_id"}}
EOF
)"
ota_info_json="$(api_post_json "/api/otaPackage" "$ota_payload")"
printf '%s\n' "$ota_info_json" > "$JSON_DIR/ota_package_info.json"
ota_id="$(printf '%s' "$ota_info_json" | json_get "id.id")"
[[ -n "$ota_id" ]] || fail "Cannot resolve OTA package id"
log "OTA package id: $ota_id"

log "Uploading OTA package binary"
ota_upload_json="$(api_post_multipart "/api/otaPackage/$ota_id?checksumAlgorithm=SHA256&checksum=$fw_checksum" "$fw_file")"
printf '%s\n' "$ota_upload_json" > "$JSON_DIR/ota_package_upload_result.json"

log "Assigning firmware package to device by updating firmwareId"
device_full_json="$(api_get "/api/device/$device_id")"
printf '%s\n' "$device_full_json" > "$JSON_DIR/device_full_before_assign.json"
device_save_payload="$(printf '%s' "$device_full_json" | json_build_device_save_payload "$ota_id")"
printf '%s\n' "$device_save_payload" > "$JSON_DIR/device_save_payload.json"
device_after_assign_json="$(api_post_json "/api/device" "$device_save_payload")"
printf '%s\n' "$device_after_assign_json" > "$JSON_DIR/device_after_assign.json"

log "Building firmware update demo in WSL"
cmake -B "$BUILD_DIR" \
    -DHQ_DEFCONFIG=defconfig/posix.defconfig \
    -DHQ_BUILD_EXAMPLES=ON \
    -DFW_DEMO_CLIENT_ID=fw_demo_01 \
    -DFW_DEMO_USERNAME="$device_token" \
    -DFW_DEMO_PASSWORD="$device_token" \
    -DFW_DEMO_DEVICE_NAME="$DEVICE_NAME" \
    -DFW_DEMO_MQTT_URL="$MQTT_URL" \
    -DFW_DEMO_CHECK_PERIOD_MS="$CHECK_PERIOD_MS" \
    -DFW_DEMO_RECONNECT_DELAY_MS="$RECONNECT_DELAY_MS" \
    -DFW_DEMO_CHUNK_SIZE="$CHUNK_SIZE" \
    . | tee -a "$LOG_FILE"
cmake --build "$BUILD_DIR" --target thingboard_firmware_update_demo -j4 | tee -a "$LOG_FILE"

log "Running demo for ${RUN_SECONDS}s"
timeout "${RUN_SECONDS}s" "./$BUILD_DIR/examples/thingboard_firmware_update_demo" | tee "$OUT_DIR/demo_runtime.log" || true

log "Fetching latest firmware telemetry from ThingsBoard"
telemetry_json="$(api_get "/api/plugins/telemetry/DEVICE/$device_id/values/timeseries?keys=fw_state,fw_error,current_fw_title,current_fw_version")"
printf '%s\n' "$telemetry_json" > "$JSON_DIR/firmware_telemetry.json"

cat > "$OUT_DIR/summary.txt" <<EOF
TB URL: $TB_URL
Device name: $DEVICE_NAME
Device id: $device_id
Device token: $device_token
Device profile id: $device_profile_id
Firmware file: $fw_file
Firmware title: $FW_TITLE
Firmware version: $FW_VERSION
Firmware checksum (SHA256): $fw_checksum
OTA package id: $ota_id
Demo runtime log: $OUT_DIR/demo_runtime.log
Telemetry JSON: $JSON_DIR/firmware_telemetry.json
EOF

log "Completed. Summary: $OUT_DIR/summary.txt"