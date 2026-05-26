#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CERT_DIR="$PROJECT_DIR/cert"

mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
  -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=hq-mqtt-test-ca" \
  -out ca.crt

openssl genrsa -out server.key 2048
openssl req -new -key server.key \
  -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=localhost" \
  -out server.csr
cat > server.ext <<'EOF'
subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
EOF
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 825 -sha256 -extfile server.ext

openssl genrsa -out client.key 2048
openssl req -new -key client.key \
  -subj "/C=PL/ST=Mazowieckie/L=Warsaw/O=HQ Platform/OU=QA/CN=hq-mqtt-client" \
  -out client.csr
cat > client.ext <<'EOF'
extendedKeyUsage=clientAuth
keyUsage=digitalSignature,keyEncipherment
EOF
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt -days 825 -sha256 -extfile client.ext

chmod 600 *.key

echo "Generated MQTT test certificates in $CERT_DIR"
