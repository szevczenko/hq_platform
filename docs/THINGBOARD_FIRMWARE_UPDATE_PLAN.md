# ThingsBoard Firmware Update Plan

## Goal

Add a cross-platform firmware update flow for HQ Platform using ThingsBoard MQTT device APIs, with OTA operations abstracted by OSAL and a dummy backend for POSIX and ESP.

## Scope

- Main target platform: ESP32.
- POSIX is used as a development and test platform.
- Current OTA backend is dummy for both platforms.
- Future extension should allow Zephyr integration by adding another OSAL backend.

## Criteria

1. Use OSAL for OTA operations and keep platform-specific details in OSAL backends.
2. Use existing MQTT transport integration (`mqtt_app.h` via ThingsBoard client layer).
3. Implement ThingsBoard firmware update driver in `src/thingsboard`.
4. Add an example app for update testing, similar to the RGB lamp example style.
5. Support firmware upload and assignment from ThingsBoard server API (default localhost deployment).
6. Run demo application and verify full dummy update flow.
7. Add unit tests where practical.
8. Build and test in WSL using:

```bash
cmake -B build -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON
cmake --build build
cmake -B build -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON
cmake --build build
```

9. Run ESP32 validation after POSIX tests pass.

## Implemented Design (Current Iteration)

### OSAL OTA Layer

- Added public OTA API (`osal_ota.h`) with:
  - `osal_ota_begin`
  - `osal_ota_write`
  - `osal_ota_finish`
  - `osal_ota_abort`
  - `osal_ota_get_progress`
- Added dummy platform backends:
  - POSIX implementation
  - ESP implementation

### ThingsBoard Firmware Update Driver

- Added module `tb_firmware_update` with:
  - initialization and callback registration
  - firmware metadata check request through shared attributes
  - chunked firmware download flow using `v2/fw/request/{reqId}/chunk/{n}` and `v2/fw/response/{reqId}/chunk/{n}`
  - OTA handoff to OSAL API
  - firmware state telemetry (`IDLE`, `DOWNLOADING`, `DOWNLOADED`, `VERIFIED`, `UPDATING`, `UPDATED`, `FAILED`)

### Example App

- Added `thingboard_firmware_update_demo` POSIX example:
  - connects to MQTT broker
  - initializes firmware updater
  - periodically requests firmware metadata
  - processes dummy OTA flow end-to-end

### Unit Tests

- Extended `tb_tests` with firmware update flow test:
  - metadata response simulation
  - chunk response simulation
  - callback validation for successful update

## Manual Demo Procedure (Localhost ThingsBoard)

1. Start ThingsBoard stack in `docker/thingboard` and ensure MQTT + REST are reachable.
2. Create device and copy its access token.
3. Build and run firmware update demo with token and MQTT URL.
4. Upload firmware package in ThingsBoard and assign it to the test device profile/device.
5. Observe demo logs and telemetry state transitions.

## Next Iteration (Real OTA)

1. Replace dummy ESP OSAL backend with real `esp_ota_*` integration.
2. Add checksum verification in the firmware update driver.
3. Persist update metadata/state in non-volatile storage.
4. Add reboot and rollback handling strategy for failed boots.
5. Add Zephyr backend by implementing `osal_ota_*` for Zephyr APIs.

## Definition of Done

- POSIX build with tests passes in WSL.
- POSIX build with examples passes in WSL.
- `tb_tests` firmware update test passes.
- Demo app compiles and runs, and reacts to firmware metadata/chunks.
- ESP build compiles with dummy OTA backend.