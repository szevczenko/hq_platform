# ThingsBoard Device SDK Parity Plan

## Purpose

This document tracks the work needed to evolve `src/thingsboard` into a
production-ready embedded device client comparable to the ThingsBoard Python
device MQTT SDK. It complements
[THINGBOARD_FIRMWARE_UPDATE_PLAN.md](THINGBOARD_FIRMWARE_UPDATE_PLAN.md), which
documents the initial firmware-update implementation.

The project already supports MQTT connection management, telemetry, client
attributes, shared-attribute requests and updates, server and client RPC,
claiming, provisioning, and chunked firmware download. The work below closes
the reliability and security gaps, then adds an example for every public
device-client capability.

## Priorities

### P0 - Firmware Trust And Recovery

- [ ] **Require a firmware signature in addition to the checksum.**
  - A checksum detects accidental corruption but does not authenticate a
    firmware publisher.
  - Define project-owned shared attributes such as `fw_signature`,
    `fw_signature_algorithm`, and `fw_signing_key_id`; ThingsBoard's standard
    firmware metadata does not provide a universal image-signature contract.
  - Verify a detached signature over the final image digest with a public key
    compiled into the firmware or stored in protected key storage. Prefer
    ECDSA P-256 or Ed25519 if the chosen ESP-IDF/crypto configuration supports
    it; document the selected algorithm and key-rotation format.
  - Treat a checksum as an integrity check and the signature as the
    authenticity check. Both must pass before `osal_ota_finish(true)`.
  - Unit tests: correct signature; modified payload; wrong public key; unknown
    key ID; malformed signature; signature verification failure must leave the
    active boot partition unchanged.

- [ ] **Enable and validate ESP Secure Boot and Flash Encryption for release
  builds.**
  - ESP Secure Boot verifies the signed application image during boot and is
    the final anti-tamper control. Configure it in the ESP-IDF production
    `sdkconfig` and use the IDF signing workflow; do not implement a second
    incompatible application-image signature format without a documented
    reason.
  - Flash Encryption protects firmware stored on the device at rest. Confirm
    the OTA partition layout and `esp_ota_*` flow work when flash encryption
    is enabled.
  - This does **not** encrypt the MQTT download in transit. Use `mqtts://`, a
    trusted CA, hostname verification, and client credentials where required.
  - If encrypted firmware packages are needed before they reach the device,
    define a separate envelope format and key-provisioning model. Do not reuse
    the TLS or flash-encryption keys. Decrypt incrementally before hashing and
    writing, authenticate the encrypted envelope, and zeroize temporary key
    material.
  - Hardware validation: boot a signed image, reject an unsigned/tampered
    image, apply a signed OTA image, force a failed boot, verify rollback, and
    verify encrypted OTA slots across reset.

- [ ] **Finalize OTA health-confirmation deadline behavior.**
  - A health milestone and explicit confirmation flow are implemented.
  - Add a deadline; if the milestone is not met, leave the image pending so
    ESP-IDF rollback can restore the prior image.
  - Integration test: boot a pending image with forced network/MQTT failure
    and verify it is not confirmed.

- [ ] **Complete power-loss/restart validation for persisted OTA state.**
  - OTA state persistence and restart behavior are implemented.
  - Test reset/power-loss at begin, mid-chunk, post-download, and post-boot
    partition selection.

### OTA Validation Checklist

Use this checklist to validate the implemented checksum, health-confirmation,
and OTA-state persistence behavior against a real ThingsBoard deployment and a
real device target.

#### ThingsBoard Platform Validation

- [ ] Create or update a device profile that exposes `fw_title`,
  `fw_version`, `fw_checksum`, `fw_checksum_algorithm`, and `fw_size` shared
  attributes.
- [ ] Upload a firmware binary whose SHA-256 digest matches the configured
  `fw_checksum` value.
- [ ] Trigger an OTA update and verify the device reports:
  `DOWNLOADING` -> `DOWNLOADED` -> `VERIFIED` -> `UPDATING` -> `UPDATED`.
- [ ] Publish a bad checksum and verify the device reports `FAILED` with a
  checksum-specific error and does not reboot into the new image.
- [ ] Publish malformed checksum metadata and verify no chunk download starts.
- [ ] Publish an unsupported checksum algorithm and verify no chunk download
  starts.
- [ ] Interrupt chunk delivery mid-transfer, reboot the device, and verify the
  restarted session reports the persisted failed/interrupted state before a new
  clean download is requested.

#### Real Target Validation

- [ ] Boot a newly updated image in `PENDING_VERIFY` state and verify it is
  not confirmed immediately at startup.
- [ ] Reach the configured health milestone on the real target and verify the
  image is confirmed only after successful telemetry publish.
- [ ] Force network or MQTT failure after booting a pending image and verify
  the health milestone is not reached.
- [ ] Add and validate a deadline policy so a pending image that never reaches
  health confirmation remains rollback-eligible.
- [ ] Power-cycle or reset the target during these phases and verify the
  persisted OTA state is reported correctly after reboot:
  begin, mid-chunk, post-download, post-verify, and post-boot-partition
  selection.
- [ ] Verify the selected restart policy is applied consistently after reboot:
  clean restart of download, safe resume if ever implemented, or explicit
  failure and restart.
- [ ] On ESP hardware, verify checksum failure never results in the new boot
  partition being selected.
- [ ] On ESP hardware, verify rollback returns to the previous image when a
  pending image is not confirmed.

## P1 - Client Reliability And Protocol Behavior

- [ ] **Adopt connection-state callbacks across demos.**
  - Public connect/disconnect/connection-failure callbacks with user-data and
    reason codes are implemented and covered by unit tests.
  - Use the callbacks in demos to publish connection state and in the OTA
    demo to drive health confirmation.

- [ ] **Implement request deadlines and timeout callbacks.**
  - Attribute and client-RPC requests need explicit timeout tracking rather
    than retaining pending slots indefinitely.
  - Add a portable timer-driven cleanup path. Call the API callback with a
    result/error status; preserve the JSON callback payload for successful
    results.
  - Add cancellation on client deinitialization and disconnect.
  - Unit tests: response before deadline, response after deadline, timeout
    slot reuse, cancellation, and simultaneous requests up to
    `TB_MAX_PENDING_REQUESTS`.

- [ ] **Expose QoS, keepalive, and reconnect policy through the client API.**
  - Allow a caller to select QoS 0 or 1 per publish/subscribe or through
    defaults in `tb_client_config_t`.
  - Make MQTT keepalive, initial reconnect delay, maximum reconnect delay,
    and backoff behavior configurable with bounded defaults.
  - Ensure the reconnect path does not call `mqtt_app_deinit()` merely because
    a connection attempt is still in progress.
  - Tests: QoS passed to the MQTT adapter, bounded exponential backoff, and
    resubscription after reconnect.

- [ ] **Enforce ThingsBoard session limits and payload bounds.**
  - Request `getSessionLimits` by client-side RPC after a successful connect.
  - Parse server limits for message rate, telemetry rate, telemetry data
    points, maximum payload size, and maximum inflight messages.
  - Implement bounded queues and a token-bucket limiter suitable for an MCU.
    Reject or defer data rather than blocking the Mongoose event thread.
  - Split telemetry and attribute batches by configured payload/data-point
    limits while preserving timestamp and metadata semantics.
  - Unit tests: parser validation, queue saturation, limiter timing through a
    fake clock, payload splitting, and server-limit changes after reconnect.

- [ ] **Improve attribute subscription semantics.**
  - Keep the existing all-shared-attributes callback for a small API.
  - Add optional per-key subscriptions and independent subscription handles,
    allowing multiple consumers without singleton callback replacement.
  - Define callback ownership and safe removal during callback execution.
  - Unit tests: wildcard and per-key dispatch, multiple callbacks, removal,
    and reconnect restoration.

## P2 - Public API And Demo Coverage

Create one small, independently buildable POSIX demo in `examples/common` and
one ESP wrapper under `examples/esp/<demo>/` where hardware/network behavior is
meaningful. Add the POSIX targets to `examples/posix/CMakeLists.txt`. Each demo
must support compile-time overrides for MQTT URL, client ID, token/password,
and device name, and must log its connection lifecycle.

- [ ] **`thingboard_telemetry_attributes_demo`**
  - Send scalar telemetry, timestamped telemetry JSON, client attributes, and
    a small telemetry batch.
  - Demonstrate QoS and publish-failure handling once the P1 API exists.
  - Mirrors Python: `send_telemetry_and_attr.py`, `send_telemetry_pack.py`,
    and `hardware_specs_sender.py`.
  - Tests: JSON topic/payload shape and split batches.

- [ ] **`thingboard_attributes_demo`**
  - Request client and shared attributes, subscribe to all updates, and show
    optional per-key callback behavior.
  - Mirrors Python: `request_attributes.py` and
    `subscription_to_attrs.py`.
  - Tests: response matching, timeout, callback dispatch, and reconnection.

- [ ] **`thingboard_rpc_demo`**
  - Receive a server-side RPC, validate method/parameters, reply, then issue
    a client-side RPC and handle the asynchronous response or timeout.
  - Mirrors Python: `client_rpc_request.py` plus server-RPC handling.
  - Tests: malformed JSON, request ID handling, timeout, and reconnect.

- [ ] **`thingboard_claim_demo`**
  - Claim a device with and without a secret key and report the resulting
    telemetry/attributes.
  - Mirrors Python: `claiming_device_pe_only.py`.
  - Tests: request JSON shape and publish failure.

- [ ] **`thingboard_provision_demo`**
  - Request credentials with the provisioning token, validate the response,
    store credentials through an OSAL secure/persistent-storage interface, and
    reconnect as the provisioned device.
  - Mirrors Python: `client_provisioning.py`.
  - Never log tokens, passwords, or private key material.
  - Tests: each credentials type, invalid responses, storage failure, and
    reconnect using persisted credentials.

- [ ] **`thingboard_tls_demo`**
  - Use `mqtts://` with CA verification and an optional client certificate.
  - Demonstrate expected failure for an unknown CA or hostname mismatch.
  - Mirrors Python: `tls_connect.py`.
  - Tests: adapter configuration and a local TLS broker integration test.

- [ ] **`thingboard_firmware_update_demo` enhancement**
  - Retain the current POSIX and ESP demos but add checksum/signature state
    reporting, TLS configuration, persisted state, and post-boot health
    confirmation.
  - Mirrors Python: `firmware_update.py`.
  - Add an ESP test procedure using a real partition table, Secure Boot/Flash
    Encryption production configuration, and a controlled rollback scenario.

## Shared Test And Delivery Work

- [ ] Keep `tests/thingsboard/tb_tests.c` as the unit-test entry point, but
  split it into focused source files when module coverage becomes difficult to
  navigate.
- [ ] Add deterministic MQTT adapter mocks for connect/disconnect, SUBACK,
  PUBACK, delayed responses, dropped connections, and subscription replay.
- [ ] Add a POSIX integration test job against a disposable ThingsBoard and
  MQTT broker deployment. Cover telemetry, attributes, RPC, claim,
  provisioning, TLS, and OTA checksum/signature rejection.
- [ ] Add an ESP hardware-in-the-loop checklist for Wi-Fi loss, broker loss,
  OTA resume/abort, signed OTA, rollback, Secure Boot, and Flash Encryption.
- [ ] Document the supported ThingsBoard server version, required device
  profile permissions, and custom OTA signature attributes.

## Definition Of Done

- [ ] Every public ThingsBoard module has unit tests for success, malformed
  input, transport failure, timeout, and reconnect where applicable.
- [ ] Every feature in the demo matrix builds for POSIX; applicable demos also
  build under ESP-IDF.
- [ ] Firmware installation requires checksum and signature validation before
  boot partition selection.
- [ ] ESP release configuration validates Secure Boot, Flash Encryption, TLS
  server authentication, and rollback health confirmation.
- [ ] POSIX unit tests and the ThingsBoard integration suite pass in CI.

## CI Job Setup (Current State: Manual Testing)

At the moment, verification is run manually. Add CI jobs to automate the same
checks.

- [ ] Create a CI job for POSIX build (`cmake -S . -B build_posix`).
- [ ] Create a CI job for ThingsBoard unit tests (`tb_tests`).
- [ ] Create a CI job for broker reconnect integration
  (`tb_reconnect_integration_tests`) using a disposable MQTT broker service.
- [ ] Create a CI job for firmware update integration script(s) against local
  ThingsBoard docker stack.
- [ ] Publish test logs/artifacts from CI for failed runs.
- [ ] Keep manual test scripts as a local fallback until CI is stable.