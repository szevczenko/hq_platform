# Secure Boot And OTA Architecture

## Scope And Ownership

`hq_platform` integrates vendor-native image verification, OTA slots, rollback,
boot confirmation, encryption status, and test tooling behind OSAL. It does not
implement a bootloader or own production keys.

The product owns private signing keys or HSM access, release approval, partition
policy, security-version increments, manufacturing, eFuse provisioning, and
recovery policy. No production private key belongs in this repository.

## Artifact Contract

For ESP, upload the final Secure Boot-signed application binary to ThingsBoard.
The firmware checksum and size must be calculated from that exact signed file.
The OTA transport supplies plaintext signed-image bytes to `esp_ota_write()`.
When flash encryption is active, ESP-IDF encrypts those bytes while writing the
OTA app partition.

Do not pass a host-pre-encrypted partition image to `esp_ota_write()`. Flash
ciphertext is tied to a device key and flash offset and would be encrypted again.
Use MQTTS with certificate and hostname verification for confidentiality in
transit. Flash encryption protects data at rest, not the network transfer.

SHA-256 remains a transport-integrity check. Image authenticity is provided by
the ESP native signature or, in future, the MCUboot signature.

## Portable API

`osal_ota_get_security_info()` reports:

- hardware Secure Boot enforcement;
- flash encryption state;
- rollback and anti-rollback configuration;
- normalized running-image boot state.

The POSIX backend reports no hardware enforcement and supports contract testing.
The ESP backend reads runtime security state without changing eFuses.

Future Zephyr support should map the same concepts to MCUboot image signatures,
primary/secondary slots, test upgrades, confirmation, and rollback. DTS,
Partition Manager, MCUboot keys, and trailer details remain backend/product
concerns.

## ESP Demo Profiles

The firmware-update demo uses `sdkconfig.defaults` as its safe default and has
profile deltas under `examples/esp/thingboard_fwu/profiles/`:

- `dev`: current unsigned development behavior; no security eFuse changes.
- `signed-ota-dev`: verifies RSA-signed OTA applications without hardware Secure
  Boot; no security eFuse changes.
- `flash-encryption-dev`: enables flash encryption Development mode; first boot
  permanently changes eFuses.
- `production-secure`: build-only Secure Boot v2 plus Release-mode flash
  encryption template; never flash this profile to the sole development board.

The encryption profiles use `partitions_ota_secure.csv`, which moves the
partition table to `0x10000` and the first app to `0x20000`. Secure Boot and
flash encryption increase bootloader size beyond the normal `0x8000` partition
table boundary. Do not combine an encryption profile with the normal layout.

Use a separate build directory and sdkconfig for each profile. For example:

```bash
cd examples/esp/thingboard_fwu
idf.py -B build_signed \
  -DSDKCONFIG=sdkconfig.signed-ota-dev \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;profiles/sdkconfig.signed-ota-dev" \
  build
```

The signed profile deliberately disables signing during build. Sign the padded
application externally with ESP-IDF tooling or an HSM, then calculate the
ThingsBoard checksum and size from the signed output.

## Device Safety

Hardware Secure Boot is irreversible after its enable eFuse is burned. Future
firmware must match the trusted signing-key digest.

Flash encryption Development mode is not reversible to factory state. It burns
and protects a device encryption key, changes security/debug eFuses, and permits
only a finite number of plaintext reflash/encryption cycles. Release mode is
more restrictive and is treated as production-only.

Anti-rollback consumes one-way security-version eFuse bits and can permanently
reject older images.

No normal build or demo command may burn eFuses. Any eFuse-changing HIL script
must require an explicit `--confirm-irreversible` option and archive before/after
state.

## Unit And Integration Tests

Required host coverage:

- checksum success and mismatch;
- native image-validation and security-version error mapping;
- security capability and boot-state queries;
- pending-image confirmation and rollback behavior;
- interrupted update state;
- deferred reboot sequencing;
- ThingsBoard error telemetry;
- existing OTA, reconnect, TLS, and provisioning regressions.

POSIX tests model policy and state only; they must never claim hardware security.

## ESP HIL On The Current Board

The explicit device is `/dev/ttyUSB1`.

Before any eFuse-changing operation:

```bash
idf.py -p /dev/ttyUSB1 efuse-summary
```

Archive the output, chip revision/MAC, partition table, build artifacts, and
recovery instructions.

Safe profiles may be flashed with:

```bash
idf.py flash -p /dev/ttyUSB1
```

For `flash-encryption-dev`, require explicit irreversible-operation approval,
stable power, and the archived pre-state before flashing. Do not interrupt the
first encryption boot. Capture the complete monitor log and then archive:

```bash
idf.py -p /dev/ttyUSB1 efuse-summary
```

Verify a signed plaintext OTA is accepted, the inactive OTA slot is ciphertext
at rest, the image boots and confirms health, rollback remains possible, and a
second OTA still works. Record remaining Development-mode plaintext reflash
capacity.

Do not enable hardware Secure Boot, Release-mode encryption, or anti-rollback on
the only board. Those tests require disposable or production-intended hardware.

## Future Zephyr Backend

A future `src/osal/zephyr/osal_ota_impl.c` should use MCUboot-native signed
images and map:

- secondary-slot upload to OTA begin/write/verify;
- test upgrade request to apply;
- image confirmation to OSAL confirmation;
- MCUboot revert state to pending/invalid boot state;
- MCUboot security counters to anti-rollback capability.

The common API must not expose ESP partition addresses, eFuse fields, MCUboot
trailers, or signing-key file formats.
