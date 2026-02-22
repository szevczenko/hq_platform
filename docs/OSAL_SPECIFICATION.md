# Operating System Abstraction Layer (OSAL) Implementation Guide

## Table of Contents
1. [Project Structure](#1-project-structure)
2. [Header Files Organization](#2-header-files-organization)
3. [Task Management API](OSAL_Task_Management.md) 📄
4. [Semaphore API](OSAL_Semaphore_API.md) 📄
5. [Queue API](OSAL_Queue_API.md) 📄
6. [Timer API](OSAL_Timer_API.md) 📄
7. [Assertions and Validation](OSAL_Assertions.md) 📄
8. [Macros](osal_macro.h) 📄
9. [Build System](HQ_PLATFORM_BUILD_SYSTEM.md) 📄
10. [Implementation Checklist](#10-implementation-checklist)
11. [References](#11-references)

---

## 1. Project Structure

### 1.1 Directory Layout

OSAL is a component within the `hq_platform` project. See [HQ Platform Build System](HQ_PLATFORM_BUILD_SYSTEM.md) for the full project structure and CMake build rules.

OSAL files live under `src/osal/`:

```
src/osal/
├── CMakeLists.txt
├── include/              # Public API headers
│   ├── osal_common_type.h
│   ├── osal_error.h
│   ├── osal_task.h
│   ├── osal_bin_sem.h
│   ├── osal_count_sem.h
│   ├── osal_mutex.h
│   ├── osal_queue.h
│   ├── osal_timer.h
│   ├── osal_log.h
│   ├── osal_log_impl.h
│   ├── osal_macro.h
│   └── osal_assert.h
├── common/               # Platform-independent implementation
│   ├── osal_log.c
│   └── osal_error.c
├── posix/                # POSIX-specific implementation
│   ├── osal_impl_task.h
│   ├── osal_impl_sem.h
│   ├── osal_impl_queue.h
│   ├── osal_impl_timer.h
│   ├── osal_task_impl.c
│   ├── osal_bin_sem_impl.c
│   ├── osal_count_sem_impl.c
│   ├── osal_mutex_impl.c
│   ├── osal_queue_impl.c
│   ├── osal_timer_impl.c
│   ├── osal_log_impl.c
│   └── osal_assert.c
└── esp/                  # ESP32/FreeRTOS-specific implementation
    ├── osal_impl_task.h
    ├── osal_impl_sem.h
    ├── osal_impl_queue.h
    ├── osal_impl_timer.h
    ├── osal_task_impl.c
    ├── osal_bin_sem_impl.c
    ├── osal_count_sem_impl.c
    ├── osal_mutex_impl.c
    ├── osal_queue_impl.c
    ├── osal_timer_impl.c
    ├── osal_log_impl.c
    └── osal_assert.c
```

**Header visibility**:
- `include/` — public API headers (exposed to all components via `PUBLIC` include dirs)
- `posix/`, `esp/` — platform-specific headers and sources (private to `hq_osal` target)

---

## 2. Header Files Organization

Create the following header files to define the OSAL API and types:

### 2.1 Common Types and Error Codes (`osal_common_type.h`, `osal_error.h`)

**Purpose**: These files define common data types and error codes used throughout the OSAL. Users only need to include these two header files to access all OSAL types and error codes.

#### Standard Type Definitions

Include standard C headers for portable type definitions:

```c
#include <stdint.h>   // Fixed-width integer types (uint8_t, int32_t, etc.)
#include <stddef.h>   // Standard definitions (size_t, NULL, etc.)
#include <stdbool.h>  // Boolean type (bool, true, false)
```

**Note**: All custom integer types should be replaced with standard fixed-width types from `stdint.h` (e.g., rename `uint8` to `uint8_t`, `int8` to `int8_t`).

#### Common Constants

```c
/** @brief Infinite timeout for blocking operations */
#define OSAL_MAX_DELAY  0xFFFFFFFF

/** @brief Maximum name length for all OSAL objects (tasks, queues, semaphores, timers) */
#define OSAL_MAX_NAME_LEN  32

/** @brief Semaphore initial value: empty/unavailable (0) */
#define OSAL_SEM_EMPTY  0

/** @brief Semaphore initial value: full/available (1) */
#define OSAL_SEM_FULL   1
```

#### Status Codes

**Migration Note**: Replace all `OS_` prefixes with `OSAL_` and convert `#define` macros to an `enum` for better type safety.

```c
typedef enum {
    OSAL_SUCCESS                     = 0,   /**< @brief Successful execution */
    OSAL_ERROR                       = -1,  /**< @brief Failed execution */
    OSAL_INVALID_POINTER             = -2,  /**< @brief Invalid pointer argument */
    OSAL_ERROR_ADDRESS_MISALIGNED    = -3,  /**< @brief Memory address is not properly aligned */
    OSAL_ERROR_TIMEOUT               = -4,  /**< @brief Operation timed out */
    OSAL_INVALID_INT_NUM             = -5,  /**< @brief Invalid interrupt number */
    OSAL_SEM_FAILURE                 = -6,  /**< @brief Semaphore operation failed */
    OSAL_SEM_TIMEOUT                 = -7,  /**< @brief Semaphore operation timed out */
    OSAL_QUEUE_EMPTY                 = -8,  /**< @brief Queue is empty */
    OSAL_QUEUE_FULL                  = -9,  /**< @brief Queue is full */
    OSAL_QUEUE_TIMEOUT               = -10, /**< @brief Queue operation timed out */
    OSAL_QUEUE_INVALID_SIZE          = -11, /**< @brief Invalid queue size */
    OSAL_QUEUE_ID_ERROR              = -12, /**< @brief Invalid queue ID */
    OSAL_ERR_NAME_TOO_LONG           = -13, /**< @brief Name exceeds maximum length */
    OSAL_ERR_NO_FREE_IDS             = -14, /**< @brief No free resource IDs available */
    OSAL_ERR_NAME_TAKEN              = -15, /**< @brief Resource name already in use */
    OSAL_ERR_INVALID_ID              = -16, /**< @brief Invalid resource ID */
    OSAL_ERR_NAME_NOT_FOUND          = -17, /**< @brief Resource name not found */
    OSAL_ERR_SEM_NOT_FULL            = -18, /**< @brief Semaphore is not full */
    OSAL_ERR_INVALID_PRIORITY        = -19, /**< @brief Invalid task/thread priority */
    OSAL_INVALID_SEM_VALUE           = -20, /**< @brief Invalid semaphore value */
    /* -21 to -26: reserved for future use */
    OSAL_ERR_FILE                    = -27, /**< @brief File operation error */
    OSAL_ERR_NOT_IMPLEMENTED         = -28, /**< @brief Feature not implemented */
    OSAL_TIMER_ERR_INVALID_ARGS      = -29, /**< @brief Invalid timer arguments */
    OSAL_TIMER_ERR_TIMER_ID          = -30, /**< @brief Invalid timer ID */
    OSAL_TIMER_ERR_UNAVAILABLE       = -31, /**< @brief Timer resource unavailable */
    OSAL_TIMER_ERR_INTERNAL          = -32, /**< @brief Internal timer error */
    OSAL_ERR_OBJECT_IN_USE           = -33, /**< @brief Resource is currently in use */
    OSAL_ERR_BAD_ADDRESS             = -34, /**< @brief Invalid memory address */
    OSAL_ERR_INCORRECT_OBJ_STATE     = -35, /**< @brief Object in incorrect state for operation */
    OSAL_ERR_INCORRECT_OBJ_TYPE      = -36, /**< @brief Incorrect object type */
    OSAL_ERR_STREAM_DISCONNECTED     = -37, /**< @brief Stream connection lost */
    OSAL_ERR_OPERATION_NOT_SUPPORTED = -38, /**< @brief Operation not supported on this object */
    /* -39: reserved for future use */
    OSAL_ERR_INVALID_SIZE            = -40, /**< @brief Invalid size parameter */
    OSAL_ERR_OUTPUT_TOO_LARGE        = -41, /**< @brief Output size exceeds limit */
    OSAL_ERR_INVALID_ARGUMENT        = -42, /**< @brief Invalid argument value */
    OSAL_ERR_TRY_AGAIN               = -43, /**< @brief Temporary failure, retry operation */
    OSAL_ERR_EMPTY_SET               = -44  /**< @brief Lookup returned no results */
} osal_status_t;
```

#### Status Conversion Function

```c
/**
 * @brief Convert an status to a human-readable string
 *
 * @param[in]  status  Status
 *
 * @return Status string or "unknown error" if 
 */
const char* osal_get_status_name(osal_status_t status);
```

---

### 2.2 Logging (`osal_log.h`)

**Purpose**: Provides platform-independent logging functionality with different severity levels.

#### Log Levels

```c
typedef enum {
    OSAL_LOG_DEBUG,    /**< @brief Detailed debugging information */
    OSAL_LOG_INFO,     /**< @brief General informational messages */
    OSAL_LOG_WARNING,  /**< @brief Warning messages */
    OSAL_LOG_ERROR     /**< @brief Error messages */
} osal_log_level_t;
```

#### Compile-Time Log Level Configuration

The minimum log level is configured at **compile time** via the `OSAL_LOG_LEVEL` macro. Only messages at or above this level are compiled into the binary. Messages below this level are completely removed by the preprocessor — zero runtime overhead.

```c
/**
 * @brief Minimum log level (compile-time configuration)
 *
 * Define in project defconfig or build flags to override.
 * Default: OSAL_LOG_INFO (debug messages excluded).
 *
 * Examples:
 *   - OSAL_LOG_DEBUG   — all messages
 *   - OSAL_LOG_INFO    — info, warning, error (default)
 *   - OSAL_LOG_WARNING — warning and error only
 *   - OSAL_LOG_ERROR   — errors only
 */
#ifndef OSAL_LOG_LEVEL
#define OSAL_LOG_LEVEL  OSAL_LOG_INFO
#endif
```

**Configuration**: Define `OSAL_LOG_LEVEL` in your project's `defconfig`, `CMakeLists.txt`, or compiler flags:
```
# In defconfig or sdkconfig (ESP32)
CONFIG_OSAL_LOG_LEVEL=OSAL_LOG_DEBUG

# Or in CMakeLists.txt
add_definitions(-DOSAL_LOG_LEVEL=OSAL_LOG_WARNING)

# Or in compiler flags
CFLAGS += -DOSAL_LOG_LEVEL=OSAL_LOG_ERROR
```

#### Logging Functions

```c
/**
 * @brief Log a debug message
 * @param[in] format  Printf-style format string
 * @param[in] ...     Variable arguments matching format string
 * @note Compiled out if OSAL_LOG_LEVEL > OSAL_LOG_DEBUG
 */
void osal_log_debug(const char *format, ...);

/**
 * @brief Log an informational message  
 * @param[in] format  Printf-style format string
 * @param[in] ...     Variable arguments matching format string
 * @note Compiled out if OSAL_LOG_LEVEL > OSAL_LOG_INFO
 */
void osal_log_info(const char *format, ...);

/**
 * @brief Log a warning message
 * @param[in] format  Printf-style format string
 * @param[in] ...     Variable arguments matching format string
 * @note Compiled out if OSAL_LOG_LEVEL > OSAL_LOG_WARNING
 */
void osal_log_warning(const char *format, ...);

/**
 * @brief Log an error message
 * @param[in] format  Printf-style format string
 * @param[in] ...     Variable arguments matching format string
 * @note Always compiled in (highest severity)
 */
void osal_log_error(const char *format, ...);
```

#### Implementation Pattern (in `osal_log.h`)

The log functions are conditionally compiled based on `OSAL_LOG_LEVEL`:

```c
#if OSAL_LOG_LEVEL <= OSAL_LOG_DEBUG
void osal_log_debug(const char *format, ...);
#else
#define osal_log_debug(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_INFO
void osal_log_info(const char *format, ...);
#else
#define osal_log_info(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_WARNING
void osal_log_warning(const char *format, ...);
#else
#define osal_log_warning(...)  ((void)0)
#endif

/* osal_log_error is always available */
void osal_log_error(const char *format, ...);
```

#### Platform Implementation (`osal_log.c`, `osal_log_impl.h`, `osal_log_impl.c`)

**Location**: 
- `src/osal/common/osal_log.c` — common logging logic
- `src/osal/include/osal_log_impl.h` — declares `osal_impl_printf()`
- `src/osal/posix/osal_log_impl.c` or `src/osal/esp/osal_log_impl.c` — platform backend

```c
/* osal_log_impl.h */

/**
 * @brief Platform-specific print function
 *
 * Must be implemented by each platform in osal_log_impl.c.
 * Called internally by osal_printf().
 *
 * @param[in] format  Printf-style format string
 * @param[in] args    va_list of format arguments
 * @return Number of characters written, or negative on error
 */
int osal_impl_printf(const char *format, va_list args);
```

**Implementation Notes**: 
- In `osal_log.c` should be defined functions from header file (only those compiled in by `OSAL_LOG_LEVEL`)
- Implement `osal_printf()` as the static underlying output function which calls `osal_impl_printf()`
- In `osal_log_impl.c` should be defined `osal_impl_printf()`
- For both POSIX and ESP32 platforms, use standard `vprintf()` as the backend for `osal_impl_printf()`
- All logging functions depend on `osal_printf()` and format messages according to log level
- Output messages format: `"[LEVEL]: message"`, e.g. `"[INFO]: HELLO WORLD"`, `"[ERROR]: Connection lost"`

---

### 2.3 Task Management (`osal_task.h`)

**Purpose**: Provides a unified API for creating and managing tasks/threads across different platforms.

**Reference Documentation**: [ESP-IDF FreeRTOS Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html)

#### Platform-Specific Types (`osal_impl_task.h`)

Each platform must define platform-specific task types in `osal_impl_task.h`:

**FreeRTOS (ESP32)**:
```c
typedef TaskHandle_t osal_task_id_t;
```

**POSIX (Linux/macOS)**:
```c
typedef pthread_t osal_task_id_t;
```

#### Common Types (in `osal_task.h`)

```c
typedef void *osal_stackptr_t;   /**< Stack pointer type (NULL = dynamic allocation) */
typedef uint32_t osal_priority_t; /**< Task priority type */
```

#### Task Attributes

**Purpose**: Optional structure for platform-specific task options beyond core creation parameters. Core parameters (name, stack, priority) are passed directly to `osal_task_create()`.

```c
#define OSAL_TASK_NO_AFFINITY  (-1)

typedef struct {
    int32_t core_affinity;    /**< Core to pin task to, or OSAL_TASK_NO_AFFINITY */
    uint32_t reserved[4];     /**< Reserved for future use. Must be zero. */
} osal_task_attr_t;
```

#### FreeRTOS Background

- **Dynamic**: `xTaskCreatePinnedToCore()` — system allocates stack
- **Static**: `xTaskCreateStaticPinnedToCore()` — user provides stack via `stack_pointer` param
- Standard `xTaskCreate()` internally calls `...PinnedToCore()` with `tskNO_AFFINITY`

---

### 2.4 Semaphores (`osal_bin_sem.h`, `osal_count_sem.h`, `osal_mutex.h`)

**Purpose**: Synchronization primitives — binary semaphores, counting semaphores, and mutexes.

See [Semaphore API](OSAL_Semaphore_API.md) for full documentation.

#### Platform-Specific Types (`osal_impl_sem.h`)

Each platform must define platform-specific semaphore and mutex types in `osal_impl_sem.h`:

**FreeRTOS (ESP32)**:
```c
typedef SemaphoreHandle_t osal_bin_sem_id_t;
typedef SemaphoreHandle_t osal_count_sem_id_t;
typedef SemaphoreHandle_t osal_mutex_id_t;
```

**POSIX (Linux/macOS)**:
```c
#include <semaphore.h>
#include <pthread.h>

typedef sem_t *osal_bin_sem_id_t;
typedef sem_t *osal_count_sem_id_t;
typedef pthread_mutex_t *osal_mutex_id_t;
```

---

### 2.5 Queue (`osal_queue.h`)

**Purpose**: Thread-safe message passing between tasks and ISRs.

See [Queue API](OSAL_Queue_API.md) for full documentation.

#### Platform-Specific Types (`osal_impl_queue.h`)

Each platform must define platform-specific queue types in `osal_impl_queue.h`:

**FreeRTOS (ESP32)**:
```c
typedef QueueHandle_t osal_queue_id_t;
```

**POSIX (Linux/macOS)**:
```c
typedef struct osal_queue_internal *osal_queue_id_t;
```

---

### 2.6 Timer (`osal_timer.h`)

**Purpose**: Software timers that execute callback functions at specified intervals.

See [Timer API](OSAL_Timer_API.md) for full documentation.

#### Platform-Specific Types (`osal_impl_timer.h`)

Each platform must define platform-specific timer types in `osal_impl_timer.h`:

**FreeRTOS (ESP32)**:
```c
typedef TimerHandle_t osal_timer_id_t;
```

**POSIX (Linux/macOS)**:
```c
typedef struct osal_timer_internal *osal_timer_id_t;
```

---

## 10. Implementation Checklist

### 10.1 Core OSAL Components

#### API Documentation
- [ ] OSAL_Task_Management.md
- [ ] OSAL_Semaphore_API.md
- [ ] OSAL_Queue_API.md
- [ ] OSAL_Timer_API.md
- [ ] OSAL_Assertions.md

#### Header Files (in `src/osal/include/`)
- [ ] osal_common_type.h - Common types and definitions
- [ ] osal_error.h - Error codes and status definitions
- [ ] osal_task.h - Task management API
- [ ] osal_bin_sem.h - Binary semaphore API
- [ ] osal_count_sem.h - Counting semaphore API
- [ ] osal_mutex.h - Mutex API
- [ ] osal_queue.h - Message queue API
- [ ] osal_timer.h - Software timer API
- [ ] osal_log.h - Logging API
- [ ] osal_log_impl.h - Platform print function declaration (`osal_impl_printf`)
- [ ] osal_macro.h - Validation macros (ARGCHECK, LENGTHCHECK)
- [ ] osal_assert.h - Assertion API (osal_assert, OSAL_CHECK_POINTER, OSAL_CHECK_STRING)

#### Platform-Specific Type Headers (in each platform directory)
- [ ] osal_impl_task.h - Platform-specific task types (`osal_task_id_t`)
- [ ] osal_impl_sem.h - Platform-specific semaphore types (`osal_bin_sem_id_t`, `osal_count_sem_id_t`, `osal_mutex_id_t`)
- [ ] osal_impl_queue.h - Platform-specific queue types (`osal_queue_id_t`)
- [ ] osal_impl_timer.h - Platform-specific timer types (`osal_timer_id_t`)

#### Platform-Specific Implementation Files

**Common Files** (in `src/osal/common/`):
- [ ] osal_log.c - Logging implementation
- [ ] osal_error.c - Error code to string conversion

**POSIX Platform** (in `src/osal/posix/`):
- [ ] osal_impl_task.h
- [ ] osal_impl_sem.h
- [ ] osal_impl_queue.h
- [ ] osal_impl_timer.h
- [ ] osal_task_impl.c
- [ ] osal_bin_sem_impl.c
- [ ] osal_count_sem_impl.c
- [ ] osal_mutex_impl.c
- [ ] osal_queue_impl.c
- [ ] osal_timer_impl.c
- [ ] osal_log_impl.c
- [ ] osal_assert.c - POSIX assertion with fprintf/stderr

**ESP Platform** (in `src/osal/esp/`):
- [ ] osal_impl_task.h
- [ ] osal_impl_sem.h
- [ ] osal_impl_queue.h
- [ ] osal_impl_timer.h
- [ ] osal_task_impl.c
- [ ] osal_bin_sem_impl.c
- [ ] osal_count_sem_impl.c
- [ ] osal_mutex_impl.c
- [ ] osal_queue_impl.c
- [ ] osal_timer_impl.c
- [ ] osal_log_impl.c
- [ ] osal_assert.c - ESP32 assertion with esp_rom_printf

### 10.2 Key Design Decisions

1. **Argument Validation**: All public API functions validate their arguments using assertion macros (ARGCHECK, LENGTHCHECK, OSAL_CHECK_POINTER, OSAL_CHECK_STRING). Invalid arguments are caught early with detailed error reporting including file, function, and line number.

2. **Platform Abstraction**: ISR context switching (higher_priority_task_woken) is handled internally by platform implementations. API functions automatically determine execution context and use appropriate primitives.

3. **Error Handling**: Consistent error codes across all platforms with human-readable string conversion via osal_get_status_name().

4. **Memory Management**: Support for both dynamic and static memory allocation strategies for tasks and timers.

5. **Multi-Core Support**: Explicit core affinity control on multi-core platforms (ESP32), with no-affinity option for automatic scheduling.

### 10.3 Testing Recommendations

- [ ] Test dynamic and static memory allocation paths
- [ ] Verify ISR-safe functions work correctly from interrupt context
- [ ] Test timeout behavior (zero timeout, finite timeout, infinite timeout)
- [ ] Validate priority inheritance for mutexes (if supported)
- [ ] Test core affinity settings on multi-core platforms (ESP32)
- [ ] Verify proper context switching from ISR operations
- [ ] Test queue overflow/underflow handling
- [ ] Validate timer accuracy and callback execution
- [ ] Test error code conversion function with all error codes
- [ ] Verify argument validation catches null pointers and invalid sizes
- [ ] Test assertion reporting with correct file/function/line information

---

## 11. References

- [ESP-IDF FreeRTOS Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html)
- [FreeRTOS Task Notifications](http://www.freertos.org/RTOS-task-notifications.html)
- [FreeRTOS API Reference](https://www.freertos.org/a00106.html)
- [POSIX Threads Programming](https://computing.llnl.gov/tutorials/pthreads/)

---

## Document Version

- **Version**: 1.0
- **Date**: February 10, 2026
- **Status**: Initial Draft - Complete OSAL Specification
