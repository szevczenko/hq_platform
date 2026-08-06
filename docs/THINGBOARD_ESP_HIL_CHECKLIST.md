# ThingsBoard ESP Hardware-In-The-Loop Checklist

This checklist is for manual validation on real ESP hardware after POSIX unit and integration coverage is green.

## Scope

Validate:

- Wi-Fi loss and recovery
- Broker loss and reconnect behavior
- OTA download resume or safe abort behavior
- Signed OTA policy once signature enforcement is enabled
- Rollback behavior for unconfirmed or failed images
- Secure Boot in release configuration
- Flash Encryption in release configuration

## Preconditions

- ESP target flashed with current demo or production test image
- Device enrolled in ThingsBoard with correct credentials
- Release-like ESP-IDF sdkconfig available for the target under test
- Serial logging captured during every scenario
- Test notes captured with firmware version, board, date, and operator

## Wi-Fi Loss

1. Boot device and verify initial Wi-Fi association succeeds.
2. Remove AP availability or block the configured SSID.
3. Verify the device reports disconnect and does not deadlock.
4. Restore AP availability.
5. Verify automatic Wi-Fi reconnect and MQTT recovery.

## Broker Loss

1. Boot device with healthy network and connected broker.
2. Stop broker service or block port 1883/8883.
3. Verify disconnect callback path and reconnect retry behavior.
4. Restore broker availability.
5. Verify subscriptions and stateful callbacks recover after reconnect.

## OTA Resume Or Abort

1. Start a firmware download from ThingsBoard.
2. Interrupt network during chunk transfer.
3. Reboot the device mid-transfer.
4. Verify persisted OTA state is reported after reboot.
5. Verify the selected policy is applied consistently:
   - clean restart of download, or
   - explicit failed state followed by new request

## Signed OTA

1. Publish firmware metadata with valid checksum and valid signature.
2. Verify update completes only when both checks pass.
3. Repeat with modified payload and original signature.
4. Repeat with wrong key id or wrong public key.
5. Verify image is rejected and active image remains unchanged.

## Rollback

1. Boot a freshly updated image in pending state.
2. Prevent health confirmation from completing.
3. Reboot the device.
4. Verify ESP rollback returns to the previous image.
5. Repeat with checksum or signature failure before boot selection.

## Secure Boot

1. Build and flash a release image with Secure Boot enabled.
2. Verify signed image boots successfully.
3. Attempt to boot tampered or unsigned image.
4. Verify device rejects it and does not enter normal application flow.

## Flash Encryption

1. Build and flash a release image with Flash Encryption enabled.
2. Verify normal boot and MQTT operation.
3. Run OTA update with encrypted flash enabled.
4. Reboot after update and verify booted image is correct.
5. Verify OTA partition contents are protected at rest per platform policy.

## Evidence To Capture

- Serial log
- ThingsBoard latest telemetry and attributes
- Firmware state transitions
- Boot slot / rollback outcome
- Release configuration identifier
