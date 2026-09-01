# Wi-Fi Provisioning Demo (ESP32)

Exercises the complete Wi-Fi HTTP provisioning flow on an ESP32:

- a captive portal served on **HTTP port 80**,
- a captive DNS responder on **UDP port 53**,
- **automatic fallback**: when no saved Wi-Fi credential exists the portal is
  started automatically at boot, and it reopens whenever the saved
  credentials are exhausted,
- after a successful connection submitted through the portal the device keeps
  AP + HTTP + DNS available for a grace period and then transitions to
  **STA-only** mode, retiring the temporary access point while keeping the
  freshly provisioned network connection.

No serial configuration is required: a phone or laptop connects to the
device's temporary AP, opens `http://10.10.0.1`, and submits the home network
credentials.

## Runtime layout

| Module                 | Role                                             |
|------------------------|--------------------------------------------------|
| Captive portal (HTTP)  | `http://0.0.0.0:80` — portal page + REST API     |
| Captive DNS (UDP)      | `udp://0.0.0.0:53` — answers `*.local` A queries |
| Storage (littlefs)     | `storage` partition — saved credentials           |

Both listeners ride the shared Mongoose process, so the provisioning
application can be stopped without tearing down the process that would host
MQTT/ThingsBoard in a real product.

## Ownership order

The runtime is initialized in the documented ownership order; each module only
depends on the layers below it. Shutdown (rare on an embedded device) happens
in the reverse order.

```
init:     OSAL -> storage -> Mongoose -> Wi-Fi management -> provisioning
shutdown: provisioning -> Wi-Fi management -> Mongoose -> storage -> OS
```

- **OSAL** — FreeRTOS wrapper providing tasks, semaphores, queues, timers.
- **Storage** — mounts the littlefs `storage` partition. Wi-Fi management
  reads saved credentials during init, so the file system is mounted before
  it starts.
- **Mongoose** — shared event loop that hosts the HTTP portal and the captive
  DNS responders.
- **Wi-Fi management** — AP+STA mode: the station for the real network plus
  the temporary access point.
- **Provisioning** — `wifi_provisioning_controller_init()` implements the
  automatic fallback policy (start on no/failed credentials) and the
  success grace period followed by the STA-only transition.

## Listeners

| Service     | Address             | Purpose                           |
|-------------|---------------------|-----------------------------------|
| HTTP portal | `http://0.0.0.0:80` | Portal landing page + REST API    |
| Captive DNS | `udp://0.0.0.0:53`  | Captive DNS responder             |

## Automatic fallback

The automatic fallback controller (`wifi_provisioning_controller`) is enabled
in `defconfig/esp_provisioning.defconfig` and behaves as follows:

| Condition                          | Behavior                                         |
|------------------------------------|--------------------------------------------------|
| No saved credential exists          | Portal opens immediately at boot                  |
| Saved credentials exhausted (connect failed) | Portal reopens once per fallback session  |
| Single transient disconnect         | Portal never reopens                              |
| Credential submitted while portal up| Station connects; portal stays for the grace period |
| Grace period expires                | Portal stops; STA-only transition (AP retired)     |

## Provisioning flow

1. Power the board. With no `wifi_ap.json` on the storage partition the demo
   starts the portal automatically: the device creates the `wifi_provisioning`
   access point (SSID advertised in the console banner) and the captive DNS
   responder answers all A queries with `10.10.0.1`.
2. Connect a phone/laptop to that AP. A browser opens the captive portal at
   `http://10.10.0.1` (any DNS/HTTP request is redirected to it).
3. Select the home network and enter its password; the portal submits the
   credentials via `POST /api/v1/wifi/credentials`.
4. The station connects and obtains an IP; the credentials are stored in
   `wifi_ap.json`; after the grace period the demo drops the AP and keeps the
   station connected (STA-only).

REST endpoints:

| Method | Path                       | Purpose                             |
|--------|----------------------------|-------------------------------------|
| GET    | `/api/v1/wifi/status`      | Provisioning + link state, IP info |
| POST   | `/api/v1/wifi/scans`       | Start asynchronous scan (202)       |
| GET    | `/api/v1/wifi/networks`    | Scan results                        |
| POST   | `/api/v1/wifi/credentials` | Submit Wi-Fi credentials (202)      |
| DELETE | `/api/v1/wifi/connection`  | Asynchronous disconnect (202)       |

## Credential persistence

Submitted credentials are written to `wifi_ap.json` on the littlefs
`storage` partition (via `wifi_config_save`). On every subsequent boot the
Wi-Fi management layer loads that file, so the device reconnects to the saved
network without user interaction; the portal only reopens if the saved
credentials fail.

## Build, flash, monitor

Requires ESP-IDF v5.5.x. From this directory:

```sh
# 1. set up the IDF environment (or use the espressif/idf container)
source $IDF_PATH/export.sh

# 2. build (default target esp32)
idf.py set-target esp32
idf.py build

# 3. flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

The generated provisioning configuration is written to
`build/config/hq_config.h`; verify `CONFIG_WIFI_HTTP_PROVISIONING` is defined
after configuration.

## Erase-and-reprovision

To wipe saved credentials and return the device to the out-of-box provisioning
state:

```bash
# Option 1: erase the whole flash, then reflash the firmware
idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash monitor

# Option 2: erase only the storage partition (keeps the firmware)
esptool.py -p /dev/ttyUSB0 erase_region 0x210000 0x40000
```

After the erase the device boots with no `wifi_ap.json`, the automatic
fallback opens the portal and the provisioning flow starts over.