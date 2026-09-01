# Wi-Fi Provisioning Demo (POSIX, simulated Wi-Fi)

Runs the Wi-Fi HTTP provisioning application end-to-end against the POSIX
Wi-Fi simulator (`src/wifi/platforms/posix/wifi_hal_driver.c`). No real radio,
router or internet is needed: the simulator exposes a fixed set of virtual
access points with distinct connection behaviours.

The demo brings the layered runtime up in strict ownership order and shuts it
down in the reverse order:

```
init:     OSAL -> Mongoose -> Wi-Fi management -> provisioning
shutdown: provisioning -> Wi-Fi management -> Mongoose -> OSAL
```

## Listeners

| Service      | Address                     | Purpose                          |
|--------------|-----------------------------|----------------------------------|
| HTTP portal  | `http://127.0.0.1:8080`      | Captive portal page + REST API   |
| Captive DNS  | `udp://127.0.0.1:10053`      | Captive DNS responder            |

Both listeners ride the shared Mongoose process (the same process that would
host MQTT/ThingsBoard), so stopping provisioning never tears down the shared
process.

## Build

```sh
cmake -B build \
  -DHQ_DEFCONFIG=defconfig/posix.defconfig \
  -DHQ_BUILD_TESTS=ON \
  -DHQ_BUILD_EXAMPLES=ON
cmake --build build
```

The demo binary is produced at:

```
build/examples/wifi_provisioning_demo
```

## Run

```sh
./build/examples/wifi_provisioning_demo
```

Then open the portal in a local browser:

```
http://127.0.0.1:8080/
```

## HTTP workflows (against simulated Wi-Fi)

The portal REST API is served by the provisioning application; all of it runs
against the simulated Wi-Fi environment.

| Method | Path                          | Description                              |
|--------|-------------------------------|------------------------------------------|
| GET    | `/api/v1/wifi/status`         | Provisioning + link state, IP details    |
| POST   | `/api/v1/wifi/scans`          | Start an asynchronous scan (202)         |
| GET    | `/api/v1/wifi/networks`       | Scan generation, state, AP records       |
| POST   | `/api/v1/wifi/credentials`    | Submit Wi-Fi credentials (202)           |
| DELETE | `/api/v1/wifi/connection`     | Request an asynchronous disconnect (202) |

Example using `curl`:

```sh
# status
curl -s http://127.0.0.1:8080/api/v1/wifi/status

# scan, then list networks
curl -s -X POST http://127.0.0.1:8080/api/v1/wifi/scans
curl -s http://127.0.0.1:8080/api/v1/wifi/networks

# connect to the stable simulated network
curl -s -X POST http://127.0.0.1:8080/api/v1/wifi/credentials \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"properly_ap","password":"12345678"}'

# status should now report state "connected"
curl -s http://127.0.0.1:8080/api/v1/wifi/status

# disconnect the station (portal stays up)
curl -s -X DELETE http://127.0.0.1:8080/api/v1/wifi/connection
```

### Captive DNS workflow

The captive DNS responder answers single-question A queries with the portal
address (`10.10.0.1`). From a terminal:

```sh
dig @127.0.0.1 -p 10053 provision.local

# or with dig omitted (Python)
python3 - <<'PY'
import socket, struct

q = struct.pack('>HHHHHH', 0x1234, 0x0100, 1, 0, 0, 0)
q += b'\x09provision\x05local\x00' + struct.pack('>HH', 1, 1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
s.sendto(q, ('127.0.0.1', 10053))
data, _ = s.recvfrom(512)
print('A record bytes (expect 10.10.0.1):', data[-4:])
PY
```

## Shutdown

Press `Ctrl-C` (SIGINT) or type `exit` and press Enter. The demo performs a
clean shutdown in reverse ownership order:

1. `wifi_http_provisioning_stop()` - closes the HTTP and DNS listeners,
2. `wifi_mgmt_stop()` - stops the Wi-Fi worker and simulated HAL,
3. `MongooseProcess_Deinit()` - stops the shared Mongoose poll thread,

and then proves both listener ports (`8080`, `10053`) were released by binding
them back on loopback.

## Simulated networks

The POSIX Wi-Fi simulator (see `src/wifi/platforms/posix/wifi_hal_driver.c`)
exposes these networks for scan and connection:

| SSID               | Password     | Behaviour                                            |
|--------------------|--------------|------------------------------------------------------|
| `properly_ap`      | `12345678`   | Stable connection, never drops                       |
| `disconnect_15_sec`| `12345678`   | Connects, then drops after 15 s                      |
| `broken`           | `12345678`   | Always rejects connection attempts                   |
| `disconnect_1_min` | `12345678`   | Connects, then drops after 60 s                     |
| `slow_connect`     | `12345678`   | Connects after a 3 s delay (GOT_IP)                |
| `weak_signal`      | `12345678`   | Stable but very low RSSI (-90 dBm)                  |
| `wrong_password`   | `correct_pw` | Rejects unless the password is exactly `correct_pw`  |

When credentials are submitted through the portal they are saved to
`wifi_ap.json` (plaintext storage in the working directory, same as the real
product on POSIX).