# HQ Platform

## Prerequisites

- CMake >= 3.16
- C99-compatible compiler (GCC, Clang)
- pthreads (POSIX builds)
- ESP-IDF (ESP32 builds)

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

```bash
idf.py build
```

The ESP platform is selected automatically when `ESP_PLATFORM` is defined by the IDF toolchain.

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
| `CONFIG_OSAL_LOG_LEVEL` | 0-4 | OSAL log verbosity |
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
