# Kconfig Developer Guide

This document explains how the hq_platform Kconfig system works and how to
extend it.

## Overview

hq_platform uses [Kconfig](https://www.kernel.org/doc/html/latest/kbuild/kconfig-language.html)
(via Python [kconfiglib](https://github.com/ulfalber/Kconfiglib)) for
compile-time configuration. The system is architecture-independent and works
across all supported targets (POSIX, ESP32, Zephyr, STM32).

```
Kconfig (root)
├── source "src/osal/Kconfig"
├── source "src/cmd/Kconfig"
└── source "src/mongoose/Kconfig"
```

## How it works

```
┌─────────────┐      ┌──────────────────┐      ┌─────────────┐
│  Root       │      │  defconfig/      │      │             │
│  Kconfig    │─────▶│  posix.defconfig │─────▶│ hq_config.h │
│  + sources  │      │  esp.defconfig   │      │             │
└─────────────┘      └──────────────────┘      └─────────────┘
                            │
                     cmake/configure.cmake
                     runs `genconfig`
```

1. Root `Kconfig` sources per-component Kconfig files
2. A defconfig file provides per-target values
3. `configure.cmake` runs `genconfig` to produce `hq_config.h`
4. All source files include `hq_config.h` to access `CONFIG_*` defines

## File locations

| File | Purpose |
|------|---------|
| `Kconfig` | Root — platform selection + `source` statements |
| `src/<component>/Kconfig` | Component-specific options |
| `defconfig/posix.defconfig` | Default values for POSIX builds |
| `defconfig/esp.defconfig` | Default values for ESP32 builds |
| `defconfig/zephyr.defconfig` | Placeholder for Zephyr builds |
| `cmake/configure.cmake` | Runs genconfig during cmake configure |
| `build_*/hq_config.h` | Generated header (do not edit) |

## Adding a new option to an existing component

1. Edit `src/<component>/Kconfig`
2. Add the config symbol inside the existing `menu ... endmenu` block:

```kconfig
config OSAL_STACK_OVERFLOW_CHECK
  bool "Enable stack overflow checking"
  default y
  help
    Adds runtime stack overflow detection to OSAL tasks.
```

3. Add the default value to each defconfig that needs it:

```
# defconfig/posix.defconfig
CONFIG_OSAL_STACK_OVERFLOW_CHECK=y
```

4. Rebuild: `cmake -B build_posix ...` (genconfig runs automatically)

5. Use in code:
```c
#include "hq_config.h"

#if CONFIG_OSAL_STACK_OVERFLOW_CHECK
    // ...
#endif
```

## Creating Kconfig for a new component

1. Create `src/newcomponent/Kconfig`:

```kconfig
menu "New Component"

config NEWCOMP_BUFFER_SIZE
  int "Buffer size"
  default 1024
  range 64 65536
  help
    Size of the internal buffer in bytes.

endmenu
```

2. Add `source` in root `Kconfig`:

```kconfig
source "src/newcomponent/Kconfig"
```

3. Add defaults to defconfig files:

```
CONFIG_NEWCOMP_BUFFER_SIZE=1024
```

4. Rebuild to verify.

## How defconfig files work

A defconfig file contains one `KEY=value` per line. It sets non-default values
for Kconfig symbols. Symbols not listed use the defaults defined in the
Kconfig files themselves.

- `CONFIG_FOO=y` — boolean enabled
- `# CONFIG_FOO is not set` — boolean disabled (Kconfig comment syntax)
- `CONFIG_BAR=42` — integer value
- `CONFIG_BAZ="hello"` — string value (must be quoted)

### Per-target defaults

Each architecture has its own defconfig. The build system selects the
appropriate one via `HQ_DEFCONFIG`:

```bash
# POSIX
cmake -B build_posix -DHQ_DEFCONFIG=defconfig/posix.defconfig

# ESP (handled by esp.cmake)
cmake -B build_esp -DHQ_DEFCONFIG=defconfig/esp.defconfig
```

## Interactive configuration

### POSIX (menuconfig)

```bash
./scripts/menuconfig.sh
```

This opens a TUI where you can browse and modify all options. Changes are
saved to the active defconfig.

### ESP (idf.py menuconfig)

```bash
cd examples/esp/<example>
source <path-to-esp-idf>/export.sh
idf.py menuconfig
```

Note: ESP-IDF has its own Kconfig system. The hq_platform options appear
under the generated `hq_config.h` via `configure.cmake`, separate from
ESP-IDF's `sdkconfig.h`.

## Example-specific configuration

Examples do NOT add options to the global Kconfig. Instead, they use
`cmake -D` flags passed as compile definitions:

```bash
cmake -B build_posix \
  -DHQ_DEFCONFIG=defconfig/posix.defconfig \
  -DHQ_BUILD_EXAMPLES=ON \
  -DLAMP_MQTT_URL="mqtt://mybroker:1883" \
  -DLAMP_USERNAME="device_token"
```

The example's `CMakeLists.txt` passes these as `-DCONFIG_LAMP_*` defines.
Source files use `#ifndef CONFIG_LAMP_*` for fallback defaults.

## Naming conventions

| Pattern | Usage |
|---------|-------|
| `CONFIG_HQ_PLATFORM_*` | Target platform selection |
| `CONFIG_OSAL_*` | OSAL component options |
| `CONFIG_CMD_*` | Command-line component options |
| `CONFIG_MQTT_*` / `CONFIG_MONGOOSE_*` | Mongoose/MQTT options |
| `CONFIG_<COMP>_ENABLE_<FEAT>` | Boolean feature enables |

## Rules

- Every option must have a `help` text
- Integer options must specify `range min max`
- Use `depends on` for platform-specific options
- Use `source` for mandatory components, `osource` for optional ones
- Keep one `menu ... endmenu` block per component Kconfig file
- Do not put example-specific options in component Kconfig files
