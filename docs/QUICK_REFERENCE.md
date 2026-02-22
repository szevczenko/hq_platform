# OSAL Quick Reference

## Build Commands

### POSIX (Linux/macOS)
```bash
mkdir -p build && cd build
cmake -DHQ_DEFCONFIG=../defconfig/posix.defconfig ..
cmake --build .
```

### ESP32 (integrated in ESP-IDF)
```bash
# In ESP-IDF project component folder
cp -r hq_platform/src/osal .
idf.py build
```

## Configuration

### Platform Selection
Edit `defconfig/*` file:
- `CONFIG_HQ_PLATFORM_POSIX=y` for POSIX
- `CONFIG_HQ_PLATFORM_ESP=y` for ESP32

### Log Level Control
```
CONFIG_OSAL_LOG_LEVEL=3  # 0=DEBUG, 1=INFO, 2=WARNING, 3=ERROR
```

## API Cheat Sheet

### Task Management
```c
osal_task_handle_t handle = osal_task_create("mytask", priority, stack_size, task_func, arg);
osal_task_delete(handle);
osal_task_prio_set(handle, new_priority);
osal_task_yield();
```

### Semaphores
```c
// Binary
osal_bin_sem_handle_t sem = osal_bin_sem_create();
osal_bin_sem_give(sem);
osal_bin_sem_take(sem, OSAL_MAX_DELAY);

// Counting
osal_count_sem_handle_t csem = osal_count_sem_create(max_count, initial_count);
osal_count_sem_give(csem);
osal_count_sem_take(csem, timeout_ms);
```

### Mutex
```c
osal_mutex_handle_t mutex = osal_mutex_create();
osal_mutex_lock(mutex);
if (osal_mutex_trylock(mutex) == OSAL_OK) { /* locked */ }
osal_mutex_unlock(mutex);
osal_mutex_delete(mutex);
```

### Queue
```c
osal_queue_handle_t q = osal_queue_create(msg_size, max_msgs);
osal_queue_send(q, msg_ptr, timeout_ms);
osal_queue_receive(q, msg_ptr, timeout_ms);
osal_queue_delete(q);
```

### Timer
```c
osal_timer_handle_t timer = osal_timer_create("timer", period_ms, periodic, callback, arg);
osal_timer_start(timer);
osal_timer_stop(timer);
osal_timer_delete(timer);
```

### Logging
```c
OSAL_LOG_ERROR("Error: %d", code);
OSAL_LOG_WARNING("Warning message");
OSAL_LOG_INFO("Info message");
OSAL_LOG_DEBUG("Debug message");

osal_log_set_level(OSAL_LOG_LEVEL_DEBUG);  // Runtime control
```

### Assertions
```c
OSAL_ASSERT(condition);
OSAL_ASSERT_MSG(condition, "Error message");
```

## Return Codes
```c
OSAL_OK                   // Success
OSAL_ERROR                // Generic error
OSAL_QUEUE_TIMEOUT        // Queue operation timed out
OSAL_QUEUE_EMPTY          // Queue is empty
OSAL_EINVAL               // Invalid parameter
OSAL_ENOMEM               // Out of memory
OSAL_EBUSY                // Resource busy
OSAL_ETIMEOUT             // Operation timed out
OSAL_ENOENT               // No such entity
OSAL_EINTR                // Interrupted system call
```

## File Structure
```
src/osal/
├── include/              # Public API headers
├── common/               # Shared code
├── posix/                # POSIX implementations
└── esp/                  # FreeRTOS implementations
```

## Limits & Constraints

| Item | Limit | Notes |
|------|-------|-------|
| Task Name | 32 chars | Null-terminated |
| Queue Message Size | 1-4096 bytes | Configurable per queue |
| Task Priority Range | 0-31 (POSIX) | Platform-specific |
| Task Stack Min | 2 KB | Typical minimum |
| Timer Period | 1-2^31 ms | 32-bit milliseconds |
| Binary Semaphore Max | 1 | By definition |
| Counting Semaphore Max | Configurable | Set on creation |

## Common Issues & Fixes

**"Platform not selected"**
→ Use `-DHQ_DEFCONFIG=../defconfig/posix.defconfig` in cmake

**"Conflicting platforms"**
→ Set exactly ONE of POSIX or ESP to 'y' in defconfig

**"Undefined reference to pthread_*"**
→ Link with `-lpthread` on POSIX systems

**Queue full on send()**
→ Queue is non-blocking on full; increase max_msgs or use separate producer thread

**Timer callback not firing**
→ Check callback return value; return with restart required for periodic mode

## Module Mapping

| OSAL Module | POSIX Impl | ESP Impl |
|-------------|-----------|---------|
| Task | pthread | FreeRTOS xTask |
| Binary Sem | POSIX sem | FreeRTOS xSemaphore |
| Counting Sem | POSIX sem | FreeRTOS xSemaphore |
| Mutex | pthread_mutex | FreeRTOS xMutex |
| Queue | Custom ring buffer | FreeRTOS xQueue |
| Timer | timer_create + thread | FreeRTOS xTimer |
| Logging | stderr | ESP_LOG |
| Assert | abort() | esp_restart() |

## Performance Characteristics

### POSIX
- Task overhead: ~96 bytes per task struct
- Queue overhead: ~24 bytes + (msg_size × depth)
- Timer overhead: 1 kernel timer + 1 thread per timer
- Context switch: ~1-10 microseconds (system-dependent)

### ESP/FreeRTOS
- Task overhead: ~16 bytes handle + TCB (~240 bytes)
- Queue overhead: FreeRTOS queue struct (~144 bytes) + msgs
- Timer overhead: Shared daemon task (~20 timers per task)
- Context switch: ~2-5 microseconds (typical)

## Integration Checklist

- [ ] Build succeeds with zero warnings
- [ ] `libhq_osal.a` (POSIX) or component linked (ESP)
- [ ] `#include <osal/osal_*.h>` resolves correctly
- [ ] Link with `-lhq_osal -lpthread` (POSIX only)
- [ ] Logging output appears as expected
- [ ] Basic task/semaphore operations work
- [ ] Cross-platform code uses only provided APIs

## Documentation Files

- `OSAL_SPECIFICATION.md` - Complete API spec
- `OSAL_Task_Management.md` - Task details
- `OSAL_Semaphore_API.md` - Semaphore details
- `OSAL_Queue_API.md` - Queue implementation spec
- `OSAL_Timer_API.md` - Timer details
- `OSAL_Assertions.md` - Assert framework
- `BUILD.md` - Build & compilation guide
- `IMPLEMENTATION.md` - Architecture & design details
