# Timer API

[← Back to Main Specification](OSAL_SPECIFICATION.md)

---

## Overview

**Purpose**: Provides software timers that execute callback functions at specified intervals.

**Location**:
- Header: `osal/osal_timer.h`
- Platform-specific types: `osal_impl_timer.h` within each platform directory

---

## Platform-Specific Types (`osal_impl_timer.h`)

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

**FreeRTOS Implementation Notes**:
- Timers are managed by a dedicated timer daemon/service task
- Timer callbacks execute in the context of the timer daemon task (not ISR context)
- Timers use a command queue to communicate with the daemon task
- Command queue length configured via `configTIMER_QUEUE_LENGTH`

---

## Timer Types

**One-Shot Timer** (`auto_reload = false`):
- Executes callback once after specified period
- Enters dormant state after expiry
- Must be manually restarted

**Auto-Reload Timer** (`auto_reload = true`):
- Executes callback repeatedly at specified intervals
- Automatically restarts after each expiry
- Runs continuously until stopped

---

## Timer States

**Dormant**:
- Timer created but not started
- Expired one-shot timer that hasn't been restarted

**Active**:
- Timer is running and will execute callback when period expires
- Transition from dormant using: `osal_timer_start()`, or ISR variant

---

## Memory Allocation

**Dynamic** (typical usage — `stack_pointer = NULL`):
- Timer control block automatically allocated from system heap
- Simpler to use but requires heap space

**Static** (advanced usage — `stack_pointer != NULL`):
- User provides pre-allocated buffer via `stack_pointer` and `stack_size`
- No dynamic allocation, suitable for memory-constrained systems
- Buffer must be large enough for the platform-specific timer structure
  - FreeRTOS: `sizeof(StaticTimer_t)`
  - POSIX: `sizeof(struct osal_timer_internal)`

---

## Timer Functions

```c
/**
 * @brief Create a software timer
 * @param[out] timer_id       Returned timer ID
 * @param[in]  name           Timer name for debugging (optional)
 * @param[in]  period_ms      Timer period in milliseconds
 * @param[in]  auto_reload    true for auto-reload, false for one-shot
 * @param[in]  callback       Function to call when timer expires
 * @param[in]  callback_arg   Argument passed to callback function
 * @param[in]  stack_pointer  Pointer to pre-allocated timer buffer, or NULL for dynamic allocation
 * @param[in]  stack_size     Size of the pre-allocated buffer in bytes (ignored when stack_pointer is NULL)
 * @return OSAL status code
 *
 * @note If stack_pointer is NULL, the system allocates the timer control block dynamically.
 * @note If stack_pointer is non-NULL, the buffer must be large enough to hold the
 *       platform-specific timer structure (e.g., StaticTimer_t on FreeRTOS).
 */
osal_status_t osal_timer_create(osal_timer_id_t *timer_id,
                                const char *name,
                                uint32_t period_ms,
                                bool auto_reload,
                                void (*callback)(osal_timer_id_t),
                                void *callback_arg,
                                osal_stackptr_t stack_pointer,
                                size_t stack_size);

/**
 * @brief Start a timer (task context)
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 * @note Transitions a dormant timer to the active state.
 *       To restart an already-active timer, use osal_timer_reset().
 */
osal_status_t osal_timer_start(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Reset a running timer (task context)
 *
 * Resets the timer's expiry time so it is recalculated relative to
 * when osal_timer_reset() is called, not when the timer was originally
 * started. If the timer is dormant (not running), this function starts it.
 *
 * Typical use case: a backlight timer that restarts a 5-second countdown
 * on every key press, so the backlight stays on as long as keys are being
 * pressed and turns off 5 seconds after the last key press.
 *
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 *
 * @note FreeRTOS: Maps to xTimerReset()
 */
osal_status_t osal_timer_reset(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Stop a timer (task context)
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 */
osal_status_t osal_timer_stop(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Delete/destroy a timer
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 */
osal_status_t osal_timer_delete(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Change timer period (task context)
 * @param[in] timer_id      Timer ID
 * @param[in] new_period_ms New period in milliseconds
 * @param[in] timeout_ms    Max time to wait if command queue is full
 * @return OSAL status code
 * @note This also starts the timer if it's dormant
 */
osal_status_t osal_timer_change_period(osal_timer_id_t timer_id, 
                                       uint32_t new_period_ms,
                                       uint32_t timeout_ms);

/**
 * @brief Check if timer is active
 * @param[in] timer_id  Timer ID
 * @return true if timer is active, false if dormant
 */
bool osal_timer_is_active(osal_timer_id_t timer_id);

/**
 * @brief Start a timer from ISR context
 * @param[in] timer_id  Timer ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_timer_start_from_isr(osal_timer_id_t timer_id);

/**
 * @brief Stop a timer from ISR context
 * @param[in] timer_id  Timer ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_timer_stop_from_isr(osal_timer_id_t timer_id);

/**
 * @brief Reset a running timer from ISR context
 *
 * ISR-safe variant of osal_timer_reset(). Recalculates the timer's
 * expiry time relative to now. If the timer is dormant, starts it.
 *
 * @param[in] timer_id  Timer ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 * @note FreeRTOS: Maps to xTimerResetFromISR()
 */
osal_status_t osal_timer_reset_from_isr(osal_timer_id_t timer_id);

/**
 * @brief Get the user context associated with a timer
 *
 * Retrieves the context pointer that was set during osal_timer_create()
 * (via callback_arg) or later via osal_timer_set_context().
 * This is typically called from within a timer callback to access
 * user-defined data.
 *
 * @param[in] timer_id  Timer ID
 * @return Pointer to user context, or NULL if none was set
 */
void *osal_timer_get_context(osal_timer_id_t timer_id);

/**
 * @brief Set or update the user context associated with a timer
 *
 * Associates an arbitrary user pointer with the timer. This can be
 * retrieved later with osal_timer_get_context(), typically from
 * within the timer callback.
 *
 * @param[in] timer_id  Timer ID
 * @param[in] context   Pointer to user data (may be NULL to clear)
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Context set successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid timer ID
 */
osal_status_t osal_timer_set_context(osal_timer_id_t timer_id, void *context);
```

---

## Usage Examples

### Simple One-Shot Timer

```c
#include "osal_timer.h"
#include "osal_log.h"

void timeout_callback(osal_timer_id_t timer_id) {
    osal_log_info("Timer expired!");
    // Perform timeout action
    handle_timeout();
}

void init_timeout_timer(void) {
    osal_timer_id_t timer;
    
    // Create 5-second one-shot timer (dynamic allocation)
    osal_timer_create(&timer, "Timeout", 5000, false, timeout_callback, NULL,
                      NULL, 0);
    
    // Start timer
    osal_timer_start(timer, 100);
}
```

### Auto-Reload Periodic Timer

```c
void periodic_callback(osal_timer_id_t timer_id) {
    // Called every second
    update_status_led();
}

void init_periodic_timer(void) {
    osal_timer_id_t timer;
    
    // Create auto-reload timer (1 second period, dynamic allocation)
    osal_timer_create(&timer, "StatusLED", 1000, true, periodic_callback, NULL,
                      NULL, 0);
    
    // Start timer
    osal_timer_start(timer, 0);
}
```

### Timer with Custom Data

```c
typedef struct {
    int counter;
    const char *name;
} timer_context_t;

void counter_callback(osal_timer_id_t timer_id) {
    timer_context_t *ctx = (timer_context_t *)osal_timer_get_context(timer_id);
    
    ctx->counter++;
    osal_log_info("%s: count = %d", ctx->name, ctx->counter);
    
    if (ctx->counter >= 10) {
        // Stop after 10 executions
        osal_timer_stop(timer_id, 100);
    }
}

void init_counter_timer(void) {
    static timer_context_t ctx = {0, "MyCounter"};
    osal_timer_id_t timer;
    
    osal_timer_create(&timer, "Counter", 500, true, counter_callback, &ctx,
                      NULL, 0);
    osal_timer_start(timer, 0);
}
```

### Dynamic Period Adjustment

```c
osal_timer_id_t adaptive_timer;

void adaptive_callback(osal_timer_id_t timer_id) {
    // Callback that adjusts its own period based on conditions
    uint32_t new_period;
    
    if (system_busy()) {
        new_period = 2000;  // Slow down to 2 seconds
    } else {
        new_period = 500;   // Speed up to 500ms
    }
    
    osal_timer_change_period(timer_id, new_period, 100);
}

void init_adaptive_timer(void) {
    osal_timer_create(&adaptive_timer, "Adaptive", 1000, true, 
                      adaptive_callback, NULL, NULL, 0);
    osal_timer_start(adaptive_timer, 0);
}
```

### Backlight Timeout Example (from FreeRTOS docs)

A common use for timer reset: keep a backlight on as long as keys are being
pressed, and turn it off 5 seconds after the last key press.

```c
osal_timer_id_t backlight_timer;

void backlight_timer_callback(osal_timer_id_t timer_id) {
    // Timer expired - turn off backlight
    set_backlight(false);
}

void key_press_handler(void) {
    // Turn on backlight
    set_backlight(true);
    
    // Reset timer — restart the 5-second countdown from now
    osal_timer_reset(backlight_timer, 10);
}

void init_backlight_control(void) {
    // Create one-shot 5-second timer (dynamic allocation)
    osal_timer_create(&backlight_timer, "Backlight", 5000, false, 
                      backlight_timer_callback, NULL, NULL, 0);
}
```

### Creating a Timer with Static Allocation

```c
#include "osal_timer.h"
#include "osal_impl_timer.h"  // For platform-specific timer buffer size

// Pre-allocate timer control block buffer
static uint8_t heartbeat_timer_buf[OSAL_TIMER_STATIC_SIZE];

void heartbeat_callback(osal_timer_id_t timer_id) {
    toggle_heartbeat_led();
}

void init_heartbeat_timer(void) {
    osal_timer_id_t timer;
    
    // Create auto-reload timer with static allocation
    osal_timer_create(&timer, "Heartbeat", 500, true,
                      heartbeat_callback, NULL,
                      (osal_stackptr_t)heartbeat_timer_buf,
                      sizeof(heartbeat_timer_buf));
    
    osal_timer_start(timer, 0);
}
```

> **Note**: `OSAL_TIMER_STATIC_SIZE` is a platform-defined constant in `osal_impl_timer.h`,
> representing the minimum buffer size required for a timer control block (e.g.,
> `sizeof(StaticTimer_t)` on FreeRTOS, `sizeof(struct osal_timer_internal)` on POSIX).

### ISR-Triggered Timer

```c
osal_timer_id_t debounce_timer;

void button_isr(void) {
    // Start debounce timer from ISR
    // Platform handles context switching automatically
    osal_timer_start_from_isr(debounce_timer);
}

void debounce_callback(osal_timer_id_t timer_id) {
    // Debounce period expired - read button state
    if (read_button()) {
        handle_button_press();
    }
}

void init_button_debounce(void) {
    osal_timer_create(&debounce_timer, "Debounce", 50, false, 
                      debounce_callback, NULL, NULL, 0);
}
```

---

## Important Notes

### Timer Daemon Task Priority

- Configured via `configTIMER_TASK_PRIORITY` in `FreeRTOSConfig.h`
- Affects when timer commands are processed relative to other tasks
- Higher priority = more responsive timers, but may delay other tasks

### Callback Function Constraints

- Executes in timer daemon task context
- Keep callbacks short and efficient
- Do NOT call blocking functions with non-zero timeouts from callbacks
- Avoid deadlock: never block indefinitely in a callback

```c
// ❌ WRONG - Blocking in callback
void bad_callback(osal_timer_id_t timer_id) {
    osal_bin_sem_take(sem);  // Can cause deadlock!
}

// ✅ CORRECT - Non-blocking or zero timeout
void good_callback(osal_timer_id_t timer_id) {
    if (osal_bin_sem_timed_wait(sem, 0) == OSAL_SUCCESS) {  // Non-blocking
        // Got semaphore
    }
}
```

---

## Best Practices

1. **Use meaningful timer names** for easier debugging
2. **Keep callbacks short** - offload heavy work to separate tasks
3. **Check timer state** before operating on dormant timers
4. **Set appropriate command queue timeout** - usually 0 or small value
5. **Don't create/delete timers frequently** - reuse timers when possible
6. **Monitor timer daemon queue** - may need to increase `configTIMER_QUEUE_LENGTH`
7. **Test timer accuracy** - software timers have limited precision
8. **Use hardware timers** for microsecond-level precision requirements

---

## Common Pitfalls

### ❌ Wrong: Blocking in callback
```c
void timer_callback(osal_timer_id_t timer_id) {
    osal_task_delay_ms(1000);  // DEADLOCK! Blocks timer daemon
}
```

### ✅ Correct: Signal task from callback
```c
osal_bin_sem_id_t work_sem;

void timer_callback(osal_timer_id_t timer_id) {
    osal_bin_sem_give(work_sem);  // Just signal, don't block
}

void worker_task(void *arg) {
    while (1) {
        osal_bin_sem_take(work_sem);
        do_heavy_work();  // Heavy work in separate task
    }
}
```

### ❌ Wrong: Forgetting to start timer
```c
osal_timer_id_t timer;
osal_timer_create(&timer, "MyTimer", 1000, true, callback, NULL, NULL, 0);
// Timer created but never started - callback never called!
```

### ✅ Correct: Start after creation
```c
osal_timer_id_t timer;
osal_timer_create(&timer, "MyTimer", 1000, true, callback, NULL, NULL, 0);
osal_timer_start(timer, 0);  // Start the timer
```

---

## Timer Accuracy

**Resolution**: Typically tied to system tick rate (1-10ms)

**Jitter**: Callbacks may be delayed by:
- Higher priority tasks
- Timer daemon task priority
- Command queue congestion
- Interrupt latency

**For High Precision**:
- Use hardware timers directly
- Keep timer daemon at high priority
- Minimize ISR execution time
- Reduce system load

---

## Platform-Specific Notes

### POSIX Implementation
- May use `timer_create()` and POSIX interval timers
- Or software implementation with threads and `nanosleep()`
- All `_from_isr()` variants return `OSAL_ERR_NOT_IMPLEMENTED` — POSIX has no ISR context

### FreeRTOS/ESP32 Implementation
- Native FreeRTOS software timers
- Timer daemon task must be enabled in configuration
- Full ISR support with automatic context switching
- Platform implementation manages `portYIELD_FROM_ISR()` internally
- Typical tick rate: 100-1000 Hz (1-10ms resolution)
- Dynamic: `xTimerCreate()` — when `stack_pointer == NULL`
- Static: `xTimerCreateStatic()` — when `stack_pointer != NULL`, buffer must hold `StaticTimer_t`

---

## Argument Validation

All public timer functions validate their arguments:

- **Pointer checks**: Timer IDs and callback function pointers validated as non-NULL
- **String validation**: Timer names checked for maximum length
- **Parameter validation**: Period values, timeout values checked for valid ranges

**Implementation Requirements**:
```c
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_timer_create(osal_timer_id_t *timer_id,
                                const char *name,
                                uint32_t period_ms,
                                bool auto_reload,
                                void (*callback)(osal_timer_id_t),
                                void *callback_arg,
                                osal_stackptr_t stack_pointer,
                                size_t stack_size) {
    OSAL_CHECK_POINTER(timer_id);
    OSAL_CHECK_POINTER(callback);
    
    if (name != NULL) {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }
    
    ARGCHECK(period_ms > 0, OSAL_TIMER_ERR_INVALID_ARGS);
    
    // stack_pointer may be NULL (dynamic allocation)
    // If non-NULL, stack_size must be sufficient for platform timer structure
    if (stack_pointer != NULL) {
        ARGCHECK(stack_size > 0, OSAL_ERR_INVALID_SIZE);
    }
    
    // Implementation...
}
```

See [OSAL Assertions](OSAL_Assertions.md) for complete validation guidelines.

---

[← Back to Main Specification](OSAL_SPECIFICATION.md)
