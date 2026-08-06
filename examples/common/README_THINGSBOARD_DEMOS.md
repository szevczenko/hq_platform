# ThingsBoard Demo Onboarding (Shared)

This guide is the common setup for ThingsBoard demos in this repository.
Use it for current and next demos.

## 1. Prerequisites

- Windows host with WSL (Ubuntu recommended)
- Docker Desktop (for local ThingsBoard stack)
- CMake + compiler toolchain in WSL

Repository root in this guide:

- /mnt/c/projekty/hq_platform

## 2. Start Local ThingsBoard (optional, recommended)

From PowerShell:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform/docker/thingboard; docker compose up -d"
```

Default local endpoints from this stack:

- Web UI: http://127.0.0.1:8080
- MQTT: mqtt://127.0.0.1:1883
- MQTT over TLS: mqtts://127.0.0.1:8883

## 3. Create Device In ThingsBoard UI

1. Open http://127.0.0.1:8080 and sign in as tenant admin.
2. Go to Entities -> Devices.
3. Click Add new device.
4. Set a device name (example: ta_demo_device_01).
5. Save.
6. Open the created device.
7. Go to Credentials.
8. Keep credentials type as Access Token.
9. Copy the token value.

Use that token as the demo username/access token.

## 4. Token/Secret Cheat Sheet For Demos

- Telemetry/Attributes demo:
  - Needed: Access Token
  - Used as: username/access token in tb_client_config
- Attributes demo (next):
  - Needed: Access Token
- RPC demo (next):
  - Needed: Access Token
- Claim demo (next):
  - Needed: Access Token, optional claim secret key
- Provision demo (next):
  - Needed: Provision key + provision secret (+ optional token/cert settings)
- TLS demo (next):
  - Needed: Access Token + CA certificate (and optional client cert/key)
- Firmware update demo:
  - Needed: Access Token + firmware package metadata on platform

## 5. Common Build Pattern (POSIX demos)

From PowerShell:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake -S . -B build_wsl -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON"
```

Build one demo target:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; cmake --build build_wsl --target <demo_target> -j"
```

Run one demo binary:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; ./build_wsl/examples/<demo_target>"
```

## 6. Common Runtime Parameters

All ThingsBoard demos in this repo follow this pattern:

- MQTT URL
- Client ID
- Username/access token
- Password (optional depending on demo)
- Device name

Values are passed with CMake -D options and mapped to compile-time macros.

## 7. Platform Side Checks

Before running a demo, verify:

- Device exists and is enabled.
- Access token matches the demo build parameters.
- Broker URL/port is reachable from WSL.
- Device profile permits telemetry and attributes operations.

## 8. Troubleshooting

- Connection loops/reconnect logs:
  - Verify MQTT URL and token.
- Auth rejected:
  - Re-copy device access token from ThingsBoard device credentials.
- UI works but MQTT does not:
  - Confirm port 1883 is exposed and not blocked.
- Docker stack unhealthy:
  - Check container logs under docker/thingboard compose project.
