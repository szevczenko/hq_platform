# thingboard_attributes_demo

This demo mirrors request_attributes.py and subscription_to_attrs.py behavior.

It demonstrates:

- Requesting client attributes
- Requesting shared attributes
- Subscribing to all shared attribute updates
- Optional per-key shared callbacks (threshold, mode)

Source:

- examples/common/thingboard_attributes_demo.c

Shared setup guide:

- examples/common/README_THINGSBOARD_DEMOS.md

## 1. Platform Setup

1. Create device in ThingsBoard UI.
2. Copy device Access Token.
3. Set shared attributes on the device (for example):
   - threshold: 42
   - mode: auto

## 2. Configure And Build (WSL)

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON -DATTR_DEMO_CLIENT_ID=attr_demo_client_01 -DATTR_DEMO_USERNAME=<ACCESS_TOKEN> -DATTR_DEMO_PASSWORD='' -DATTR_DEMO_DEVICE_NAME='Attributes Demo' -DATTR_DEMO_MQTT_URL='mqtt://127.0.0.1:1883' -DATTR_DEMO_REQUEST_PERIOD_MS=10000 -DATTR_DEMO_RECONNECT_DELAY_MS=3000 -DATTR_DEMO_ENABLE_KEY_SUBS=1"
```

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target thingboard_attributes_demo -j"
```

## 3. Run

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/thingboard_attributes_demo"
```

## 4. Expected Logs

- Connected
- shared subscriptions active
- client request response: {...}
- shared request response: {...}
- shared update: {...}
- shared key update [threshold] ...
- shared key update [mode] ...

## 5. Compile-Time Parameters

- ATTR_DEMO_CLIENT_ID
- ATTR_DEMO_USERNAME (Access Token)
- ATTR_DEMO_PASSWORD
- ATTR_DEMO_DEVICE_NAME
- ATTR_DEMO_MQTT_URL
- ATTR_DEMO_REQUEST_PERIOD_MS
- ATTR_DEMO_RECONNECT_DELAY_MS
- ATTR_DEMO_ENABLE_KEY_SUBS
