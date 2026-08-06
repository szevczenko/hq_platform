# thingboard_rpc_demo

This demo mirrors client_rpc_request.py plus server-side RPC handling.

It demonstrates:

- Receiving server-side RPC requests
- Validating method/params and replying with JSON
- Sending client-side RPC requests
- Handling asynchronous client-side success and timeout callbacks

Source:

- examples/common/thingboard_rpc_demo.c

Shared setup guide:

- examples/common/README_THINGSBOARD_DEMOS.md

## 1. Platform Setup

1. Create device in ThingsBoard UI.
2. Copy device Access Token.
3. Optional for quick server-RPC testing:
   - create an RPC command from UI with method getStatus
   - or method setValue and params {"value":123}

## 2. Configure And Build (WSL)

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON -DRPC_DEMO_CLIENT_ID=rpc_demo_client_01 -DRPC_DEMO_USERNAME=<ACCESS_TOKEN> -DRPC_DEMO_PASSWORD='' -DRPC_DEMO_DEVICE_NAME='RPC Demo' -DRPC_DEMO_MQTT_URL='mqtt://127.0.0.1:1883' -DRPC_DEMO_REQUEST_PERIOD_MS=15000 -DRPC_DEMO_REQUEST_TIMEOUT_MS=5000 -DRPC_DEMO_RECONNECT_DELAY_MS=3000"
```

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target thingboard_rpc_demo -j"
```

## 3. Run

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/thingboard_rpc_demo"
```

## 4. Expected Logs

- server RPC subscription active
- server RPC: method=... request_id=... params=...
- client RPC request sent: method=getServerTime
- client RPC response for getServerTime: ...
- client RPC result for getServerTime: 1 (timeout case)

## 5. Compile-Time Parameters

- RPC_DEMO_CLIENT_ID
- RPC_DEMO_USERNAME (Access Token)
- RPC_DEMO_PASSWORD
- RPC_DEMO_DEVICE_NAME
- RPC_DEMO_MQTT_URL
- RPC_DEMO_REQUEST_PERIOD_MS
- RPC_DEMO_REQUEST_TIMEOUT_MS
- RPC_DEMO_RECONNECT_DELAY_MS
