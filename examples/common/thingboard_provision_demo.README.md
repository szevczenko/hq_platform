# thingboard_provision_demo

This demo mirrors client_provisioning.py behavior.

It demonstrates:

- Sending provisioning request with key/secret
- Validating provisioning response format
- Persisting credentials with OSAL file API
- Reconnecting using persisted credentials (ACCESS_TOKEN and MQTT_BASIC)

Source:

- examples/common/thingboard_provision_demo.c

Shared setup guide:

- examples/common/README_THINGSBOARD_DEMOS.md

## Security Notes

- Do not print or log credentialsValue, tokens, passwords, or private key data.
- Keep provisioning key/secret out of committed files.

## 1. Platform Setup

1. In ThingsBoard, configure device provisioning profile/key/secret.
2. Use provisioning MQTT identity for bootstrap client (usually username provision).
3. Confirm your profile is configured to return supported credentials type.

## 2. Configure And Build (WSL)

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON -DPROV_DEMO_BOOTSTRAP_CLIENT_ID='tb_provision_demo_bootstrap' -DPROV_DEMO_BOOTSTRAP_USERNAME='provision' -DPROV_DEMO_BOOTSTRAP_PASSWORD='' -DPROV_DEMO_BOOTSTRAP_DEVICE_NAME='Provision Bootstrap' -DPROV_DEMO_MQTT_URL='mqtt://127.0.0.1:1883' -DPROV_DEMO_PROVISION_KEY='<PROVISION_KEY>' -DPROV_DEMO_PROVISION_SECRET='<PROVISION_SECRET>' -DPROV_DEMO_DEVICE_NAME='Provisioned Device Demo' -DPROV_DEMO_STORAGE_PATH='tb_provisioned_credentials.json' -DPROV_DEMO_REQUEST_TIMEOUT_MS=10000 -DPROV_DEMO_RECONNECT_DELAY_MS=3000"
```

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target thingboard_provision_demo -j"
```

## 3. Run

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/thingboard_provision_demo"
```

## 4. Expected Logs

- Connecting bootstrap client...
- Provisioning response accepted and stored
- Reconnected using persisted credentials

No secret material is printed.

## 5. Compile-Time Parameters

- PROV_DEMO_BOOTSTRAP_CLIENT_ID
- PROV_DEMO_BOOTSTRAP_USERNAME
- PROV_DEMO_BOOTSTRAP_PASSWORD
- PROV_DEMO_BOOTSTRAP_DEVICE_NAME
- PROV_DEMO_MQTT_URL
- PROV_DEMO_PROVISION_KEY
- PROV_DEMO_PROVISION_SECRET
- PROV_DEMO_DEVICE_NAME
- PROV_DEMO_STORAGE_PATH
- PROV_DEMO_REQUEST_TIMEOUT_MS
- PROV_DEMO_RECONNECT_DELAY_MS
