# thingboard_tls_demo

This demo mirrors tls_connect.py behavior.

It demonstrates:

- Connecting with mqtts:// and CA verification enabled
- Optional client certificate and key configuration
- Expected TLS failure for unknown CA
- Expected TLS failure for hostname mismatch

Source:

- examples/common/thingboard_tls_demo.c

Shared setup guide:

- examples/common/README_THINGSBOARD_DEMOS.md

## 1. Platform Setup

1. Create device in ThingsBoard and copy Access Token.
2. Prepare CA certificate file in OSAL FS path used by this demo.
3. Optional: provide client certificate and client key if your broker requires mTLS.
4. For failure scenarios, prepare:
   - a CA that does not sign the broker cert (unknown CA)
   - an alternative broker endpoint with certificate hostname mismatch

## 2. Configure And Build (WSL)

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON -DTLS_DEMO_CLIENT_ID='tb_tls_demo_0001' -DTLS_DEMO_USERNAME='<ACCESS_TOKEN>' -DTLS_DEMO_PASSWORD='' -DTLS_DEMO_DEVICE_NAME='TLS Demo Device' -DTLS_DEMO_MQTT_URL='mqtts://localhost:8883' -DTLS_DEMO_CA_CERT_PATH='ca.crt' -DTLS_DEMO_CLIENT_CERT_PATH='' -DTLS_DEMO_CLIENT_KEY_PATH='' -DTLS_DEMO_UNKNOWN_CA_PATH='ca_unknown.crt' -DTLS_DEMO_HOSTNAME_MISMATCH_URL='mqtts://localhost:8884' -DTLS_DEMO_CONNECT_TIMEOUT_MS=10000 -DTLS_DEMO_RECONNECT_DELAY_MS=3000"
```

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target thingboard_tls_demo -j"
```

## 3. Run

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/thingboard_tls_demo"
```

## 4. Expected Logs

- Scenario: verified tls -> TLS connection established with verification
- Scenario: unknown CA failure -> Expected TLS failure observed
- Scenario: hostname mismatch failure -> Expected TLS failure observed

## 5. Compile-Time Parameters

- TLS_DEMO_CLIENT_ID
- TLS_DEMO_USERNAME (Access Token)
- TLS_DEMO_PASSWORD
- TLS_DEMO_DEVICE_NAME
- TLS_DEMO_MQTT_URL
- TLS_DEMO_CA_CERT_PATH
- TLS_DEMO_CLIENT_CERT_PATH (optional)
- TLS_DEMO_CLIENT_KEY_PATH (optional)
- TLS_DEMO_UNKNOWN_CA_PATH (optional)
- TLS_DEMO_HOSTNAME_MISMATCH_URL (optional)
- TLS_DEMO_CONNECT_TIMEOUT_MS
- TLS_DEMO_RECONNECT_DELAY_MS
