# Task Management API

[← Back to Main Specification](OSAL_SPECIFICATION.md)

---

## Overview

**Purpose**: Provides a unified API for creating and managing tasks/threads across different platforms.

**Reference Documentation**: [ESP-IDF FreeRTOS Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html)

---

## Platform-Specific Types (`osal_impl_task.h`)

Each platform must define platform-specific task types in `osal_impl_task.h`:

**FreeRTOS (ESP32)**:
```c
typedef TaskHandle_t osal_task_id_t;
```

**POSIX (Linux/macOS)**:
```c
typedef pthread_t osal_task_id_t;
```

---

## Common Types (`osal_task.h`)

### Stack Pointer Type

```c
/**
 * @brief Stack pointer type for task stack memory
 *
 * When set to NULL, the system dynamically allocates the stack.
 * When pointing to a user-provided buffer, static allocation is used.
 */
typedef void *osal_stackptr_t;
```

### Priority Type

```c
/**
 * @brief Task priority type
 *
 * Higher values = higher priority.
 * Valid range is platform-dependent:
 * - FreeRTOS: 0 (lowest) to configMAX_PRIORITIES-1 (highest)
 * - POSIX: Mapped to POSIX scheduling priorities
 */
typedef uint32_t osal_priority_t;
```

---

## Task Attributes

**Purpose**: Optional configuration structure for platform-specific task options that go beyond the core creation parameters.

### Attribute Structure Definition

```c
/**
 * @brief Core affinity constants
 */
#define OSAL_TASK_NO_AFFINITY  (-1)  /**< Task can run on any available core */

/**
 * @brief Task attributes structure
 *
 * Contains optional, platform-specific task configuration.
 * Pass NULL to osal_task_create() if defaults are acceptable.
 * Use osal_task_attributes_init() to initialize with defaults before modifying.
 */
typedef struct {
    int32_t core_affinity;    /**< Core to pin task to (0, 1, ...) or OSAL_TASK_NO_AFFINITY.
                                   Default: OSAL_TASK_NO_AFFINITY */
    uint32_t reserved[4];     /**< Reserved for future use. Must be zero. */
} osal_task_attr_t;
```

### Attribute Defaults

| Field | Default Value | Description |
|-------|---------------|-------------|
| `core_affinity` | `OSAL_TASK_NO_AFFINITY` | Task can run on any core |
| `reserved[0..3]` | `0` | Reserved for future extensions |

**FreeRTOS Background**:
- Standard `xTaskCreate()` internally calls `xTaskCreatePinnedToCore()` with `tskNO_AFFINITY`
- `xTaskCreatePinnedToCore()`: Dynamic allocation with core affinity
- `xTaskCreateStaticPinnedToCore()`: Static allocation with core affinity

---

## Task Management Functions

```c
/**
 * @brief Initialize task attributes with default values
 *
 * Sets all fields to their defaults. Must be called before modifying
 * individual attribute fields.
 *
 * @param[out] attr  Pointer to task attributes structure
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Attributes initialized
 * @retval OSAL_INVALID_POINTER   attr is NULL
 */
osal_status_t osal_task_attributes_init(osal_task_attr_t *attr);

/**
 * @brief Create and start a new task
 *
 * Creates a task with the given parameters. The task begins executing
 * immediately upon successful creation.
 *
 * @param[out] task_id         Returned task ID for future operations
 * @param[in]  task_name       Human-readable task name for debugging
 * @param[in]  routine         Function pointer to task entry point
 * @param[in]  arg             Argument passed to task function (may be NULL)
 * @param[in]  stack_pointer   Pointer to pre-allocated stack memory, or NULL for dynamic allocation
 * @param[in]  stack_size      Stack size in bytes
 * @param[in]  priority        Task execution priority (higher values = higher priority)
 * @param[in]  attr            Optional task attributes (core affinity, etc.), or NULL for defaults
 *
 * @return OSAL status code
 * @retval OSAL_SUCCESS            Task created successfully
 * @retval OSAL_ERROR              Task creation failed
 * @retval OSAL_INVALID_POINTER    task_id, task_name, or routine is NULL
 * @retval OSAL_ERR_NAME_TOO_LONG  task_name exceeds OSAL_MAX_NAME_LEN length
 * @retval OSAL_ERR_NO_FREE_IDS    Task table is full
 * @retval OSAL_ERR_NAME_TAKEN     Task name already in use
 * @retval OSAL_ERR_INVALID_PRIORITY Priority value out of valid range
 *
 * @note If stack_pointer is NULL, the system allocates stack memory dynamically.
 * @note If attr is NULL, platform defaults are used (no core affinity, etc.).
 */
osal_status_t osal_task_create(osal_task_id_t *task_id,
                               const char *task_name,
                               void (*routine)(void *),
                               void *arg,
                               osal_stackptr_t stack_pointer,
                               size_t stack_size,
                               osal_priority_t priority,
                               const osal_task_attr_t *attr);

/**
 * @brief Delete/terminate a task
 * @param[in] task_id  ID of task to delete
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Task deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid task ID
 */
osal_status_t osal_task_delete(osal_task_id_t task_id);

/**
 * @brief Delay/sleep the calling task for specified milliseconds
 * @param[in] milliseconds  Time to sleep in milliseconds
 * @return OSAL status code
 */
osal_status_t osal_task_delay_ms(uint32_t milliseconds);

/**
 * @brief Get current system time in milliseconds since boot
 * @return Time in milliseconds (32-bit unsigned integer)
 */
uint32_t osal_task_get_time_ms(void);
```

### Function Summary

| Function | Description |
|----------|-------------|
| `osal_task_attributes_init` | Initialize task attributes with default values |
| `osal_task_create` | Create and start a new task |
| `osal_task_delete` | Delete/terminate a task |
| `osal_task_delay_ms` | Delay the calling task for N milliseconds |
| `osal_task_get_time_ms` | Get system uptime in milliseconds |

---

## Usage Examples

### Creating a Simple Task (Dynamic Stack)

```c
#include "osal_task.h"

void my_task_function(void *arg) {
    while (1) {
        // Task logic here
        osal_task_delay_ms(1000);  // Sleep for 1 second
    }
}

int main(void) {
    osal_task_id_t task_id;
    
    // Create task with dynamic stack allocation, no special attributes
    osal_status_t status = osal_task_create(
        &task_id,              // [out] Task ID
        "MyTask",              // Task name
        my_task_function,      // Entry function
        NULL,                  // No argument
        NULL,                  // Dynamic stack allocation
        4096,                  // 4KB stack
        5,                     // Priority 5
        NULL                   // Default attributes
    );
    
    if (status == OSAL_SUCCESS) {
        // Task created successfully
    }
    
    return 0;
}
```

### Creating a Task with Core Affinity (ESP32)

```c
void high_priority_task(void *arg) {
    while (1) {
        // Critical processing on Core 0
        osal_task_delay_ms(100);
    }
}

void init_tasks(void) {
    osal_task_id_t task_id;
    osal_task_attr_t attr;
    
    // Initialize attributes with defaults, then set core affinity
    osal_task_attributes_init(&attr);
    attr.core_affinity = 0;  // Pin to Core 0
    
    osal_task_create(
        &task_id,
        "CriticalTask",
        high_priority_task,
        NULL,              // No argument
        NULL,              // Dynamic stack
        8192,              // 8KB stack for complex processing
        10,                // High priority
        &attr              // Core affinity attributes
    );
}
```

### Creating a Task with Static Stack

```c
// Pre-allocate stack memory
static uint8_t my_task_stack[4096];

void init_static_task(void) {
    osal_task_id_t task_id;
    
    osal_task_create(
        &task_id,
        "StaticTask",
        my_task_function,
        NULL,
        (osal_stackptr_t)my_task_stack,  // User-provided stack
        sizeof(my_task_stack),
        3,
        NULL                              // Default attributes
    );
}
```

### Passing Arguments to a Task

```c
typedef struct {
    uint32_t sensor_id;
    uint32_t sample_rate_ms;
} sensor_config_t;

void sensor_task(void *arg) {
    sensor_config_t *config = (sensor_config_t *)arg;
    
    while (1) {
        read_sensor(config->sensor_id);
        osal_task_delay_ms(config->sample_rate_ms);
    }
}

void init_sensor_tasks(void) {
    osal_task_id_t task_id;
    
    // Must be static or heap-allocated - must outlive the task
    static sensor_config_t temp_config = { .sensor_id = 1, .sample_rate_ms = 500 };
    static sensor_config_t press_config = { .sensor_id = 2, .sample_rate_ms = 1000 };
    
    osal_task_create(&task_id, "TempSensor", sensor_task, &temp_config,
                     NULL, 4096, 5, NULL);
    
    osal_task_create(&task_id, "PressSensor", sensor_task, &press_config,
                     NULL, 4096, 5, NULL);
}
```

---

## Best Practices

1. **Always check return values** from task creation functions
2. **Use appropriate stack sizes** - too small causes stack overflow, too large wastes memory
3. **Set meaningful task names** for easier debugging
4. **Consider priority carefully** - avoid priority inversion issues
5. **For ESP32 multi-core**: Use core affinity only when necessary for performance
6. **Clean up tasks** properly using `osal_task_delete()` when no longer needed
7. **Use static stacks** in memory-constrained environments to avoid heap fragmentation
8. **Task arguments must outlive the task** - use static/global or heap-allocated data

---

## Argument Validation

All public task functions validate their arguments:

```c
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_task_create(osal_task_id_t *task_id,
                               const char *task_name,
                               void (*routine)(void *),
                               void *arg,
                               osal_stackptr_t stack_pointer,
                               size_t stack_size,
                               osal_priority_t priority,
                               const osal_task_attr_t *attr) {
    OSAL_CHECK_POINTER(task_id);
    OSAL_CHECK_POINTER(routine);
    OSAL_CHECK_STRING(task_name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    ARGCHECK(stack_size > 0, OSAL_ERR_INVALID_SIZE);
    
    // attr may be NULL (defaults will be used)
    // arg may be NULL (task takes no argument)
    // stack_pointer may be NULL (dynamic allocation)
    
    // Implementation...
}
```

See [OSAL Assertions](OSAL_Assertions.md) for complete validation guidelines.

---

## Platform-Specific Notes

### POSIX Implementation
- Maps to `pthread_create()` and related functions
- Core affinity uses `pthread_setaffinity_np()` (non-portable)
- Stack size should be at least `PTHREAD_STACK_MIN`
- Static stack via `pthread_attr_setstack()`

### FreeRTOS/ESP32 Implementation
- Uses ESP-IDF enhanced FreeRTOS APIs
- Core affinity fully supported on dual-core ESP32
- Priority range: 0 (lowest) to configMAX_PRIORITIES-1 (highest)
- Default stack size recommendations: 2048-4096 bytes for simple tasks
- Dynamic: `xTaskCreatePinnedToCore()` / Static: `xTaskCreateStaticPinnedToCore()`

---

[← Back to Main Specification](OSAL_SPECIFICATION.md)
