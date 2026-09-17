# HQ Platform

## Prerequisites

- CMake >= 3.16
- C99-compatible compiler (GCC, Clang)
- pthreads (POSIX builds)
- ESP-IDF (ESP32 builds)

Initialize submodules before the first build:

```bash
git submodule update --init --recursive
```

## Build (POSIX)

```bash
cmake -B build -DHQ_DEFCONFIG=defconfig/posix.defconfig
cmake --build build
```

## Build with tests

```bash
cmake -B build -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON
cmake --build build
```

Run tests:

```bash
./build/tests/osal_tests
```

Note: POSIX test builds currently produce a single aggregated test binary (`osal_tests`).

## Build with examples

```bash
cmake -B build -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_EXAMPLES=ON
cmake --build build
```

## Build (ESP32)

Requires ESP-IDF environment to be sourced (`. $IDF_PATH/export.sh`).

The repository itself is a collection of ESP-IDF components under `src/`.
ESP-IDF projects consume them as an external component collection (see
[Integrating into a downstream ESP-IDF application](#integrating-into-a-downstream-esp-idf-application)),
or you can build one of the in-tree examples, e.g.:

```bash
cd examples/esp/osal_demo
idf.py set-target esp32
idf.py build
```

The ESP platform is selected automatically when `ESP_PLATFORM` is defined by the IDF toolchain.

## Integrating into a downstream ESP-IDF application

Downstream ESP-IDF applications consume `hq_platform` as an external
collection of ESP-IDF components. The components live under `src/`: one
self-contained ESP-IDF component per subdirectory (`src/osal`, `src/cmd`,
...), each registered with `idf_component_register()` from its own
`CMakeLists.txt`. (`src/protocols/` is not an ESP-IDF component and is not
part of the build. `src/hal` is the one exception: because ESP-IDF derives
the component name from the directory basename and `hal` already names a
framework component, the HAL's ESP-IDF entry point is the `src/hal/hq_hal/`
subdirectory, whose basename supplies the collision-free name `hq_hal`.)
Downstream projects point ESP-IDF at those component
directories via `EXTRA_COMPONENT_DIRS`; they never copy `hq_platform` source
files into their own tree.

### Repository component layout

| Path | Component (IDF name) | Notes |
|------|----------------------|-------|
| `src/osal` | `osal` | OS Abstraction Layer (tasks, semaphores, queues, timers, filesystem) |
| `src/cmd` | `cmd` | Command-line interface |
| `src/mongoose` | `mongoose` | Mongoose/MQTT networking layer |
| `src/wifi` | `wifi` | Wi-Fi management |
| `src/wifi_provisioning` | `wifi_provisioning` | Wi-Fi HTTP provisioning (optional) |
| `src/thingsboard` | `thingsboard` | ThingsBoard client |
| `src/hal/hq_hal` | `hq_hal` | Hardware abstraction layer. Public API: `src/hal/include`. The ESP-IDF component entry point is `src/hal/hq_hal/` (its basename supplies the collision-free component name `hq_hal` — registering `src/hal` directly would override the framework `hal` component); on host/POSIX builds the shared `src/hal/CMakeLists.txt` builds the `hq_hal` target |
| `cmake/configure.cmake` | — | Generates `hq_config.h` / `hq_config.cmake` from Kconfig + defconfig |
| `cmake/esp.cmake`, `cmake/modules.cmake` | — | ESP platform flags and vendored-library paths |
| `defconfig/*.defconfig` | — | hq_platform configuration presets |
| `third_party/` | — | vendored submodules (littlefs, mongoose, embedded-cli, cJSON, mbedtls) |

`main/` and `examples/` are repository-internal demo projects and are not required by downstream consumers.

### Expected directory layout: cloning into a downstream project

`hq_platform` works from any location inside the application tree, but it must
**not** be placed under the application's `components/` directory. The
repository root is an application-level CMake project: its root `CMakeLists.txt`
calls `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` and `project()` when
`ESP_PLATFORM` is defined. It is not an ESP-IDF component and does not call
`idf_component_register()`. ESP-IDF auto-discovers every directory under
`components/` as a component, so a clone/submodule of the repository root there
would be executed as a component and trigger a nested-project configuration
failure before the `src/*` components are discovered.

The conventional location is therefore outside `components/`, for example
`<app>/external/hq_platform`:

```
my_app/
├─ CMakeLists.txt
├─ external/
│  └─ hq_platform/        # git clone of hq_platform
│     ├─ CMakeLists.txt
│     ├─ cmake/
│     ├─ defconfig/
│     ├─ src/
│     │  ├─ osal/
│     │  ├─ cmd/
│     │  ├─ mongoose/
│     │  ├─ wifi/
│     │  ├─ wifi_provisioning/
│     │  └─ thingsboard/
│     └─ third_party/        # nested submodules
└─ main/
   ├─ CMakeLists.txt
   └─ app_main.c
```

Whatever location is chosen, the same path must be used for `HQ_REPO_ROOT` and
in the `EXTRA_COMPONENT_DIRS` list (see below), and only the `src/*` component
directories are listed there — never the repository root.

### Expected directory layout: Git submodule

Add the submodule outside `components/` for the same reason as a clone — the
repository root is an application-level CMake project (it includes
`project.cmake`), not an ESP-IDF component, so it must not be auto-discovered
by ESP-IDF:

```
git submodule add <hq_platform-url> external/hq_platform
git submodule update --init --recursive
```

`--recursive` is required: `hq_platform` vendors `third_party/littlefs`,
`third_party/mongoose`, `third_party/embedded-cli`, `third_party/cJSON` and
`third_party/mbedtls` as its own submodules (plus `tests/unity` for test
builds), so they must be initialized before the components can build.

```
my_app/
├─ .gitmodules
├─ CMakeLists.txt
├─ external/
│  └─ hq_platform/          # submodule
│     └─ third_party/       # initialized via --recursive
└─ main/
   ├─ CMakeLists.txt
   └─ app_main.c
```

### Component discovery via `EXTRA_COMPONENT_DIRS`

Because the components live under `src/`, list every hq_platform component
the application needs — including transitive ones — explicitly. ESP-IDF only
scans the directories listed in `EXTRA_COMPONENT_DIRS`, so a component that is
pulled in transitively (for example `osal` via `mongoose` or `wifi`) still has
to be listed there, exactly as the in-tree examples do:

```cmake
set(EXTRA_COMPONENT_DIRS
  ${HQ_REPO_ROOT}/src/osal
  ${HQ_REPO_ROOT}/src/cmd              # only if needed
  ${HQ_REPO_ROOT}/src/mongoose
  ${HQ_REPO_ROOT}/src/wifi             # only if needed
  ${HQ_REPO_ROOT}/src/wifi_provisioning  # only if needed
  ${HQ_REPO_ROOT}/src/thingsboard      # only if needed
  ${HQ_REPO_ROOT}/src/hal/hq_hal       # only if needed
)
```

Set the list in your top-level `CMakeLists.txt` before including
`$ENV{IDF_PATH}/tools/cmake/project.cmake`. The in-tree examples
(`examples/esp/*`) use exactly this mechanism; `examples/esp/osal_demo`
lists only `src/osal` and `src/mongoose`.

### ESP-IDF component-manager integration

`hq_platform` ships no `idf_component.yml` / `dependencies.yml` manifests and
is not published to the ESP-IDF component registry, so `hq_platform` cannot
be pulled in through the component manager. Do not add it to
`dependencies.yml` or `idf_component.yml`. The supported discovery mechanism
is `EXTRA_COMPONENT_DIRS` described above.

### Public header inclusion rule

Applications include only public headers. Public header directories per
component:

| Component | Public include directory | Public headers |
|-----------|--------------------------|---------------|
| `osal`    | `src/osal/include`       | `osal_assert.h`, `osal_bin_sem.h`, `osal_common_type.h`, `osal_count_sem.h`, `osal_dir.h`, `osal_error.h`, `osal_file.h`, `osal_log.h`, `osal_macro.h`, `osal_mount.h`, `osal_mutex.h`, `osal_ota.h`, `osal_ota_state.h`, `osal_queue.h`, `osal_task.h`, `osal_time.h`, `osal_timer.h` |
| `cmd`     | `src/cmd/include`        | `hq_cmd.h` |
| `mongoose`| `src/mongoose`           | `mongoose.h`, `mongoose_process.h`, `mqtt_app.h`, `mqtt_config.h`, `captive_dns_server.h` |
| `wifi`    | `src/wifi`              | `wifi_config.h`, `wifi_managment.h`, `wifi_utils.h`, `wifi_hal_driver.h` |
| `wifi_provisioning` | `src/wifi_provisioning` | `wifi_http_provisioning.h`, `wifi_provisioning_controller.h` |
| `thingsboard` | `src/thingsboard/include` | all `tb_*.h` (e.g. `tb_client.h`, `tb_telemetry.h`, `tb_attributes.h`, `tb_rpc.h`, `tb_provision.h`, `tb_claim.h`, `tb_firmware_update.h`) |

Uses:

```c
#include "osal_task.h"   /* OK: osal public header */
#include "mongoose.h"    /* OK: mongoose public header */
#include "tb_client.h"    /* OK: thingsboard public header */
```

Inclusion rules:

- Include only public headers from the table above.
- Never include implementation files or private/platform-specific headers:
  - `src/osal/include/osal_log_impl.h` — an implementation helper that lives
    in the public include directory but is not part of the public OSAL API,
  - `src/osal/posix/**` and `src/osal/esp/**` (`osal_impl_*.h`, `*_impl.c`, ...),
  - `src/cmd/include/hq_cmd_internal.h`,
  - `src/thingsboard/tb_provision_internal.h`,
  - `src/wifi/wifidrv.h` — not part of the public Wi-Fi API: it
    unconditionally includes `app_config.h`, which is provided by the
    application, not by `hq_platform`,
  - any `src/**/platforms/**` implementation headers,
  - any `*.c` file under `src/`.
- `hq_config.h` is a generated public header (see below); include it as
  `"hq_config.h"` but never edit it and never include generated files from
  another application's build directory.
- Rely on IDF `REQUIRES` as usual: requiring an hq_platform component makes its
  public include directory visible to your component.

### OSAL filesystem mount/format semantics (no format on mount failure)

The OSAL LittleFS backends (`src/osal/esp/osal_mount_impl.c`,
`src/osal/posix/osal_mount_impl.c`) follow a strict mount/format contract
(TASK-118):

- **`osal_mount()` never formats.** If the filesystem is missing or corrupt,
  the mount fails with `OSAL_ERROR` and the partition/volume contents are left
  completely untouched (`format_if_mount_failed = false` in the ESP backend).
  An ordinary boot-time mount error must therefore never silently destroy
  credentials or manufacturing state.
- **Formatting happens only through explicit entry points**: `osal_mkfs()`,
  `osal_rmfs()` (and `osal_initfs()` volume setup, plus provisioning flows).
  On ESP the ESP-IDF 5.x register-format-unregister trick
  (`esp_vfs_littlefs_register()` with `format_if_mount_failed = true`,
  then `esp_vfs_littlefs_unregister()`) is used inside those explicit paths
  only.
- Recovery from a corrupt filesystem is a product decision (e.g. a
  manufacturing/provisioning flow), never an automatic reaction to a mount
  error.

See `src/osal/include/osal_mount.h` and
`tests/osal/osal_mount_test.c`
(`test_mount_failure_preserves_contents`,
`test_explicit_format_paths_still_work`) for the normative behavior and the
regression tests. On-target verification of the ESP path:

1. `idf.py -p <port> erase-flash`, then boot: the first `osal_mount()` after a
   successful `osal_mkfs()` must succeed (explicit format path works).
2. Corrupt the `storage` partition (e.g. `esptool.py erase_region` over it):
   the next boot must report the mount failure and leave the partition
   unformatted — a repeat mount keeps failing instead of auto-formatting.

### Required Kconfig options

`hq_platform` has its own Kconfig system, separate from ESP-IDF's sdkconfig.
Set the `HQ_DEFCONFIG` CMake variable in your top-level `CMakeLists.txt` to a
`hq_platform` defconfig preset (or your own defconfig) before including
`${HQ_REPO_ROOT}/cmake/configure.cmake`.

- Every build must enable exactly one platform symbol:
  - `CONFIG_HQ_PLATFORM_ESP=y` for ESP-IDF builds (`CONFIG_HQ_PLATFORM_POSIX` stays `n`),
  - `CONFIG_HQ_PLATFORM_POSIX=y` for POSIX builds.
- The preset `defconfig/esp.defconfig` already contains the correct values;
  point `HQ_DEFCONFIG` at it, or supply your own defconfig file that enables
  exactly one platform symbol.
- Component options are consumed by sources through the generated
  `hq_config.h`:
  - `osal`: `CONFIG_OSAL_LOG_LEVEL`.
  - `cmd`: `CONFIG_CMD_ESP_OUTPUT_CONSOLE` / `CONFIG_CMD_ESP_OUTPUT_UART` (choice), `CONFIG_CMD_ESP_UART_NUM`, `CONFIG_CMD_ESP_UART_BAUDRATE`, `CONFIG_CMD_INPUT_TASK_STACK_SIZE`, `CONFIG_CMD_RX_BUFFER_SIZE`, `CONFIG_CMD_BUFFER_SIZE`, ...
  - `mongoose`: `CONFIG_MONGOOSE_LOG_LEVEL`, `CONFIG_MQTT_DEFAULT_ADDRESS`, `CONFIG_MQTT_DEFAULT_SSL`, `CONFIG_MQTT_DEFAULT_TOPIC_PREFIX`, `CONFIG_MQTT_DEFAULT_POST_TOPIC`, `CONFIG_MQTT_DEFAULT_USERNAME`, `CONFIG_MQTT_DEFAULT_PASSWORD`, `CONFIG_MQTT_DEFAULT_CLIENT_ID`, ...
  - `wifi_provisioning`: `CONFIG_WIFI_HTTP_PROVISIONING` and its sub-options (`CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK`, `CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL`, `CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL`, `CONFIG_WIFI_HTTP_PROVISIONING_DNS_TTL`, `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`, `CONFIG_WIFI_HTTP_PROVISIONING_BODY_MAX`); the preset `defconfig/esp_provisioning.defconfig` enables them.
- These options are generated into `build/config/hq_config.h` by
  `cmake/configure.cmake` (which also emits `build/config/hq_config.cmake`).
  They are not ESP-IDF sdkconfig options.

### Minimum ESP-IDF version and supported targets

- The repository-defined minimum ESP-IDF version is **>= 5.0**: the OSAL
  LittleFS backend rejects older versions in
  `src/osal/esp/littlefs_impl/esp_littlefs.c`
  (`#error "esp_littlefs requires esp-idf >=5.0"`).
- The repository's automated CI (GitHub Actions,
  `.github/workflows/build.yml`) builds the ESP examples and
  `tests/platform/esp` with ESP-IDF **v5.5.4**
  (`espressif/esp-idf-ci-action`, `esp_idf_version: v5.5.4`); that is the
  version verified by the repository.
- The local `scripts/esp_build.sh` Docker helper is a convenience script that
  builds with the `espressif/idf:v6.0` image; it is not the CI-verified
  version and does not change the >= 5.0 source-enforced minimum.
- The supported/verified target is **`esp32`**: the committed example
  `sdkconfig` files (`examples/esp/cmd_demo`, `examples/esp/thingboard_fwu`,
  `examples/esp/wifi_provisioning_demo`) select `CONFIG_IDF_TARGET=esp32`,
  and `scripts/esp_build.sh` hardcodes `IDF_TARGET=esp32`. No other ESP32
  variant is verified by the repository.
- CMake >= 3.16 and a C99 compiler are required (POSIX and ESP builds).
- `hq_platform` requires pre-built physical boards for hardware-in-the-loop
  testing; CI runs POSIX unit tests and the ESP examples are built with
  the IDF toolchain described above.

### Minimal downstream application

A minimal app that uses `osal` and `mongoose`:

`my_app/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)

# hq_platform is cloned/submoduled into <app>/external/hq_platform.
get_filename_component(HQ_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/external/hq_platform" ABSOLUTE)

# hq_platform compile-time configuration (Kconfig preset).
set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")

# Discover hq_platform components under src/.
set(EXTRA_COMPONENT_DIRS
  ${HQ_REPO_ROOT}/src/osal
  ${HQ_REPO_ROOT}/src/mongoose
)

# hq_platform CMake helpers: Kconfig -> hq_config.h, ESP flags, lib paths.
include(${HQ_REPO_ROOT}/cmake/configure.cmake)
# Load the generated CMake configuration (CONFIG_* variables). Required for
# every consumer of the generated hq_platform configuration: the ESP component
# CMakeLists.txt files branch on these variables (e.g.
# CONFIG_WIFI_HTTP_PROVISIONING in src/mongoose and src/wifi_provisioning).
include(${HQ_CONFIG_CMAKE})
include(${HQ_REPO_ROOT}/cmake/esp.cmake)
include(${HQ_REPO_ROOT}/cmake/modules.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

`my_app/main/CMakeLists.txt`:

```cmake
idf_component_register(
  SRCS app_main.c
  INCLUDE_DIRS .
  REQUIRES osal
)
```

`my_app/main/app_main.c`:

```c
#include "osal_task.h"

static void my_task(void *arg)
{
    (void)arg;
    for (;;) {
        osal_task_delay_ms(1000);
    }
}

void app_main(void)
{
    osal_task_id_t id;
    osal_task_create(&id, "my_task", my_task, NULL, NULL,
                     4096, 5, NULL);
}
```

Build and flash:

```bash
source $IDF_PATH/export.sh
cd my_app
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

To use more components, extend both `EXTRA_COMPONENT_DIRS` and the
`REQUIRES` list of your `main` component.

> Note: the CMake example above is a downstream copy of the pattern used by the
> in-tree ESP examples (`examples/esp/osal_demo` is the smallest example).

## Configuration

Platform configuration is provided via defconfig files:

| File | Platform |
|------|----------|
| `defconfig/posix.defconfig` | Linux / macOS |
| `defconfig/esp.defconfig` | ESP32 (FreeRTOS) |

Available config options:

| Option | Values | Description |
|--------|--------|-------------|
| `CONFIG_HQ_PLATFORM_POSIX` | y/n | Enable POSIX backend |
| `CONFIG_HQ_PLATFORM_ESP` | y/n | Enable ESP backend |
| `CONFIG_CMD_ESP_OUTPUT_CONSOLE` | y/n | CMD output via ESP-IDF console UART |
| `CONFIG_CMD_ESP_OUTPUT_UART` | y/n | CMD output via direct UART driver |
| `CONFIG_CMD_ESP_UART_NUM` | 0-2 | UART port for direct UART mode |
| `CONFIG_CMD_ESP_UART_BAUDRATE` | int | UART baudrate for direct UART mode |
| `CONFIG_OSAL_LOG_LEVEL` | 0-3 | OSAL log verbosity |
| `CONFIG_MONGOOSE_LOG_LEVEL` | 0-4 | Mongoose log verbosity |

For ESP-IDF settings required by each CMD output mode, see [ESP_UART_Configuration.md](docs/ESP_UART_Configuration.md).

## Documentation

- [OSAL_SPECIFICATION.md](docs/OSAL_SPECIFICATION.md) - OSAL API specification
- [HQ_PLATFORM_BUILD_SYSTEM.md](docs/HQ_PLATFORM_BUILD_SYSTEM.md) - Build system details
- [ESP_UART_Configuration.md](docs/ESP_UART_Configuration.md) - ESP CMD output configuration
- [OSAL_Task_Management.md](docs/OSAL_Task_Management.md) - Task API
- [OSAL_Semaphore_API.md](docs/OSAL_Semaphore_API.md) - Semaphore API
- [OSAL_Queue_API.md](docs/OSAL_Queue_API.md) - Queue API
- [OSAL_Timer_API.md](docs/OSAL_Timer_API.md) - Timer API
