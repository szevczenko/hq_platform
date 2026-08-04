# Build Scripts

This directory contains automated build scripts for the HQ Platform project.

## Scripts

### posix_build.sh

**Purpose:** Build the project for POSIX platforms (Linux/macOS)

**Features:**
- Automatically creates `build_posix` directory
- Configures CMake with POSIX toolchain
- Applies default POSIX defconfig settings
- Enables testing by default (`HQ_BUILD_TESTS=ON`)
- Sets log level to ERROR (3)
- Compiles all source code and generates unified test application

**Usage:**
```bash
./scripts/posix_build.sh
```

**Output:**
- Built libraries: `build_posix/src/osal/libhq_osal.a`, `build_posix/src/mongoose/libhq_mongoose.a`
- Test executable: `build_posix/tests/osal_tests`

**Run tests:**
```bash
./build_posix/tests/osal_tests
```

---

### esp_build.sh

**Purpose:** Build the project for ESP32 using Docker and ESP-IDF v6.0

**Features:**
- Uses official `espressif/idf:v6.0` Docker image
- Hardcoded target: **ESP32**
- Hardcoded IDF version: **6.0**
- Automatic Docker daemon validation
- Mounts project directory read-write into container
- Preserves file ownership (uses host UID)
- Optional `clean` argument to remove build artifacts

**Requirements:**
- Docker installed and running
- At least ~2GB free disk space for the Docker image

**Usage:**
```bash
# Standard build
./scripts/esp_build.sh

# Build with clean (remove old artifacts first)
./scripts/esp_build.sh clean
```

**Build Directory:**
- `build_esp/` - ESP-IDF build directory

**Flash to Device (from Docker):**
Requires the serial port to be passed through:
```bash
docker run --rm \
    -v $PWD:/project \
    -w /project \
    -u $UID \
    -e HOME=/tmp \
    --device /dev/ttyUSB0 \
    espressif/idf:v6.0 \
    idf.py -B build_esp flash
```

Replace `/dev/ttyUSB0` with your actual serial port.

---

### thingboard_firmware_update_test.sh

Purpose: End-to-end firmware update test flow against local ThingsBoard 4.3.

Features:
- Optionally starts the local docker compose stack from docker/thingboard.
- Validates REST API availability with API key authentication.
- Creates or reuses a test device.
- Resolves device access token.
- Generates dummy firmware binary.
- Creates OTA package metadata and uploads package bytes.
- Assigns firmware package to the selected device.
- Builds and runs the firmware update demo application.
- Fetches firmware telemetry and writes run artifacts.

Usage from PowerShell:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; bash scripts/thingboard_firmware_update_test.sh"
```

Main environment overrides:
- TB_URL (default: http://127.0.0.1:8080)
- TB_API_KEY_FILE (default: docker/thingboard/api_key)
- TB_USERNAME and TB_PASSWORD (optional JWT fallback when API key lacks tenant-admin scope)
- TB_FW_DEVICE_NAME
- TB_FW_TITLE
- TB_FW_VERSION
- TB_FW_SIZE_BYTES
- TB_FW_RUN_SECONDS
- TB_FW_START_STACK (1 to start docker compose, 0 to skip)

Output:
- Root folder: scripts/output/firmware_update
- One timestamped subfolder per run with:
    - run.log
    - demo_runtime.log
    - summary.txt
    - json/*.json snapshots from REST operations

Auth note:
- This script needs tenant-level privileges to create OTA package, upload package data, and assign firmware to device.
- If API key in docker/thingboard/api_key is restricted, set TB_USERNAME and TB_PASSWORD for a tenant admin account.

---

### thingboard_firmware_update_quick.sh

Purpose: Fast repeat run for firmware update validation during development.

What it does:
- Wraps `thingboard_firmware_update_test.sh`.
- Uses faster defaults:
    - `TB_FW_START_STACK=0` (do not restart docker stack)
    - `TB_FW_RUN_SECONDS=20`
    - `TB_FW_SIZE_BYTES=4096`
    - auto-generated unique `TB_FW_VERSION`

Usage from PowerShell:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; bash scripts/thingboard_firmware_update_quick.sh"
```

Optional override example:

```bash
wsl.exe bash -lc "cd /mnt/c/projekty/hq_platform; TB_FW_RUN_SECONDS=30 TB_FW_START_STACK=1 bash scripts/thingboard_firmware_update_quick.sh"
```

---

### menuconfig.sh

**Purpose:** Open the project Kconfig menu on Linux using Python `kconfiglib`

**Features:**
- Linux-only helper
- Verifies `python3`
- Uses `pipx` when available (install or upgrade `kconfiglib` idempotently)
- Falls back to a local virtual environment (`.venv_menuconfig`) when `pipx` is unavailable
- Installs/updates `kconfiglib` in the selected environment
- Launches the `menuconfig` frontend from that environment
- Writes the configuration to `.config` by default

**Usage:**
```bash
./scripts/menuconfig.sh
```

**Requirements:**
- Linux
- Python 3
- Either `pipx` or Python `venv` support
- Terminal with `curses` support

---

## Configuration

### Log Level
Default log level is set to **ERROR (3)** in both scripts to reduce console output during builds.

To change log levels:
- Modify `CONFIG_OSAL_LOG_LEVEL` in `posix_build.sh`
- Modify `Kconfig` for ESP builds or pass via `idf.py` menuconfig

### Parallelization
Both scripts use all available CPU cores for faster compilation:
- POSIX: `cmake --build . --parallel $(nproc)`
- ESP: Docker uses single core by default (can be customized)

---

## Troubleshooting

### POSIX Build Issues
- **CMake not found:** Install CMake: `apt install cmake` (Ubuntu/Debian) or `brew install cmake` (macOS)
- **Compiler not found:** Install build tools: `apt install build-essential` (Ubuntu/Debian)
- **Permission denied:** Ensure script is executable: `chmod +x scripts/*.sh`
- **menuconfig.sh fails before launch:** Install pip: `sudo apt install python3-pip`
- **kconfiglib install path not found:** Add the user Python bin directory to `PATH` or rerun from the same shell after installation

### ESP Build Issues
- **Docker not found:** Install Docker from https://docs.docker.com/install/
- **Docker daemon not running:** Start Docker service
- **Permission denied on Docker:** Add user to docker group: `sudo usermod -aG docker $USER`
- **Image pull timeout:** Retry the script (images are cached after first pull)
- **Firmware update script fails at API call:** Verify ThingsBoard stack is running and API key in `docker/thingboard/api_key` is valid

---

## Environment

### POSIX
- CMake 3.10+
- GCC/Clang C compiler
- POSIX-compatible system (Linux, macOS)

### menuconfig
- Linux
- Python 3
- `python3-pip`
- `kconfiglib` from PyPI

### ESP32
- Docker Engine 19.03+
- espressif/idf:v6.0 image (auto-pulled on first run)
