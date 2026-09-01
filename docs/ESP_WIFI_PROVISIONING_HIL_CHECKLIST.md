# ESP32 Wi-Fi HTTP Provisioning — Manual Hardware-In-The-Loop Checklist

Manual validation checklist for a human operator with physical access to an
ESP32 board. It exercises the Wi-Fi HTTP provisioning example added in
`TASK-131` (`examples/esp/wifi_provisioning_demo`) against real captive-portal
clients (Android, iOS/macOS, Windows) and records the results by hand.

| Item | Value |
|------|-------|
| Depends on | `TASK-131` (ESP provisioning example, `examples/esp/wifi_provisioning_demo`) |
| Example reference | `examples/esp/wifi_provisioning_demo/README.md` |
| Design reference | `docs/WIFI_HTTP_PROVISIONING_PLAN.md` (sections 6.2, 11.4, 13) |
| Matrix template | `docs/ESP_WIFI_PROVISIONING_SUPPORTED_CLIENT_MATRIX.md` |
| Execution status | **DEFERRED** — not executed as part of the automated plan |
| Blocking | **NON-BLOCKING** — this checklist is not a completion gate for any automated task |
| Physical ESP32 required | **No** — this task is complete without running any hardware test |

> **Execution policy.** This checklist is **deferred and non-blocking**. It is a
> manual procedure for a human with board access; it is not run by the automated
> plan, and missing a physical ESP32 does not block this task or any dependent
> automation. A human operator runs it only when a board, matching firmware, and
> client devices are available, and then only to validate behavior that POSIX
> unit/integration tests cannot observe (OS captive assistants, DHCP behavior of
> real clients, on-device resource usage).

---

## 1. Test record

Complete this record for **every run**. Each run binds one board, one firmware
image, one set of client devices, and one operator.

| Field | Value |
|-------|-------|
| **Board** | e.g. ESP32-DevKitC V4, ESP32-WROOM-32 module, rev. 1 |
| **Firmware revision** | git SHA / build id of the flashed image (from `idf.py` build or `git rev-parse HEAD`) |
| **Firmware defconfig** | `defconfig/esp_provisioning.defconfig` (or a delta, list it) |
| **ESP-IDF version** | v5.5.x (exact commit) |
| **Client device: Android** | Device model + Android version (e.g. Pixel 7, Android 14) |
| **Client device: iOS** | Device model + iOS version (e.g. iPhone 13, iOS 17) |
| **Client device: macOS** | Mac model + macOS version (e.g. MacBook Air, macOS 14) |
| **Client device: Windows** | Laptop model + Windows version (e.g. ThinkPad X1, Windows 11 23H2) |
| **Home network under test** | SSID + band (2.4/5 GHz) + auth type (WPA2/WPA3/open) |
| **Results** | Summary of PASS/FAIL per section (copy the tally from section 12) |
| **Logs** | Path/location of saved serial logs and client captures per run |
| **Resource measurements** | Values from section 9 (free heap, task count, stack high-water marks) |
| **Operator / date** | Name + date + environment (bench, lab) |

### Per-step result convention

Every step below has an **Expected result**. Record each step as:

- **PASS** — observed behavior matches the expected result exactly;
- **FAIL** — observed behavior differs (then follow the failure procedure);
- **NA** — step not applicable to this run (state why in the notes).

### Failure procedure (applies to every step)

If the actual result differs from the **Expected result**:

1. Record `FAIL` for the step in the supported-client matrix template
   (`docs/ESP_WIFI_PROVISIONING_SUPPORTED_CLIENT_MATRIX.md`) and in the result
   tally (section 12).
2. Save the **full serial log** from power-on through the failing action
   (do not truncate), plus any client-side evidence: screenshots of the captive
   assistant, browser console, and network captures (`tcpdump`/Wireshark on the
   client or a mirrored port).
3. Record which board, firmware revision, and client device/OS versions were in
   use (fields in section 1) — state them in the failure report.
4. File a **follow-up task** in the tracker (title prefix `HIL-FOLLOWUP:`) that
   references this document by name and step ID, attaches the evidence above,
   and names the expected result that failed. Do not re-mark the checklist or
   matrix complete until the follow-up is resolved.
5. Note the follow-up task reference in the matrix row for the failing step.

---

## 2. Preconditions

- ESP32 board with the `wifi_provisioning_demo` firmware flashed
  (`examples/esp/wifi_provisioning_demo`, build/flash per its README, target
  `esp32`, ESP-IDF v5.5.x).
- Storage partition erased so the device boots with **no** `wifi_ap.json`
  (fresh out-of-box state) — see the demo README "Erase-and-reprovision".
- Serial monitor attached and logging from power-on (115200 baud default).
- A home access point with **known-good credentials** for success tests.
- A second SSID or a deliberately wrong password for invalid-credential tests.
- One client per supported OS, each able to join a Wi-Fi AP:
  Android, iOS, macOS, and Windows device (see the supported-client matrix
  template).

### Known runtime parameters

| Parameter | Value (default) |
|-----------|-----------------|
| AP SSID | `WIFI_AP_NAME` — `Bimbrownik` (from `src/wifi/wifi_managment.h`) |
| AP / gateway IP | `10.10.0.1` |
| HTTP portal | `http://0.0.0.0:80` (`CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL`) |
| Captive DNS | `udp://0.0.0.0:53` (`CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL`) |
| Success grace period | `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS` = `30000` ms (30 s) |
| Wildcard DNS answer | any `A` query answered with `10.10.0.1`, TTL 60 s |
| REST API | `GET /api/v1/wifi/status`, `POST /api/v1/wifi/scans`, `GET /api/v1/wifi/networks`, `POST /api/v1/wifi/credentials`, `DELETE /api/v1/wifi/connection` |

Console banner expected at boot (portal auto-started):

```
[demo] init 5/5: provisioning app (HTTP http://0.0.0.0:80, DNS udp://0.0.0.0:53)...
[demo] init 5/5: provisioning controller ready (state=provisioning)
[demo] status: controller=provisioning provisioning=running wifi_running=yes wifi_connected=no
```

---

## 3. Baseline and boot behavior

**Step 3.1 — Fresh boot opens the portal automatically**

- **Action:** Power on a board with erased storage (no `wifi_ap.json`).
- **Expected result:** The console shows init steps 0/5..5/5 in order
  (OSAL → storage → Mongoose → Wi-Fi → provisioning), then a status line with
  `controller=provisioning provisioning=running wifi_running=yes
  wifi_connected=no`. The AP `Bimbrownik` is visible to a client scanner.
- **On failure:** Follow the failure procedure (section 1).

**Step 3.2 — AP DHCP lease and advertised DNS**

- **Action:** Connect a laptop to the `Bimbrownik` AP and inspect the DHCP lease
  (e.g. `ipconfig`/`ip addr`, or the client's network settings).
- **Expected result:** The client receives an IP in the `10.10.0.x` subnet with
  gateway `10.10.0.1` and **DNS server `10.10.0.1`** (the ESP DHCP server
  advertises the device IP as DNS).
- **On failure:** Follow the failure procedure (section 1).

**Step 3.3 — Wildcard captive DNS**

- **Action:** From a client on the AP run
  `nslookup foo.invalid 10.10.0.1` / `nslookup captive.apple.com 10.10.0.1`, or
  `dig @10.10.0.1 foo.local A`.
- **Expected result:** Every `A` query (arbitrary hostname) is answered with
  `10.10.0.1`.
- **On failure:** Follow the failure procedure (section 1).

**Step 3.4 — Portal reachable at the AP address**

- **Action:** From a client on the AP open `http://10.10.0.1/`.
- **Expected result:** The portal HTML page loads (HTTP 200), the page shows a
  scan/status workflow, and `GET /api/v1/wifi/status` returns JSON with
  `provisioning` and `state` fields.
- **On failure:** Follow the failure procedure (section 1).

---

## 4. Android captive detection

**Step 4.1 — Android connectivity check triggers the assistant**

- **Action:** On Android, join the `Bimbrownik` AP (forget any saved network
  first). Keep the client's Wi-Fi details screen open and watch for a
  "Sign in to Wi-Fi network" / "Connected, no internet" notification.
- **Expected result:** Android's captive check requests Android probe paths
  (`/generate_204` against the connectivity check host, which the ESP DNS
  resolves to `10.10.0.1`); the probe receives the portal HTML instead of a
  204, so Android marks the network "no internet" and shows the
  **Sign in to network** notification/assistant, which opens the portal at
  `http://10.10.0.1`.
- **On failure:** Follow the failure procedure (section 1).

**Step 4.2 — Android canned-browser scan and status**

- **Action:** In the Android captive browser, trigger a scan (portal button)
  and open `http://10.10.0.1/api/v1/wifi/status`.
- **Expected result:** A network scan runs (`202` on
  `POST /api/v1/wifi/scans`, results via `GET /api/v1/wifi/networks`); the
  status endpoint returns a JSON snapshot and the page renders the scanned
  home network. An **unknown browser GET** path (e.g. `/anything`) is
  redirected with `302` to the portal root.
- **On failure:** Follow the failure procedure (section 1).

---

## 5. iOS captive detection

**Step 5.1 — iOS assistant opens the portal**

- **Action:** On an iPhone/iPad, join `Bimbrownik` (Settings → Wi-Fi). Watch for
  the "Sign in to network" assistant sheet to appear.
- **Expected result:** iOS resolves `captive.apple.com/hotspot-detect.html`
  through the ESP DNS to `10.10.0.1`; the probe gets the portal HTML (not
  `Success`), so iOS presents the **Sign in to network** assistant that loads
  `http://10.10.0.1` and can navigate the portal.
- **On failure:** Follow the failure procedure (section 1).

**Step 5.2 — iOS portal interaction and status**

- **Action:** In the iOS assistant, scan, view status, and confirm the home SSID
  is listed.
- **Expected result:** Scan results and `GET /api/v1/wifi/status` render in the
  assistant browser; the portal is interactive (forms work inside the captive
  session).
- **On failure:** Follow the failure procedure (section 1).

---

## 6. macOS captive detection

**Step 6.1 — macOS Wi-Fi menu shows "Sign in to network"**

- **Action:** On a Mac, join `Bimbrownik` (click the Wi-Fi menu icon).
- **Expected result:** macOS runs its captive check
  (`captive.apple.com/hotspot-detect.html` via the ESP DNS), receives the
  portal HTML, and the Wi-Fi menu shows `Bimbrownik` with a
  **"Sign in to network..."** action that opens the portal in the captive
  browser.
- **On failure:** Follow the failure procedure (section 1).

**Step 6.2 — macOS portal loads and reports status**

- **Action:** Open the assistant's portal page and call
  `http://10.10.0.1/api/v1/wifi/status`.
- **Expected result:** The portal page loads (HTTP 200) and the status JSON is
  returned; browser GET to a non-API path is 302-redirected to `/`.
- **On failure:** Follow the failure procedure (section 1).

---

## 7. Windows captive detection

**Step 7.1 — Windows NCSI triggers a sign-in action**

- **Action:** On a Windows laptop, join `Bimbrownik` and open the Network flyout.
- **Expected result:** Windows NCSI probes (`/connecttest.txt` against
  `www.msftconnecttest.com` and `/ncsi.txt` against `wlan.msftncsi.com`) are
  answered with the portal HTML; Windows marks the network as a captive portal
  and the flyout / notification offers a **"Sign in"** action that opens the
  portal.
- **On failure:** Follow the failure procedure (section 1).

**Step 7.2 — Windows captive browser shows portal and scan**

- **Action:** Open the portal from the Windows Sign in action and run a scan.
- **Expected result:** The portal page renders, scanning returns the home SSID,
  and `GET /api/v1/wifi/status` returns the JSON snapshot. A `GET` to an
  unknown path is 302-redirected to the portal root.
- **On failure:** Follow the failure procedure (section 1).

---

## 8. Credential flows (invalid, retry, success, disconnect)

Run this section on **one representative client** (repeat on each OS from the
matrix if desired).

**Step 8.1 — Invalid credentials keep the portal open**

- **Action:** In the portal, select the home SSID and submit a **wrong password**
  (`POST /api/v1/wifi/credentials`).
- **Expected result:** The submit is accepted (`202`); the station reports
  `CONNECT_FAILED`; `GET /api/v1/wifi/status` shows `last_failure` =
  `connect_failed`; the portal **stays open**, the AP stays available, no
  credentials are saved to `wifi_ap.json`, and the device does not reboot.
- **On failure:** Follow the failure procedure (section 1).

**Step 8.2 — Retry after failure succeeds**

- **Action:** Without reloading the flow, resubmit the same SSID with the
  **correct password**.
- **Expected result:** The submit is accepted (`202`); the station connects and
  obtains an IP; the portal shows the connected state; the console transitions
  to `controller=grace` (the success grace period has started).
- **On failure:** Follow the failure procedure (section 1).

**Step 8.3 — Successful credentials persist and connect**

**Step 8.3.1 —** After step 8.2, read `GET /api/v1/wifi/status`.

- **Expected result:** JSON reports `ssid` = home SSID, `ip`/`netmask`/`gateway`
  from the home network, `state` = connected, `last_failure` = `no_failure`.

**Step 8.3.2 —** Reboot the device **after** the grace period completes
(see section 9.1 for timing) and confirm it reconnects without the portal.

- **Expected result:** The device loads `wifi_ap.json` at boot and connects to
  the home network directly; the console shows
  `controller=online provisioning=stopped wifi_running=yes wifi_connected=yes`;
  the portal does **not** reopen.
- **On failure:** Follow the failure procedure (section 1).

**Step 8.4 — Disconnect case keeps provisioning available**

**Step 8.4.1 —** With the portal active (fresh or after a failed attempt),
disconnect using the portal's disconnect action or
`DELETE /api/v1/wifi/connection` (expect `202`).

- **Expected result:** The station disconnects cleanly; a disconnect is not a
  failure (`last_failure` stays `no_failure`); the portal and AP remain up;
  the console does not restart WiFi or reboot.

**Step 8.4.2 —** After the disconnect, resubmit the correct credentials.

- **Expected result:** The station reconnects and the success grace period
  starts (`controller=grace`), proving the portal survived the disconnect.
- **On failure:** Follow the failure procedure (section 1).

**Step 8.5 — Disconnect during the grace period cancels grace, keeps portal**

- **Action:** Submit correct credentials to enter the grace window, then
  immediately disconnect (portal button or `DELETE /api/v1/wifi/connection`)
  before the 30 s grace expires.
- **Expected result:** The grace timer is cancelled; the controller returns to
  `provisioning`; the portal and AP **stay up**; no STA-only transition happens
  while disconnected.
- **On failure:** Follow the failure procedure (section 1).

---

## 9. Grace period and listener shutdown

**Step 9.1 — Portal remains up for the grace period**

- **Action:** After a successful credential submit (step 8.2) and **before**
  the 30 s `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS` elapses, from a
  client still on the AP: re-open `http://10.10.0.1/`, run a DNS query
  (`nslookup foo.local 10.10.0.1`), and confirm the AP `Bimbrownik` is still
  visible.
- **Expected result:** HTTP portal still serves (200), DNS still answers with
  `10.10.0.1`, the AP is still advertised, and the console shows
  `controller=grace`.
- **On failure:** Follow the failure procedure (section 1).

**Step 9.2 — HTTP listener shuts down after the grace period**

- **Action:** Wait out the grace period (observe the console transition to
  `controller=online provisioning=stopped`), then from a client `GET
  http://10.10.0.1/` and `curl` any portal route.
- **Expected result:** The HTTP request **fails** (connection refused /
  timeout) — the HTTP listener on port 80 was closed. The portal is no longer
  reachable. Unrelated Mongoose listeners are unaffected (see section 10).
- **On failure:** Follow the failure procedure (section 1).

**Step 9.3 — DNS listener shuts down after the grace period**

- **Action:** After the same transition, run
  `nslookup foo.local 10.10.0.1` / `dig @10.10.0.1 foo.local A` from the
  client.
- **Expected result:** The DNS query receives **no answer** (timeout, or
  ICMP port unreachable) — the captive DNS listener on UDP 53 was closed.
- **On failure:** Follow the failure procedure (section 1).

**Step 9.4 — AP retired (STA-only transition) after the grace period**

- **Action:** After shutdown, scan for Wi-Fi networks from a client.
- **Expected result:** The `Bimbrownik` AP is **no longer advertised**; the
  device is client-only (`T_WIFI_TYPE_CLIENT`) while staying connected to the
  home network (`wifi_connected=yes` in the console heartbeat).
- **On failure:** Follow the failure procedure (section 1).

---

## 10. Another Mongoose service remains operational

The provisioning listeners ride the shared Mongoose process
(`mongoose_process.h`); stopping the portal must never deinitialize that
process or any service hosted on it.

**Step 10.1 — Second Mongoose service survives provisioning**

- **Action (demo or product build):**
  - *Demo with an added diagnostic listener:* before opening the portal, host a
    second Mongoose-backed service on the same `mg_mgr`, e.g. an HTTP listener
    on port 8080 serving `GET /diag` → `200 OK`, added via
    `MongooseProcess_Invoke()`. Verify it responds while the portal is up.
  - *Product build:* keep the product's other Mongoose service active (e.g. an
    MQTT/ThingsBoard session on the same process).
- **Expected result:** The second service responds with `200 OK` while
  provisioning is running.

**Step 10.2 — Coexistence after shutdown**

- **Action:** Complete a successful provisioning flow through the grace period
  (section 9) so both provisioning listeners close; then call the second
  service again (per step 10.1) and confirm the Mongoose process state.
- **Expected result:** After portal shutdown the second service **still
  responds** (e.g. `GET /diag` → `200 OK`, or MQTT remains connected and
  telemetry flows). `MongooseProcess_IsRunning()` remains true; the serial log
  shows no Mongoose re-initialization. Provisioning never calls
  `MongooseProcess_Deinit()`.
- **On failure:** Follow the failure procedure (section 1).

**Step 10.3 — Repeated start/stop cycles do not leak into the process**

- **Action:** With the second service active, cycle provisioning 10 times
  (erase the storage partition and reboot, or stop/start the provisioning
  application explicitly) and check the second service after each cycle.
- **Expected result:** The second service responds after every cycle; no AP
  listener is left bound after shutdown (`netstat`/`ss` on POSIX or
  `esp_netif`/listener state on ESP shows nothing listening on 80/53).
- **On failure:** Follow the failure procedure (section 1).

---

## 11. Resource measurements (repeat cycles, heap, tasks)

For a release-like `esp_provisioning.defconfig` build, measure and record:

- **Free heap** (`heap_caps_get_free_size(MALLOC_CAP_8BIT)` or the ESP-IDF
  monitor's heap indicator) at: boot-with-portal, during portal use, during the
  grace window, and after STA-only shutdown.
- **Task count and stack high-water marks** before and after **10** repeated
  erase-and-reprovision cycles (per `idf.py erase-flash` reflash or storage
  partition erase): number of FreeRTOS tasks and each task's minimum free stack
  must return to the baseline (no monotonic growth).
- **Flash image size delta** for the firmware with provisioning enabled vs. a
  base build (from the build output).

**Step 11.1 — Heap returns to baseline after shutdown**

- **Action:** Record free heap at boot, then again after a full success flow +
  grace + shutdown with no active clients.
- **Expected result:** Free heap after shutdown is within a small, documented
  tolerance of the boot-with-portal baseline (no growing leak across at least
  10 cycles; report the numbers).
- **On failure:** Follow the failure procedure (section 1).

**Step 11.2 — No task/stack growth across cycles**

- **Action:** Record `uxTaskGetNumberOfTasks()` and per-task high-water marks
  after cycle 1 and after cycle 10.
- **Expected result:** Task count is identical; each task's minimum free stack
  does not shrink monotonically across cycles.
- **On failure:** Follow the failure procedure (section 1).

Record all values in section 1's "Resource measurements" field and in the
matrix template.

---

## 12. Result tally and evidence

| Section | Steps | PASS | FAIL | NA | Follow-up task refs |
|---------|-------|------|------|----|---------------------|
| 3  Baseline/boot | 3.1–3.4 | | | | |
| 4  Android | 4.1–4.2 | | | | |
| 5  iOS | 5.1–5.2 | | | | |
| 6  macOS | 6.1–6.2 | | | | |
| 7  Windows | 7.1–7.2 | | | | |
| 8  Credential flows | 8.1–8.5 | | | | |
| 9  Grace/shutdown | 9.1–9.4 | | | | |
| 10 Mongoose coexistence | 10.1–10.3 | | | | |
| 11 Resources | 11.1–11.2 | | | | |

Each FAIL row must carry a follow-up task reference (see the failure procedure).

**Evidence to capture per run:**

- Full serial log (power-on → shutdown) saved per run;
- Per-client screenshots of the captive assistant and portal;
- One network capture showing the DNS answer and probe responses
  (`tcpdump -i <iface> port 53 or port 80` on the client);
- Resource measurements table from section 11;
- Completed supported-client matrix (template:
  `docs/ESP_WIFI_PROVISIONING_SUPPORTED_CLIENT_MATRIX.md`).