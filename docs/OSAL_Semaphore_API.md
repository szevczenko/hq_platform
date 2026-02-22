# Semaphore API

[← Back to Main Specification](OSAL_SPECIFICATION.md)

---

## Overview

**Purpose**: Provides synchronization primitives for tasks and interrupt service routines.

**Location**: 
- Header: `osal/osal_bin_sem.h`, `osal/osal_count_sem.h`, `osal/osal_mutex.h`
- Platform-specific types: `osal_impl_sem.h` within each platform directory

---

## Platform-Specific Types (`osal_impl_sem.h`)

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

OSAL provides three main types of synchronization primitives:

| Type | Description |
|------|-------------|
| **Binary Semaphores** | Two-state (0/1) semaphores used for task synchronization and signaling |
| **Counting Semaphores** | Multi-state semaphores for resource management and task coordination |
| **Mutex Semaphores** | Mutual exclusion objects for protecting critical sections and shared resources |

---

## FreeRTOS Implementation Notes

**Reference**: [FreeRTOS Semaphore Documentation](http://www.freertos.org/RTOS-task-notifications.html)

**Binary Semaphores**:
- **Performance**: Direct-to-task notifications are often faster and more memory-efficient
- **Memory**: `xSemaphoreCreateBinary()` (dynamic) / `xSemaphoreCreateBinaryStatic()` (static)
- **Initial State**: Created in "unavailable" state (must be given before taking)
- **Limitations**: Do **not** use priority inheritance (use mutexes for that)

**Counting Semaphores**:
- **Memory**: `xSemaphoreCreateCounting()` (dynamic) / `xSemaphoreCreateCountingStatic()` (static)
- **Use Cases**: Resource pool management, event counting, producer-consumer synchronization

**Mutex Semaphores**:
- **Memory**: `xSemaphoreCreateMutex()` (dynamic) / `xSemaphoreCreateMutexStatic()` (static)
- **Priority Inheritance**: Mutexes support priority inheritance to prevent priority inversion
- **Ownership**: Only the task that took the mutex can give it back
- **Limitations**: Must NOT be used from ISR context

---

## Special Constants

```c
/** @brief Semaphore initial value: empty/unavailable (0) */
#define OSAL_SEM_EMPTY  0

/** @brief Semaphore initial value: full/available (1) */
#define OSAL_SEM_FULL   1

/** @brief Infinite timeout for blocking operations */
#define OSAL_MAX_DELAY  0xFFFFFFFF
```

**Location**: Define in `osal_common_type.h`

---

## 1. Binary Semaphores

Binary semaphores are simple signaling mechanisms that can be in one of two states: available (value = 1) or unavailable (value = 0). They enable tasks to coordinate their execution by waiting for signals from other tasks or ISRs.

### Binary Semaphore Functions

```c
/**
 * @brief Create a binary semaphore
 * @param[out] sem_id        Returned semaphore ID
 * @param[in]  name          Semaphore name for debugging (optional, may be NULL)
 * @param[in]  initial_value Initial value: OSAL_SEM_EMPTY (0) or OSAL_SEM_FULL (1)
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Semaphore created successfully
 * @retval OSAL_ERROR             Creation failed
 * @retval OSAL_INVALID_POINTER   sem_id is NULL
 * @retval OSAL_INVALID_SEM_VALUE Invalid initial_value (not 0 or 1)
 */
osal_status_t osal_bin_sem_create(osal_bin_sem_id_t *sem_id,
                                  const char *name,
                                  uint32_t initial_value);

/**
 * @brief Delete a binary semaphore
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Semaphore deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid semaphore ID
 */
osal_status_t osal_bin_sem_delete(osal_bin_sem_id_t sem_id);

/**
 * @brief Give/release a binary semaphore (task context)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Do NOT use from ISR - use osal_bin_sem_give_from_isr() instead
 */
osal_status_t osal_bin_sem_give(osal_bin_sem_id_t sem_id);

/**
 * @brief Take/acquire a binary semaphore (blocking, infinite wait)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_FAILURE   Operation failed
 */
osal_status_t osal_bin_sem_take(osal_bin_sem_id_t sem_id);

/**
 * @brief Take/acquire a binary semaphore with timeout
 * @param[in] sem_id        Semaphore ID
 * @param[in] timeout_ms    Timeout in milliseconds (OSAL_MAX_DELAY for infinite)
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_TIMEOUT   Timeout expired
 */
osal_status_t osal_bin_sem_timed_wait(osal_bin_sem_id_t sem_id, uint32_t timeout_ms);

/**
 * @brief Give/release a binary semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_bin_sem_give_from_isr(osal_bin_sem_id_t sem_id);

/**
 * @brief Take/acquire a binary semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_bin_sem_take_from_isr(osal_bin_sem_id_t sem_id);
```

### Binary Semaphore Summary

| Function | Description |
|----------|-------------|
| `osal_bin_sem_create` | Creates a binary semaphore with initial value |
| `osal_bin_sem_delete` | Deletes a binary semaphore |
| `osal_bin_sem_give` | Releases a binary semaphore (task context) |
| `osal_bin_sem_take` | Takes a binary semaphore, blocking indefinitely |
| `osal_bin_sem_timed_wait` | Takes a binary semaphore with timeout |
| `osal_bin_sem_give_from_isr` | Releases a binary semaphore (ISR context) |
| `osal_bin_sem_take_from_isr` | Takes a binary semaphore (ISR context) |

---

## 2. Counting Semaphores

Counting semaphores maintain a counter value that can be incremented and decremented. They are typically used to track finite resources or to impose a limit on concurrent accesses.

### Counting Semaphore Functions

```c
/**
 * @brief Create a counting semaphore
 * @param[out] sem_id         Returned semaphore ID
 * @param[in]  name           Semaphore name for debugging (optional, may be NULL)
 * @param[in]  initial_value  Initial counter value
 * @param[in]  max_value      Maximum counter value (0 = no limit / platform default)
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Semaphore created successfully
 * @retval OSAL_ERROR             Creation failed
 * @retval OSAL_INVALID_POINTER   sem_id is NULL
 */
osal_status_t osal_count_sem_create(osal_count_sem_id_t *sem_id,
                                    const char *name,
                                    uint32_t initial_value,
                                    uint32_t max_value);

/**
 * @brief Delete a counting semaphore
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Semaphore deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid semaphore ID
 */
osal_status_t osal_count_sem_delete(osal_count_sem_id_t sem_id);

/**
 * @brief Give/increment a counting semaphore (task context)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Do NOT use from ISR - use osal_count_sem_give_from_isr() instead
 */
osal_status_t osal_count_sem_give(osal_count_sem_id_t sem_id);

/**
 * @brief Take/decrement a counting semaphore (blocking, infinite wait)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_FAILURE   Operation failed
 */
osal_status_t osal_count_sem_take(osal_count_sem_id_t sem_id);

/**
 * @brief Take/decrement a counting semaphore with timeout
 * @param[in] sem_id        Semaphore ID
 * @param[in] timeout_ms    Timeout in milliseconds (OSAL_MAX_DELAY for infinite)
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_TIMEOUT   Timeout expired
 */
osal_status_t osal_count_sem_timed_wait(osal_count_sem_id_t sem_id, uint32_t timeout_ms);

/**
 * @brief Give/increment a counting semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_count_sem_give_from_isr(osal_count_sem_id_t sem_id);

/**
 * @brief Take/decrement a counting semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_count_sem_take_from_isr(osal_count_sem_id_t sem_id);

/**
 * @brief Get the current count value of a counting semaphore
 * @param[in] sem_id  Semaphore ID
 * @return Current count value
 */
uint32_t osal_count_sem_get_count(osal_count_sem_id_t sem_id);
```

### Counting Semaphore Summary

| Function | Description |
|----------|-------------|
| `osal_count_sem_create` | Creates a counting semaphore with initial/max values |
| `osal_count_sem_delete` | Deletes a counting semaphore |
| `osal_count_sem_give` | Increments a counting semaphore (task context) |
| `osal_count_sem_take` | Decrements a counting semaphore, blocking indefinitely |
| `osal_count_sem_timed_wait` | Decrements a counting semaphore with timeout |
| `osal_count_sem_give_from_isr` | Increments a counting semaphore (ISR context) |
| `osal_count_sem_take_from_isr` | Decrements a counting semaphore (ISR context) |
| `osal_count_sem_get_count` | Gets the current count value |

---

## 3. Mutex Semaphores

Mutex semaphores (mutexes) are special synchronization objects used specifically for mutual exclusion. They ensure that only one task can access a shared resource at a time, preventing race conditions. Mutexes support priority inheritance to prevent priority inversion.

### Mutex Functions

```c
/**
 * @brief Create a mutex
 * @param[out] mutex_id  Returned mutex ID
 * @param[in]  name      Mutex name for debugging (optional, may be NULL)
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Mutex created successfully
 * @retval OSAL_ERROR             Creation failed
 * @retval OSAL_INVALID_POINTER   mutex_id is NULL
 */
osal_status_t osal_mutex_create(osal_mutex_id_t *mutex_id,
                                const char *name);

/**
 * @brief Delete a mutex
 * @param[in] mutex_id  Mutex ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Mutex deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid mutex ID
 */
osal_status_t osal_mutex_delete(osal_mutex_id_t mutex_id);

/**
 * @brief Take/lock a mutex (blocking, infinite wait)
 * @param[in] mutex_id  Mutex ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Mutex acquired
 * @retval OSAL_SEM_FAILURE   Operation failed
 * @note Must NOT be called from ISR context
 * @note Only the task that took the mutex may give it back
 */
osal_status_t osal_mutex_take(osal_mutex_id_t mutex_id);

/**
 * @brief Give/unlock a mutex
 * @param[in] mutex_id  Mutex ID
 * @return OSAL status code
 * @note Must NOT be called from ISR context
 * @note Must be called by the same task that took the mutex
 */
osal_status_t osal_mutex_give(osal_mutex_id_t mutex_id);
```

### Mutex Summary

| Function | Description |
|----------|-------------|
| `osal_mutex_create` | Creates a mutex |
| `osal_mutex_delete` | Deletes a mutex |
| `osal_mutex_take` | Locks a mutex, blocking indefinitely |
| `osal_mutex_give` | Unlocks a mutex |

> **Note**: Mutexes do **NOT** have ISR variants. Mutexes must not be used from ISR context because they implement ownership semantics and priority inheritance, which are meaningless in interrupt context. Use binary semaphores for ISR-to-task signaling.

---

## Usage Examples

### Binary Semaphore: Task-to-Task Synchronization

```c
#include "osal_bin_sem.h"
#include "osal_task.h"

osal_bin_sem_id_t sync_sem;

void producer_task(void *arg) {
    // Create binary semaphore (initially empty)
    osal_bin_sem_create(&sync_sem, "SyncSem", OSAL_SEM_EMPTY);
    
    while (1) {
        // Do work
        process_data();
        
        // Signal consumer
        osal_bin_sem_give(sync_sem);
        
        osal_task_delay_ms(100);
    }
}

void consumer_task(void *arg) {
    while (1) {
        // Wait for signal from producer (infinite wait)
        if (osal_bin_sem_take(sync_sem) == OSAL_SUCCESS) {
            // Process signaled data
            consume_data();
        }
    }
}
```

### Binary Semaphore: ISR to Task Synchronization

```c
osal_bin_sem_id_t isr_sem;

// ISR handler
void my_interrupt_handler(void) {
    // Signal task from ISR
    // Platform handles context switching automatically
    osal_bin_sem_give_from_isr(isr_sem);
}

// Task waiting for interrupt
void interrupt_handler_task(void *arg) {
    osal_bin_sem_create(&isr_sem, "IsrSem", OSAL_SEM_EMPTY);
    
    while (1) {
        // Wait for ISR to signal
        if (osal_bin_sem_take(isr_sem) == OSAL_SUCCESS) {
            // Handle interrupt event
            handle_interrupt_event();
        }
    }
}
```

### Binary Semaphore: Timed Wait

```c
void task_with_timeout(void *arg) {
    osal_bin_sem_id_t sem;
    osal_bin_sem_create(&sem, "TimeoutSem", OSAL_SEM_EMPTY);
    
    // Try to acquire with 500ms timeout
    osal_status_t status = osal_bin_sem_timed_wait(sem, 500);
    
    if (status == OSAL_SUCCESS) {
        // Got the semaphore
        handle_event();
    } else if (status == OSAL_SEM_TIMEOUT) {
        // Timeout occurred
        handle_timeout();
    }
}
```

### Counting Semaphore: Resource Pool Management

```c
#include "osal_count_sem.h"

#define MAX_CONNECTIONS 5
osal_count_sem_id_t connection_pool;

void init_connection_pool(void) {
    // Create counting semaphore: 5 available connections
    osal_count_sem_create(&connection_pool, "ConnPool", MAX_CONNECTIONS, MAX_CONNECTIONS);
}

void client_task(void *arg) {
    // Acquire a connection (decrement, wait up to 1000ms)
    osal_status_t status = osal_count_sem_timed_wait(connection_pool, 1000);
    
    if (status == OSAL_SUCCESS) {
        // Use the connection
        perform_network_operation();
        
        // Release the connection (increment)
        osal_count_sem_give(connection_pool);
    } else if (status == OSAL_SEM_TIMEOUT) {
        osal_log_warning("No connections available");
    }
}
```

### Counting Semaphore: Event Counting from ISR

```c
osal_count_sem_id_t event_counter;

void init_event_counter(void) {
    osal_count_sem_create(&event_counter, "Events", 0, 100);
}

void gpio_isr_handler(void) {
    // Count event from ISR
    osal_count_sem_give_from_isr(event_counter);
}

void event_processor_task(void *arg) {
    while (1) {
        // Wait for events (blocks until counter > 0)
        if (osal_count_sem_take(event_counter) == OSAL_SUCCESS) {
            process_event();
        }
    }
}
```

### Mutex: Critical Section Protection

```c
#include "osal_mutex.h"

osal_mutex_id_t resource_mutex;
uint32_t shared_resource = 0;

void init_shared_resource(void) {
    osal_mutex_create(&resource_mutex, "ResMutex");
}

void task_a(void *arg) {
    while (1) {
        // Enter critical section
        if (osal_mutex_take(resource_mutex) == OSAL_SUCCESS) {
            // Access shared resource safely
            shared_resource++;
            
            // Exit critical section
            osal_mutex_give(resource_mutex);
        }
        
        osal_task_delay_ms(100);
    }
}

void task_b(void *arg) {
    while (1) {
        // Enter critical section
        if (osal_mutex_take(resource_mutex) == OSAL_SUCCESS) {
            // Read shared resource safely
            uint32_t value = shared_resource;
            
            // Exit critical section
            osal_mutex_give(resource_mutex);
            
            process_value(value);
        }
        
        osal_task_delay_ms(200);
    }
}
```

---

## Choosing the Right Primitive

| Need | Use | Why |
|------|-----|-----|
| Task-to-task signaling | Binary Semaphore | Lightweight, simple give/take pattern |
| ISR-to-task signaling | Binary Semaphore | Supports ISR variants, no ownership |
| Resource pool tracking | Counting Semaphore | Counter tracks available resources |
| Event counting | Counting Semaphore | Each give increments, each take decrements |
| Protecting shared data | Mutex | Priority inheritance prevents inversion |
| Exclusive resource access | Mutex | Ownership semantics prevent accidental release |

---

## Best Practices

1. **Create semaphores/mutexes during initialization**, not in performance-critical code
2. **Always check return values** from all operations
3. **Use appropriate timeouts** - avoid infinite waits unless absolutely necessary
4. **Match give/take pairs** properly to avoid deadlocks
5. **Use mutexes for mutual exclusion** - they support priority inheritance
6. **Use binary semaphores for signaling** between tasks/ISRs
7. **Use counting semaphores for resource pools** and event counting
8. **Never call non-ISR functions from ISR** - always use `_from_isr()` variants
9. **Never use mutexes from ISR** - they have ownership semantics
10. **Always release mutexes** from the same task that acquired them

---

## Common Pitfalls

### ❌ Wrong: Using task function in ISR
```c
void isr_handler(void) {
    osal_bin_sem_give(sem);  // WRONG! Must use ISR variant
}
```

### ✅ Correct: Using ISR-safe function
```c
void isr_handler(void) {
    osal_bin_sem_give_from_isr(sem);
    // Platform handles context switching automatically
}
```

### ❌ Wrong: Using mutex from ISR
```c
void isr_handler(void) {
    osal_mutex_take(mutex);  // WRONG! Mutexes cannot be used in ISR
    shared_data = value;
    osal_mutex_give(mutex);
}
```

### ✅ Correct: Use binary semaphore to signal task
```c
void isr_handler(void) {
    osal_bin_sem_give_from_isr(sem);  // Signal task to do the work
}

void worker_task(void *arg) {
    while (1) {
        if (osal_bin_sem_take(sem) == OSAL_SUCCESS) {
            osal_mutex_take(mutex);
            shared_data = read_isr_buffer();
            osal_mutex_give(mutex);
        }
    }
}
```

### ❌ Wrong: Forgetting to give mutex
```c
void task(void *arg) {
    osal_mutex_take(mutex);
    critical_section();
    // Missing osal_mutex_give() - causes deadlock!
}
```

### ✅ Correct: Always release acquired mutex
```c
void task(void *arg) {
    if (osal_mutex_take(mutex) == OSAL_SUCCESS) {
        critical_section();
        osal_mutex_give(mutex);  // Always release
    }
}
```

### ❌ Wrong: Giving mutex from different task
```c
void task_a(void *arg) {
    osal_mutex_take(mutex);
    // ...
}

void task_b(void *arg) {
    osal_mutex_give(mutex);  // WRONG! Only task_a can release it
}
```

---

## Platform-Specific Notes

### POSIX Implementation
- Binary semaphores map to `sem_t` from `<semaphore.h>`
- Counting semaphores map to `sem_t` with initial value > 1
- Mutex semaphores map to `pthread_mutex_t`
- All `_from_isr()` variants return `OSAL_ERR_NOT_IMPLEMENTED` — POSIX has no ISR context

### FreeRTOS/ESP32 Implementation
- Binary semaphores: `xSemaphoreCreateBinary()` / `xSemaphoreCreateBinaryStatic()`
- Counting semaphores: `xSemaphoreCreateCounting()` / `xSemaphoreCreateCountingStatic()`
- Mutexes: `xSemaphoreCreateMutex()` / `xSemaphoreCreateMutexStatic()`
- Full ISR support for binary and counting semaphores with automatic context switching
- Platform implementation manages `portYIELD_FROM_ISR()` internally
- Mutexes support priority inheritance natively
- Consider using direct-to-task notifications for better performance when possible

---

## Argument Validation

All public semaphore and mutex functions validate their arguments:

- **Pointer checks**: All ID output pointers validated as non-NULL
- **Handle validation**: Semaphore/mutex IDs checked for validity
- **Value validation**: Initial values checked for valid ranges

**Implementation Requirements**:
```c
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_bin_sem_create(osal_bin_sem_id_t *sem_id,
                                  const char *name,
                                  uint32_t initial_value) {
    OSAL_CHECK_POINTER(sem_id);
    ARGCHECK(initial_value <= OSAL_SEM_FULL, OSAL_INVALID_SEM_VALUE);
    
    if (name != NULL) {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }
    // Implementation...
}

osal_status_t osal_bin_sem_timed_wait(osal_bin_sem_id_t sem_id, uint32_t timeout_ms) {
    OSAL_CHECK_POINTER(sem_id);
    ARGCHECK(is_valid_bin_sem(sem_id), OSAL_ERR_INVALID_ID);
    // Implementation...
}

osal_status_t osal_mutex_take(osal_mutex_id_t mutex_id) {
    OSAL_CHECK_POINTER(mutex_id);
    ARGCHECK(is_valid_mutex(mutex_id), OSAL_ERR_INVALID_ID);
    // Implementation...
}
```

See [OSAL Assertions](OSAL_Assertions.md) for complete validation guidelines.

---

[← Back to Main Specification](OSAL_SPECIFICATION.md)
