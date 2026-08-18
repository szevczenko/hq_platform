#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CERT_DIR="${TB_TLS_IT_CERT_DIR:-$PROJECT_DIR/cert/tls_it}"

BROKER_IMAGE="${TB_TLS_IT_BROKER_IMAGE:-eclipse-mosquitto:2}"
BROKER_OK_NAME="${TB_TLS_IT_BROKER_OK_NAME:-hq_tb_tls_it_ok}"
BROKER_BADHOST_NAME="${TB_TLS_IT_BROKER_BADHOST_NAME:-hq_tb_tls_it_badhost}"
BROKER_OK_PORT="${TB_TLS_IT_BROKER_OK_PORT:-8885}"
BROKER_BADHOST_PORT="${TB_TLS_IT_BROKER_BADHOST_PORT:-8886}"

BUILD_DIR="${TB_TLS_IT_BUILD_DIR:-$PROJECT_DIR/build_wsl}"
TEST_BIN="$BUILD_DIR/tests/tb_tls_integration_tests"

log() {
    echo "[$(date +%H:%M:%S)] $*"
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Required command not found: $1" >&2
        exit 1
    }
}

resolve_path() {
  if command -v realpath >/dev/null 2>&1; then
    realpath -m "$1"
    return
  fi
  if command -v readlink >/dev/null 2>&1; then
    readlink -m "$1"
    return
  fi
  return 1
}

validate_cert_dir() {
  local cert_root_abs cert_dir_abs

  if [[ -z "$CERT_DIR" ]]; then
    echo "CERT_DIR must not be empty" >&2
    exit 1
  fi

  cert_root_abs="$(resolve_path "$PROJECT_DIR/cert")" || {
    echo "Failed to resolve project cert root path" >&2
    exit 1
  }
  cert_dir_abs="$(resolve_path "$CERT_DIR")" || {
    echo "Failed to resolve cert directory path: $CERT_DIR" >&2
    exit 1
  }

  if [[ "$cert_dir_abs" == "/" || "$cert_dir_abs" == "$PROJECT_DIR" ||
      "$cert_dir_abs" == "$cert_root_abs" ]]; then
    echo "Refusing to clean risky cert directory path: $cert_dir_abs" >&2
    exit 1
  fi

  if [[ "$cert_dir_abs" != "$cert_root_abs"/* ]]; then
    echo "CERT_DIR must be under $cert_root_abs, got: $cert_dir_abs" >&2
    exit 1
  fi

  CERT_DIR="$cert_dir_abs"
}

gen_certs() {
  validate_cert_dir
    mkdir -p "$CERT_DIR"
    rm -f "$CERT_DIR"/*

    openssl genrsa -out "$CERT_DIR/ca_good.key" 2048 >/dev/null 2>&1
    openssl req -x509 -new -nodes -key "$CERT_DIR/ca_good.key" -sha256 -days 3650 \
      -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=hq-tls-it-good-ca" \
      -out "$CERT_DIR/ca_good.crt" >/dev/null 2>&1

    openssl genrsa -out "$CERT_DIR/ca_bad.key" 2048 >/dev/null 2>&1
    openssl req -x509 -new -nodes -key "$CERT_DIR/ca_bad.key" -sha256 -days 3650 \
      -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=hq-tls-it-bad-ca" \
      -out "$CERT_DIR/ca_bad.crt" >/dev/null 2>&1

    openssl genrsa -out "$CERT_DIR/server_good.key" 2048 >/dev/null 2>&1
    openssl req -new -key "$CERT_DIR/server_good.key" \
      -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=localhost" \
      -out "$CERT_DIR/server_good.csr" >/dev/null 2>&1
    cat > "$CERT_DIR/server_good.ext" <<'EOF'
subjectAltName=DNS:localhost
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
EOF
    openssl x509 -req -in "$CERT_DIR/server_good.csr" -CA "$CERT_DIR/ca_good.crt" \
      -CAkey "$CERT_DIR/ca_good.key" -CAcreateserial -out "$CERT_DIR/server_good.crt" \
      -days 825 -sha256 -extfile "$CERT_DIR/server_good.ext" >/dev/null 2>&1

    openssl genrsa -out "$CERT_DIR/server_badhost.key" 2048 >/dev/null 2>&1
    openssl req -new -key "$CERT_DIR/server_badhost.key" \
      -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=wronghost.local" \
      -out "$CERT_DIR/server_badhost.csr" >/dev/null 2>&1
    cat > "$CERT_DIR/server_badhost.ext" <<'EOF'
subjectAltName=DNS:wronghost.local
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
EOF
    openssl x509 -req -in "$CERT_DIR/server_badhost.csr" -CA "$CERT_DIR/ca_good.crt" \
      -CAkey "$CERT_DIR/ca_good.key" -CAcreateserial -out "$CERT_DIR/server_badhost.crt" \
      -days 825 -sha256 -extfile "$CERT_DIR/server_badhost.ext" >/dev/null 2>&1

    openssl genrsa -out "$CERT_DIR/client.key" 2048 >/dev/null 2>&1
    openssl req -new -key "$CERT_DIR/client.key" \
      -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=hq-tls-it-client" \
      -out "$CERT_DIR/client.csr" >/dev/null 2>&1
    cat > "$CERT_DIR/client.ext" <<'EOF'
extendedKeyUsage=clientAuth
keyUsage=digitalSignature,keyEncipherment
EOF
    openssl x509 -req -in "$CERT_DIR/client.csr" -CA "$CERT_DIR/ca_good.crt" \
      -CAkey "$CERT_DIR/ca_good.key" -CAcreateserial -out "$CERT_DIR/client.crt" \
      -days 825 -sha256 -extfile "$CERT_DIR/client.ext" >/dev/null 2>&1

    # chmod must leave the docker-mounted key files readable by the mosquitto
    # container user. 0600 (owner-only) files on the host cannot be read by the
    # container's `mosquitto` uid, so the broker fails to start.
    chmod 644 "$CERT_DIR"/*.key
}

write_mosquitto_configs() {
    cat > "$CERT_DIR/mosquitto_ok.conf" <<EOF
listener ${BROKER_OK_PORT} 0.0.0.0
allow_anonymous true
cafile /mosq/certs/ca_good.crt
certfile /mosq/certs/server_good.crt
keyfile /mosq/certs/server_good.key
require_certificate false
EOF

    cat > "$CERT_DIR/mosquitto_badhost.conf" <<EOF
listener ${BROKER_BADHOST_PORT} 0.0.0.0
allow_anonymous true
cafile /mosq/certs/ca_good.crt
certfile /mosq/certs/server_badhost.crt
keyfile /mosq/certs/server_badhost.key
require_certificate false
EOF
}

cleanup() {
    docker rm -f "$BROKER_OK_NAME" >/dev/null 2>&1 || true
    docker rm -f "$BROKER_BADHOST_NAME" >/dev/null 2>&1 || true
}

start_brokers() {
    docker run -d --name "$BROKER_OK_NAME" \
      -p "${BROKER_OK_PORT}:${BROKER_OK_PORT}" \
      -v "$CERT_DIR:/mosq/certs:ro" \
      -v "$CERT_DIR/mosquitto_ok.conf:/mosquitto/config/mosquitto.conf:ro" \
      "$BROKER_IMAGE" >/dev/null

    docker run -d --name "$BROKER_BADHOST_NAME" \
      -p "${BROKER_BADHOST_PORT}:${BROKER_BADHOST_PORT}" \
      -v "$CERT_DIR:/mosq/certs:ro" \
      -v "$CERT_DIR/mosquitto_badhost.conf:/mosquitto/config/mosquitto.conf:ro" \
      "$BROKER_IMAGE" >/dev/null
}

need_cmd cmake
need_cmd docker
need_cmd openssl

trap cleanup EXIT

log "Generating TLS integration certificates"
gen_certs
write_mosquitto_configs

log "Building TLS integration test target"
cmake --build "$BUILD_DIR" --target tb_tls_integration_tests

log "Starting TLS broker containers"
cleanup
start_brokers
sleep 2

log "Running TLS integration test"
TB_TLS_IT_URL_OK="mqtts://localhost:${BROKER_OK_PORT}" \
TB_TLS_IT_URL_HOSTNAME_MISMATCH="mqtts://localhost:${BROKER_BADHOST_PORT}" \
TB_TLS_IT_CA_OK_HOST_PATH="$CERT_DIR/ca_good.crt" \
TB_TLS_IT_CA_BAD_HOST_PATH="$CERT_DIR/ca_bad.crt" \
TB_TLS_IT_CLIENT_CERT_HOST_PATH="$CERT_DIR/client.crt" \
TB_TLS_IT_CLIENT_KEY_HOST_PATH="$CERT_DIR/client.key" \
"$TEST_BIN"

log "TLS integration test completed"
