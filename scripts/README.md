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

### menuconfig.sh

**Purpose:** Open the project Kconfig menu on Linux using Python `kconfiglib`

**Features:**
- Linux-only helper
- Verifies `python3` and `pip`
- Installs `kconfiglib` with `python3 -m pip install --user kconfiglib` when missing
- Launches the `menuconfig` frontend from the installed Python package
- Writes the configuration to `.config` by default

**Usage:**
```bash
./scripts/menuconfig.sh
```

**Requirements:**
- Linux
- Python 3
- `python3-pip`
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
