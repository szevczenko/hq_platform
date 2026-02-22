# OSAL Assertions

[← Back to Main Specification](OSAL_SPECIFICATION.md)

---

## Overview

This header provides assertion and pointer checking facilities for robust OSAL implementation. Assertions help catch programming errors during development and provide detailed error information.

**Location**: 
- Header: `osal/osal_assert.h`
- Implementation: `osal/<platform>/osal_assert.c` (platform-specific)

---

## Assertion Macros

### OSAL_CHECK_POINTER

```c
/**
 * @brief Check if pointer is non-NULL
 *
 * Validates that a pointer argument is not NULL. If validation fails,
 * an assertion is triggered with file, function, and line information.
 *
 * @param[in] ptr  Pointer to check
 * @return OSAL_INVALID_POINTER if ptr is NULL
 */
#define OSAL_CHECK_POINTER(ptr) \
    if ((ptr) == NULL)          \
    {                           \
        osal_assert(__FILE__, __func__, __LINE__, #ptr " is NULL"); \
        return OSAL_INVALID_POINTER; \
    }
```

---

### OSAL_CHECK_STRING

```c
/**
 * @brief Check string pointer and length
 *
 * Combined check that validates:
 * 1. String pointer is not NULL
 * 2. String contains null terminator within maxlen bytes
 *
 * @param[in] str      String pointer to check
 * @param[in] maxlen   Maximum allowed length (including null terminator)
 * @param[in] errcode  Error code to return if validation fails
 */
#define OSAL_CHECK_STRING(str, maxlen, errcode) \
    do                                          \
    {                                           \
        OSAL_CHECK_POINTER(str);                \
        LENGTHCHECK(str, maxlen, errcode);      \
    } while (0)
```

**Note**: Requires `LENGTHCHECK` macro from `osal_macro.h`

---

## Assertion Function

### osal_assert

```c
/**
 * @brief Platform-specific assertion handler
 *
 * This function is called when an assertion fails. It logs the error
 * with file, function, and line information, then returns to allow
 * the application to handle the error gracefully.
 *
 * @param[in] file      Source file where assertion failed (__FILE__)
 * @param[in] function  Function name where assertion failed (__func__)
 * @param[in] line      Line number where assertion failed (__LINE__)
 * @param[in] message   Descriptive error message
 *
 * @note Platform-specific implementation in osal_assert.c
 */
void osal_assert(const char *file, const char *function, int line, const char *message);
```

---

## Platform-Specific Implementation

Each platform must implement `osal_assert.c` with the assertion handler.

### Example Implementation (POSIX)

```c
// osal/posix/osal_assert.c
#include <stdio.h>
#include "osal_assert.h"

void osal_assert(const char *file, const char *function, int line, const char *message) {
    fprintf(stderr, "OSAL Assertion Failed:\n");
    fprintf(stderr, "  File:     %s\n", file);
    fprintf(stderr, "  Function: %s\n", function);
    fprintf(stderr, "  Line:     %d\n", line);
    fprintf(stderr, "  Message:  %s\n", message);
    fflush(stderr);
}
```

### Example Implementation (ESP32/FreeRTOS)

```c
// osal/esp/osal_assert.c
#include "esp_log.h"
#include "osal_assert.h"

static const char *TAG = "OSAL";

void osal_assert(const char *file, const char *function, int line, const char *message) {
    ESP_LOGE(TAG, "Assertion Failed:");
    ESP_LOGE(TAG, "  File:     %s", file);
    ESP_LOGE(TAG, "  Function: %s", function);
    ESP_LOGE(TAG, "  Line:     %d", line);
    ESP_LOGE(TAG, "  Message:  %s", message);
}
```

---

## Usage Examples

### Basic Pointer Validation

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
    // Check required pointer arguments
    OSAL_CHECK_POINTER(task_id);
    OSAL_CHECK_POINTER(routine);
    OSAL_CHECK_STRING(task_name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    ARGCHECK(stack_size > 0, OSAL_ERR_INVALID_SIZE);
    
    // attr may be NULL (defaults will be used)
    // arg may be NULL (task takes no argument)
    // stack_pointer may be NULL (dynamic allocation)
    
    // Implementation...
    return OSAL_SUCCESS;
}
```

### String Validation

```c
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_set_task_name(osal_task_id_t task_id, const char *name) {
    OSAL_CHECK_POINTER(task_id);
    OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    
    // Name is valid, proceed with operation
    return OSAL_SUCCESS;
}
```

### Combined Validation

```c
osal_status_t osal_queue_create(osal_queue_id_t *queue_id,
                                const char *name,
                                uint32_t max_items, 
                                uint32_t item_size) {
    // Pointer validation
    OSAL_CHECK_POINTER(queue_id);
    
    // Optional string validation
    if (name != NULL) {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }
    
    // Argument validation
    ARGCHECK(max_items > 0, OSAL_QUEUE_INVALID_SIZE);
    ARGCHECK(item_size > 0, OSAL_ERR_INVALID_SIZE);
    
    // Implementation...
    return OSAL_SUCCESS;
}
```

---

## Validation Guidelines

### Public API Functions Must Validate

All public OSAL API functions **must** validate their arguments:

**Required Checks**:
- ✅ All output pointers (return values)
- ✅ All input pointers used for reading
- ✅ String length limits
- ✅ Handle validity (IDs, handles)
- ✅ Enum value ranges
- ✅ Size/length parameters

**Example Public Function**:
```c
osal_status_t osal_bin_sem_timed_wait(osal_bin_sem_id_t sem_id, uint32_t timeout_ms) {
    OSAL_CHECK_POINTER(sem_id);
    // Validate sem_id points to valid binary semaphore
    ARGCHECK(is_valid_bin_sem(sem_id), OSAL_ERR_INVALID_ID);
    
    // Implementation...
}
```

### Internal Functions Can Trust

Internal (static) functions may skip validation if:
- Called only by validated code paths
- Performance critical
- Validation done at API boundary

```c
// Internal helper - assumes validation done by caller
static void internal_queue_insert(queue_handle_t *queue, void *item) {
    // No validation - caller must ensure valid pointers
    memcpy(queue->buffer, item, queue->item_size);
}
```

---

## Best Practices

1. **Validate at API boundary** - All public functions must check arguments
2. **Use appropriate macro** - OSAL_CHECK_POINTER for pointers, ARGCHECK for conditions
3. **Check strings carefully** - Use OSAL_CHECK_STRING for all string inputs
4. **Document requirements** - Note validation behavior in function documentation
5. **Test error paths** - Ensure error returns are handled correctly
6. **Keep messages clear** - Assertion messages should identify the problem
7. **Don't validate twice** - Skip internal checks if already validated

---

## Common Validation Patterns

### Multiple Pointer Arguments
```c
osal_status_t function(void *ptr1, void *ptr2, void *ptr3) {
    OSAL_CHECK_POINTER(ptr1);
    OSAL_CHECK_POINTER(ptr2);
    OSAL_CHECK_POINTER(ptr3);
    // ...
}
```

### Optional String Parameter
```c
osal_status_t function(const char *optional_name) {
    if (optional_name != NULL) {
        OSAL_CHECK_STRING(optional_name, MAX_LEN, OSAL_ERR_NAME_TOO_LONG);
    }
    // ...
}
```

### Handle Validation
```c
osal_status_t function(osal_handle_t handle) {
    OSAL_CHECK_POINTER(handle);
    ARGCHECK(handle->magic == VALID_MAGIC, OSAL_ERR_INVALID_ID);
    // ...
}
```

---

## Error Message Examples

When an assertion fails, the output will look like:

```
OSAL Assertion Failed:
  File:     osal_task.c
  Function: osal_task_create
  Line:     127
  Message:  task_id is NULL
```

This provides clear debugging information to developers.

---

## Integration with OSAL

Include these headers in your OSAL implementation files:

```c
#include "osal_assert.h"  // For OSAL_CHECK_POINTER, OSAL_CHECK_STRING
#include "osal_macro.h"   // For ARGCHECK, LENGTHCHECK
#include <string.h>        // For memchr (used by LENGTHCHECK)
```

---

[← Back to Main Specification](OSAL_SPECIFICATION.md)
