# thingboard_claim_demo

This demo mirrors claiming_device_pe_only.py behavior.

It demonstrates:

- Claim request with secret key
- Claim request without secret key
- Reporting claim call result via telemetry and client attributes

Source:

- examples/common/thingboard_claim_demo.c

Shared setup guide:

- examples/common/README_THINGSBOARD_DEMOS.md

## 1. Platform Setup

1. Create device in ThingsBoard UI.
2. Copy device Access Token.
3. If your tenant/profile requires claim secret, set matching secret.

## 2. Configure And Build (WSL)

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON -DCLAIM_DEMO_CLIENT_ID=claim_demo_client_01 -DCLAIM_DEMO_USERNAME=<ACCESS_TOKEN> -DCLAIM_DEMO_PASSWORD='' -DCLAIM_DEMO_DEVICE_NAME='Claim Demo' -DCLAIM_DEMO_MQTT_URL='mqtt://127.0.0.1:1883' -DCLAIM_DEMO_SECRET='demo_secret_key' -DCLAIM_DEMO_DURATION_SECRET_MS=60000 -DCLAIM_DEMO_DURATION_NO_SECRET_MS=30000 -DCLAIM_DEMO_PERIOD_MS=30000 -DCLAIM_DEMO_RECONNECT_DELAY_MS=3000"
```

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target thingboard_claim_demo -j"
```

## 3. Run

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/thingboard_claim_demo"
```

## 4. Expected Logs

- claim with secret rc=...
- claim without secret rc=...

Telemetry keys sent:

- claim_mode
- claim_rc

Client attribute key sent:

- last_claim_mode

## 5. Compile-Time Parameters

- CLAIM_DEMO_CLIENT_ID
- CLAIM_DEMO_USERNAME
- CLAIM_DEMO_PASSWORD
- CLAIM_DEMO_DEVICE_NAME
- CLAIM_DEMO_MQTT_URL
- CLAIM_DEMO_SECRET
- CLAIM_DEMO_DURATION_SECRET_MS
- CLAIM_DEMO_DURATION_NO_SECRET_MS
- CLAIM_DEMO_PERIOD_MS
- CLAIM_DEMO_RECONNECT_DELAY_MS
