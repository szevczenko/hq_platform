# Wi-Fi Logging Policy — No Credential Logs (TASK-120)

## Guarantee

The platform Wi-Fi stack never writes station credential material to any
log output. This guarantee covers **product operation logs**, not just the
platform sources' own format strings: because the product (e.g. the LED
lamp adapter) invokes these platform code paths, every statement below is
binding on the resulting product logs as well.

Concretely, no Wi-Fi log statement may emit:

- the **station SSID** of the configured credential set (including the
  SSID currently associated with, or last associated with, the station
  interface),
- the **password / pre-shared key**, in any form,
- the **password length** or any other derived value that leaks the
  credential (e.g. `pass_len`),
- the **BSSID** of the station credential set,
- authentication tokens or the contents of the persisted-credential
  document (`wifi_ap.json` / the credential list JSON).

## What may still be logged

Non-sensitive transition, status, and diagnostic information remains
allowed and expected:

- lifecycle events (init, deinit, start, stop, mode changes),
- connect/disconnect state transitions and error codes
  (e.g. `esp_wifi_set_config failed: 0x…`),
- simulated-environment behaviour classes and timing parameters
  (behaviour enum, disconnect/slow-connect delays, simulated IP),
- scan result counts and — where the provisioning user experience
  requires it — SSIDs of **foreign** access points returned by a scan
  (these are never station credentials).

## Audited scope

The following platform sources were audited for this guarantee:

| File | Result |
| --- | --- |
| `src/wifi/platforms/esp/wifi_hal_driver.c` | `wifi_hal_connect()` no longer logs SSID or password length; only a plain `"connect requested"` transition line remains. |
| `src/wifi/platforms/posix/wifi_hal_driver.c` | All SSID-bearing logs removed from `set_sta_config`, `set_ap_config`, `wifi_hal_connect` (including the not-found, wrong-password, connected, disconnect-timer and slow-connect paths) and `wifi_hal_disconnect`. |
| `src/wifi/wifi_managment.c` | Logs state transitions, error codes and counters only; no credential values. |
| `src/wifi/wifi_config.c` | Logs counts and error codes only; the persisted credential JSON document is never dumped to the log. |
| `src/wifi_provisioning/*` | HTTP routes never include or log a Wi-Fi credential (see module header comment); status payloads expose SSID/ip only as documented API responses, not log output. |

## Regression check

The host test
`tests/wifi/wifi_hal_posix_test.c :: test_connect_logs_contain_no_credentials`
feeds a known SSID/password through the real POSIX HAL connect request path
(success and not-found variants), captures the HAL log output, and asserts
the capture contains neither the SSID, the password, nor `pass_len`, while a
positive control proves non-sensitive log lines were actually captured.

Run it with the POSIX platform test build:

```bash
cd platform/hq_platform
cmake -B build_posix -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON
cmake --build build_posix --target wifi_hal_posix_tests
./build_posix/tests/wifi/wifi_hal_posix_tests
```

## On-target verification (ESP32, manual)

For the ESP target, verify on the serial console after a provisioning and
reboot round:

1. Build and flash the product with an obviously recognizable test
   credential, e.g. SSID `CRED_AUDIT_SSID` and password
   `CRED_AUDIT_PASSWORD_0123`.
2. Attach the serial monitor: `idf.py -p /dev/ttyUSB1 monitor` and save the
   full log from power-on through a successful connect (and through a
   provisioning submit + reboot, so the saved-credential auto-connect path
   is exercised).
3. Verify the capture:
   ```bash
   grep -e 'CRED_AUDIT_SSID' -e 'CRED_AUDIT_PASSWORD' -e 'pass_len' \
        <saved serial log> && echo "FAIL: credential in log" || echo "OK"
   ```
   The grep must not match. Note that the provisioning portal response body
   legitimately contains the SSID the *user submitted* — capture serial
   output only, not HTTP traffic, for this check.
4. Repeat with a deliberately wrong password; the log may report
   authentication failure/error codes but must not repeat the SSID or
   password.

## Changing Wi-Fi code

Any new Wi-Fi log statement must follow the same rule: log the event, mode,
or error code — never SSID, password, password length, or the station
credential set's BSSID. When in doubt, leave the identifying value out and
reference it by index or state instead.
