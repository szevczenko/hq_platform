# ThingsBoard Compatibility And Profile Requirements

## Supported Server Baseline

The repository is currently validated against the local docker stack defined in:

- docker/thingboard/docker-compose.yml

Current validated local version:

- ThingsBoard CE 4.3.1.3

Expected compatibility target:

- ThingsBoard CE 4.3.x for MQTT device features used by this repository

If a different server version is used, re-run:

- unit tests in tb_tests
- broker integration scripts
- firmware update integration script

## Required Device Profile Permissions By Feature

### Telemetry

Requires:

- device MQTT connectivity enabled
- telemetry ingestion allowed for the device credentials in use

Topics used:

- v1/devices/me/telemetry

### Client Attributes

Requires:

- attribute write permissions for device credentials

Topics used:

- v1/devices/me/attributes

### Shared Attribute Requests And Updates

Requires:

- shared attribute read access
- server-side ability to set shared attributes for test scenarios

Topics used:

- v1/devices/me/attributes/request/+
- v1/devices/me/attributes/response/+
- v1/devices/me/attributes

### Server And Client RPC

Requires:

- server-side RPC enabled for the device profile
- client-side RPC request handling supported by the server

Topics used:

- v1/devices/me/rpc/request/+
- v1/devices/me/rpc/response/+

### Claim

Requires:

- claim feature enabled on the platform side
- optional claim secret configured when secret-based claiming is used

Topic used:

- v1/devices/me/claim

### Provisioning

Requires:

- provisioning enabled in the device profile
- provision device key and provision device secret configured
- bootstrap MQTT identity available for provisioning flow

Topics used:

- /provision/request
- /provision/response

### TLS

Requires:

- MQTT over TLS listener enabled on the broker side
- trusted CA certificate available to the device
- matching hostname verification for the server certificate
- optional client certificate and key when mTLS is required

### Firmware Update

Requires shared firmware metadata attributes:

- fw_title
- fw_version
- fw_checksum
- fw_checksum_algorithm
- fw_size

Recommended state telemetry keys emitted by current implementation:

- fw_state
- fw_error
- current_fw_title
- current_fw_version

## Firmware Authenticity

The primary firmware authenticity mechanism is the platform-native signed
image: ESP Secure Boot application signatures and, in future, MCUboot image
signatures. `fw_checksum` is calculated over the final signed binary and checks
transport integrity; it is not an authenticity mechanism.

## Optional Custom OTA Signature Attributes

An additional detached-signature/envelope policy is not currently required.
The reserved project-owned attributes are:

- fw_signature
- fw_signature_algorithm
- fw_signing_key_id

Expected semantics:

- fw_signature: detached signature over the final firmware digest
- fw_signature_algorithm: algorithm identifier such as ECDSA_P256_SHA256 or ED25519
- fw_signing_key_id: identifier for selecting the trusted verification key in firmware

When signature verification is enabled, checksum and signature must both pass before image application.

## CI And Automation Assumptions

Current automation assumes:

- tenant-admin credentials can create or update devices in local test environments
- disposable broker or ThingsBoard containers are allowed
- MQTT listener endpoints are reachable from the POSIX test runtime
