# HQ Platform - Quick Reference

## 📋 Specification Documents

- **[OSAL_SPECIFICATION.md](OSAL_SPECIFICATION.md)** - OSAL API specification
- **[HQ_PLATFORM_BUILD_SYSTEM.md](HQ_PLATFORM_BUILD_SYSTEM.md)** - Build system and CMake rules

## 🎯 Quick Overview

The Operating System Abstraction Layer (OSAL) provides a unified API for:

1. **Task Management** - Cross-platform thread/task creation and control
2. **Synchronization** - Binary semaphores, counting semaphores, and mutexes
3. **Message Queues** - Thread-safe inter-task communication
4. **Software Timers** - Callback-based timing functionality
5. **Logging** - Platform-independent debug and error logging

## 🏗️ Project Structure

```
hq_platform/
├── CMakeLists.txt          # Root build file
├── Kconfig                 # Platform selection (HQ_PLATFORM_POSIX / HQ_PLATFORM_ESP)
├── defconfig/
│   ├── posix.defconfig
│   └── esp.defconfig
├── cmake/
│   ├── posix.cmake         # POSIX platform toolchain/flags
│   └── esp.cmake           # ESP platform toolchain/flags
├── src/
│   └── osal/
│       ├── CMakeLists.txt
│       ├── include/            # Public API headers
│       │   ├── osal_common_type.h
│       │   ├── osal_error.h
│       │   ├── osal_task.h
│       │   ├── osal_bin_sem.h
│       │   ├── osal_count_sem.h
│       │   ├── osal_mutex.h
│       │   ├── osal_queue.h
│       │   ├── osal_timer.h
│       │   ├── osal_log.h
│       │   ├── osal_log_impl.h
│       │   ├── osal_macro.h
│       │   └── osal_assert.h
│       ├── common/             # Platform-independent sources
│       │   ├── osal_log.c
│       │   └── osal_error.c
│       ├── posix/              # POSIX implementation
│       │   ├── osal_impl_task.h
│       │   ├── osal_impl_sem.h
│       │   ├── osal_impl_queue.h
│       │   ├── osal_impl_timer.h
│       │   ├── osal_task_impl.c
│       │   ├── osal_bin_sem_impl.c
│       │   ├── osal_count_sem_impl.c
│       │   ├── osal_mutex_impl.c
│       │   ├── osal_queue_impl.c
│       │   ├── osal_timer_impl.c
│       │   ├── osal_log_impl.c
│       │   └── osal_assert.c
│       └── esp/                # ESP32/FreeRTOS implementation
│           ├── osal_impl_task.h
│           ├── osal_impl_sem.h
│           ├── osal_impl_queue.h
│           ├── osal_impl_timer.h
│           ├── osal_task_impl.c
│           ├── osal_bin_sem_impl.c
│           ├── osal_count_sem_impl.c
│           ├── osal_mutex_impl.c
│           ├── osal_queue_impl.c
│           ├── osal_timer_impl.c
│           ├── osal_log_impl.c
│           └── osal_assert.c
├── tests/
│   └── CMakeLists.txt
└── examples/
    └── CMakeLists.txt
```

## 🔑 Key Features

### Portability
- Single API works across POSIX and ESP32/FreeRTOS platforms
- Platform-specific implementations hidden behind common interface

### Type Safety
- Uses C99 fixed-width integer types (`uint32_t`, `int8_t`, etc.)
- Enum-based error codes instead of `#define` macros

### ISR Support
- Dedicated `_from_isr()` functions for interrupt-safe operations
- Proper context switching support

### Comprehensive Error Handling
- Detailed error codes for precise error diagnosis
- Human-readable error name conversion

## 📚 Documentation Sections

The specification is organized into the following documents:

### Main Specification
- **[OSAL_SPECIFICATION.md](OSAL_SPECIFICATION.md)** - Core specification with project structure and header organization
- **[HQ_PLATFORM_BUILD_SYSTEM.md](HQ_PLATFORM_BUILD_SYSTEM.md)** - CMake build system, Kconfig, and platform selection

### API Documentation (Detailed)
1. **[Task Management API](OSAL_Task_Management.md)** - Thread/task creation and control
2. **[Semaphore API](OSAL_Semaphore_API.md)** - Synchronization primitives
3. **[Queue API](OSAL_Queue_API.md)** - Message passing
4. **[Timer API](OSAL_Timer_API.md)** - Software timers

Each API document includes:
- Comprehensive function documentation
- Usage examples and code samples
- Best practices and common pitfalls
- Platform-specific implementation notes

## 🚀 Getting Started

1. Read the [OSAL_SPECIFICATION.md](OSAL_SPECIFICATION.md) document
2. Read the [HQ_PLATFORM_BUILD_SYSTEM.md](HQ_PLATFORM_BUILD_SYSTEM.md) for build rules
3. Create the directory structure as specified in `src/osal/`
4. Configure platform via Kconfig (`CONFIG_HQ_PLATFORM_POSIX` or `CONFIG_HQ_PLATFORM_ESP`)
5. Implement platform-specific headers in `osal_impl_*.h` files
6. Implement platform-specific functions in posix/ or esp/ directory
7. Build: `cmake -B build -DHQ_DEFCONFIG=defconfig/posix.defconfig && cmake --build build`

## 📖 Additional Notes

### Migration from Legacy Code
- Replace all `OS_*` prefixes with `OSAL_*`
- Convert `#define` error codes to enum
- Update integer types to use `stdint.h` types

### Best Practices
- Always check return codes from OSAL functions
- Use appropriate `_from_isr()` variants in interrupt handlers
- Set `OSAL_MAX_DELAY` for infinite waits
- Keep timer callbacks short and non-blocking

---

**For complete details, examples, and API documentation, see [OSAL_SPECIFICATION.md](OSAL_SPECIFICATION.md)**
