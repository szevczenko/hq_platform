#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

BROKER_IMAGE="${TB_IT_BROKER_IMAGE:-eclipse-mosquitto:2}"
BROKER_NAME="${TB_IT_BROKER_NAME:-hq_tb_it_mosquitto}"
BROKER_PORT="${TB_IT_BROKER_PORT:-1884}"
MQTT_URL="${TB_IT_MQTT_URL:-mqtt://127.0.0.1:${BROKER_PORT}}"

BUILD_DIR="${TB_IT_BUILD_DIR:-$PROJECT_DIR/build_posix}"
TEST_BIN="$BUILD_DIR/tests/tb_reconnect_integration_tests"

log() {
    echo "[$(date +%H:%M:%S)] $*"
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Required command not found: $1" >&2
        exit 1
    }
}

start_broker() {
    if docker ps --format '{{.Names}}' | grep -qx "$BROKER_NAME"; then
        return 0
    fi

    if docker ps -a --format '{{.Names}}' | grep -qx "$BROKER_NAME"; then
        docker start "$BROKER_NAME" >/dev/null
        return 0
    fi

    docker run -d --name "$BROKER_NAME" -p "${BROKER_PORT}:1883" "$BROKER_IMAGE" >/dev/null
}

stop_broker() {
    if docker ps --format '{{.Names}}' | grep -qx "$BROKER_NAME"; then
        docker stop "$BROKER_NAME" >/dev/null
    fi
}

wait_broker_up() {
    local retries=20
    local i
    for ((i=1; i<=retries; i++)); do
        if docker ps --format '{{.Names}}' | grep -qx "$BROKER_NAME"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

cleanup() {
    stop_broker || true
    if docker ps -a --format '{{.Names}}' | grep -qx "$BROKER_NAME"; then
        docker rm "$BROKER_NAME" >/dev/null || true
    fi
}

need_cmd cmake
need_cmd docker

trap cleanup EXIT

log "Building reconnect integration test target"
cmake --build "$BUILD_DIR" --target tb_reconnect_integration_tests

log "Starting broker container ${BROKER_NAME} on localhost:${BROKER_PORT}"
start_broker
wait_broker_up || {
    echo "Broker failed to start" >&2
    exit 1
}

log "Running reconnect integration test against ${MQTT_URL}"
TB_IT_MQTT_URL="$MQTT_URL" \
TB_IT_BROKER_STOP_CMD="docker stop ${BROKER_NAME} >/dev/null" \
TB_IT_BROKER_START_CMD="docker start ${BROKER_NAME} >/dev/null" \
"$TEST_BIN"

log "Reconnect integration test completed"
