# Supported-Client Matrix — Wi-Fi HTTP Provisioning (Template)

Template for recording which real client OS/device combinations successfully
detect and complete the ESP32 Wi-Fi HTTP provisioning captive portal
(`examples/esp/wifi_provisioning_demo`, `TASK-131`).

This file is a **template**: copy it per run, fill one row per client, and keep
the checklists in `docs/ESP_WIFI_PROVISIONING_HIL_CHECKLIST.md` as the source of
the steps and expected results.

| Item | Value |
|------|-------|
| Execution status | **DEFERRED** — not executed as part of the automated plan |
| Blocking | **NON-BLOCKING** — results do not gate any automated task |
| Physical ESP32 required | **No** — completing or updating this template requires no board |
| Checklist reference | `docs/ESP_WIFI_PROVISIONING_HIL_CHECKLIST.md` |

## Run identity

| Field | Value |
|-------|-------|
| Board | |
| Firmware revision (git SHA / build id) | |
| Defconfig | `defconfig/esp_provisioning.defconfig` (or delta) |
| AP SSID / IP | `Bimbrownik` / `10.10.0.1` |
| Grace period (ms) | `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS` (default 30000) |
| Operator / date | |

## Matrix columns

| Client OS / version | Device model | Captive probe hit | Portal opened (canned browser) | Scan works | Credentials: invalid → portal stays | Credentials: retry → success | Grace window: AP+HTTP+DNS up | Shutdown: HTTP closed | Shutdown: DNS closed | AP retired (STA-only) | Disconnect case | Mongoose coexistence OK | Baseline + resources | Result | Logs attached | Follow-up ref |
|---------------------|--------------|-------------------|-------------------------------|-----------|--------------------------------------|------------------------------|------------------------------|------------------------|----------------------|------------------------|-----------------|--------------------------|------------------------|--------|---------------|---------------|
| Android 14 (example) | Pixel 7 | yes (`/generate_204` → portal HTML) | yes (`http://10.10.0.1`) | yes | yes | yes | yes | yes | yes | yes | yes | yes | heap/tasks stable | PASS | `run-2024-…-android.log` | — |
| iOS 17 (example) | iPhone 13 | yes (`/hotspot-detect.html`) | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | heap/tasks stable | PASS | `run-…-ios.log` | — |
| macOS 14 (example) | MacBook Air | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | heap/tasks stable | PASS | `run-…-macos.log` | — |
| Windows 11 (example) | ThinkPad X1 | yes (`/connecttest.txt`,`/ncsi.txt`) | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | heap/tasks stable | PASS | `run-…-win.log` | — |
| _add rows per client_ | | | | | | | | | | | | | | | | |

## Column definitions

- **Captive probe hit** — the OS connectivity probe was answered with the
  portal HTML (not a vendor "success" payload) so the client detected the
  captive network.
- **Portal opened (canned browser)** — the OS captive assistant presented
  `http://10.10.0.1` and it rendered.
- **Scan works** — `POST /api/v1/wifi/scans` accepted (`202`) and
  `GET /api/v1/wifi/networks` returns the home SSID.
- **Invalid → portal stays** — wrong-password submit leaves the portal and AP
  up; `last_failure=connect_failed`; no credentials saved.
- **Retry → success** — correct password after a failure connects and enters
  the grace window.
- **Grace window: AP+HTTP+DNS up** — during `CONFIG_…_SUCCESS_GRACE_MS` the AP
  is visible, HTTP 80 serves, DNS 53 answers `10.10.0.1`.
- **Shutdown: HTTP closed / DNS closed** — after grace the portal HTTP listener
  and the captive DNS listener are closed (refused/timeout).
- **AP retired (STA-only)** — `Bimbrownik` disappears and the device stays
  connected in client mode.
- **Disconnect case** — portal disconnect (or `DELETE /api/v1/wifi/connection`)
  keeps provisioning available and a resubmit reconnects.
- **Mongoose coexistence OK** — a second Mongoose-backed service keeps
  responding after provisioning shuts down.
- **Baseline + resources** — free heap returns to baseline across repeated
  cycles; no task/stack growth.
- **Result** — `PASS` / `FAIL` (FAIL requires the failure procedure in the
  checklist: save serial log + client captures, then file an
  `HIL-FOLLOWUP:` task and reference it here).
- **Logs attached** — path/name of the saved evidence for the row.
- **Follow-up ref** — tracker reference of any failing row.

## Notes

- A row is `PASS` only when every checklist step for that client matches the
  expected result; any `FAIL` must cite the follow-up task reference.
- OS versions listed here are the versions actually tested; the plan's probe
  matrix (Android `/generate_204`, Apple `/hotspot-detect.html`, Windows
  `/ncsi.txt` + `/connecttest.txt`) is validated by these rows.
- Completion of this matrix is deferred and non-blocking; see the checklist's
  execution policy.