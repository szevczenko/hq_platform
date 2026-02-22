# HQ Platform Build System Specification

## 1. Scope and naming

- Main project name: **hq_platform**.
- `osal` is **not** the main project. It is one component under `src/`.
- Build tool: CMake (minimum version **3.16**).
- C language standard: **C99** (`-std=c99`).
- Use file name `CMakeLists.txt` (not `CMakeFile.txt`).
- Static library naming convention: `hq_<component>` (e.g., `hq_osal`).

## 2. Target directory structure

```text
hq_platform/
├─ CMakeLists.txt
├─ Kconfig
├─ defconfig/
│  ├─ posix.defconfig
│  └─ esp.defconfig
├─ cmake/
│  ├─ posix.cmake
│  └─ esp.cmake
├─ src/
│  ├─ CMakeLists.txt
│  ├─ osal/
│  │  ├─ CMakeLists.txt
│  │  ├─ include/
│  │  ├─ common/
│  │  ├─ posix/
│  │  └─ esp/
│  └─ <other_components>/
│     ├─ CMakeLists.txt
│     └─ ...
├─ tests/
│  └─ CMakeLists.txt
└─ examples/
   └─ CMakeLists.txt
```

## 3. Platform selection

Platform selection comes from Kconfig with exactly one enabled symbol:

- `CONFIG_HQ_PLATFORM_POSIX`
- `CONFIG_HQ_PLATFORM_ESP`

Rules:

1. If both are enabled -> configure error.
2. If both are disabled -> configure error.
3. Exactly one platform must be active per build.

CMake dispatch:

- `CONFIG_HQ_PLATFORM_POSIX` -> `include(cmake/posix.cmake)`
- `CONFIG_HQ_PLATFORM_ESP` -> `include(cmake/esp.cmake)`

## 4. Root CMakeLists.txt contract

Top-level responsibilities:

1. Set project + language standard (C99).
2. Set `cmake_minimum_required(VERSION 3.16)`.
3. Load configuration from defconfig/Kconfig-generated config.
4. Select platform and include corresponding file from `cmake/`.
5. Add subdirectories:
   - always: `src/`
   - optional: `tests/`, `examples/` using options
6. Define top-level integration target if needed (for host app or aggregate link).

Recommended root options:

- `HQ_BUILD_TESTS` (default `OFF`)
- `HQ_BUILD_EXAMPLES` (default `OFF`)

## 5. Per-directory CMakeLists.txt contract

Every component directory under `src/` must define:

- source files
- public include directories
- private include directories
- public dependencies
- private dependencies

Keep this contract identical across all components to avoid build drift.

## 6. Component policy (src)

- Each component in `src/` builds as a static library.
- `osal` implementation split:
  - `src/osal/` for platform-independent code
  - `src/osal/posix/` for POSIX backend
  - `src/osal/esp/` for ESP backend
- Only the selected backend sources are compiled.

## 7. Header visibility and dependencies

- Public headers exposed from `include/`.
- Internal/platform headers stay private to the component.
- For normal CMake targets:
  - use `PUBLIC` for API-level include dirs/dependencies
  - use `PRIVATE` for implementation-only include dirs/dependencies

For ESP-IDF components:

- `REQUIRES`: dependencies visible through public headers
- `PRIV_REQUIRES`: dependencies used only in component sources

## 8. Kconfig and defconfig integration

Required files:

- Top-level `Kconfig` with platform symbols
- Example defaults in `defconfig/posix.defconfig` and `defconfig/esp.defconfig`

Document and enforce:

1. Config values are exposed to C/C++ via a generated header file `hq_config.h`.
   - CMake generates this header from Kconfig output during configure step.
   - All source files that need config symbols include `hq_config.h`.
2. Precedence order: CLI cache variables override defconfig defaults.
3. Mapping between Kconfig symbols and CMake decisions:
   - `CONFIG_HQ_PLATFORM_POSIX` → `include(cmake/posix.cmake)`
   - `CONFIG_HQ_PLATFORM_ESP` → `include(cmake/esp.cmake)`
   - `CONFIG_OSAL_LOG_LEVEL` → passed as `-DOSAL_LOG_LEVEL=...` compile definition to `hq_osal` target.

## 9. Third-party libraries

### 9.1 Plain CMake libraries

Use standard CMake flow:

```cmake
add_subdirectory(foo)
target_link_libraries(hq_osal PUBLIC foo)
```

If supported by library options, disable unneeded parts:

```cmake
set(FOO_BUILD_STATIC OFF)
set(FOO_BUILD_TESTS OFF)
```

### 9.2 ESP-IDF-aware libraries

Allow linking to IDF component targets when building under ESP:

```cmake
if(ESP_PLATFORM)
  target_link_libraries(foo PRIVATE idf::spi_flash)
endif()
```

### 9.3 ESP-IDF component registration

When building as an ESP-IDF component, use `idf_component_register()` instead of `add_library()`:

```cmake
idf_component_register(SRCS "foo.c" "bar.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mbedtls
                       PRIV_REQUIRES console spiffs)
```

Rules:
- `REQUIRES`: components whose headers appear in **public** headers of this component.
- `PRIV_REQUIRES`: components whose headers appear only in **source files**.
- `REQUIRES` / `PRIV_REQUIRES` values must NOT depend on `CONFIG_xxx` macros (requirements are expanded before configuration is loaded). Source file lists and include paths CAN depend on config.

## 10. Tests and examples

- `tests/` and `examples/` each have their own `CMakeLists.txt`.
- Build controlled by options:
  - `HQ_BUILD_TESTS`
  - `HQ_BUILD_EXAMPLES`
- Expected scope:
  - POSIX: host-native test execution
  - ESP: integrated through ESP-IDF build flow

## 11. Common failure cases to prevent

1. Using global include/compile flags instead of target-based properties.
2. Wrong `PUBLIC`/`PRIVATE` causing missing transitive includes.
3. Compiling both `src/osal/posix/*` and `src/osal/esp/*` in one build.
4. Relative include paths that break when called from subdirectories.
5. Silent fallback when platform is unset (must be fatal at configure time).

## 12. Minimal acceptance criteria

1. Fresh clone builds POSIX with one documented configure/build command.
2. Fresh ESP-IDF integration builds `hq_platform` with ESP backend selected.
3. Exactly one platform backend is compiled per build.
4. Every directory listed in Section 2 has a `CMakeLists.txt` with clear target/dependency rules.

## 13. Build commands

### POSIX build

```bash
# Configure (using POSIX defconfig)
cmake -B build -DCMAKE_TOOLCHAIN_FILE= -DHQ_DEFCONFIG=defconfig/posix.defconfig

# Build
cmake --build build
```

### ESP-IDF build

```bash
# From ESP-IDF project that includes hq_platform as a component
idf.py set-target esp32
idf.py build
```

## 14. Header include guard policy

All generated header files must use traditional include guards (not `#pragma once`), following the pattern:

```c
#ifndef OSAL_<MODULE>_H
#define OSAL_<MODULE>_H

/* ... */

#endif /* OSAL_<MODULE>_H */
```

Examples:
- `osal_task.h` → `OSAL_TASK_H`
- `osal_common_type.h` → `OSAL_COMMON_TYPE_H`
- `osal_impl_task.h` → `OSAL_IMPL_TASK_H`
