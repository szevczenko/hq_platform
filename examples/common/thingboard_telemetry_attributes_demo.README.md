# thingboard_telemetry_attributes_demo

This demo sends:

- Scalar telemetry (int, double, bool, string)
- Timestamped telemetry JSON
- Client attributes
- Small telemetry batch payload

It also includes a QoS publish example and logs publish failures.

Source:

- examples/common/thingboard_telemetry_attributes_demo.c

Shared setup guide:

- examples/common/README_THINGSBOARD_DEMOS.md

## 1. Create Device And Get Token

In ThingsBoard UI:

1. Open http://127.0.0.1:8080
2. Sign in as tenant admin
3. Create device (example name: ta_demo_device_01)
4. Open device -> Credentials
5. Copy Access Token

This Access Token is used as CONFIG_TA_DEMO_USERNAME.

## 2. Configure And Build (WSL on Windows)

From PowerShell:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON -DTA_DEMO_CLIENT_ID=ta_demo_client_01 -DTA_DEMO_USERNAME=<ACCESS_TOKEN> -DTA_DEMO_PASSWORD='' -DTA_DEMO_DEVICE_NAME='Telemetry Attributes Demo' -DTA_DEMO_MQTT_URL='mqtt://127.0.0.1:1883' -DTA_DEMO_SEND_PERIOD_MS=7000 -DTA_DEMO_RECONNECT_DELAY_MS=3000 -DTA_DEMO_ENABLE_QOS_DEMO=1"
```

Then build target:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target thingboard_telemetry_attributes_demo -j"
```

## 3. Run Demo

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/thingboard_telemetry_attributes_demo"
```

Expected logs include:

- Connecting...
- Connected
- scalar telemetry sent
- timestamped telemetry JSON sent
- client attributes JSON sent
- small telemetry batch sent

## 4. Verify In ThingsBoard

1. Open device -> Latest telemetry.
2. Check keys like temperature, pressure_hpa, fan_enabled, mode, cpu, ram_kb.
3. Open device -> Latest attributes.
4. Check keys like hw_model, serial, fw_version, sdk.

## 5. Compile-Time Parameters

- TA_DEMO_CLIENT_ID
- TA_DEMO_USERNAME (Access Token)
- TA_DEMO_PASSWORD
- TA_DEMO_DEVICE_NAME
- TA_DEMO_MQTT_URL
- TA_DEMO_SEND_PERIOD_MS
- TA_DEMO_RECONNECT_DELAY_MS
- TA_DEMO_ENABLE_QOS_DEMO

## 6. Notes

- QoS demo uses QoS1 publish directly to telemetry topic.
- Publish failure handling is currently done via return code checks and logs.
- Keep token values out of committed files and command history where possible.
