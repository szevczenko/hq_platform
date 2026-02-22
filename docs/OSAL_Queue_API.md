# Queue API

[← Back to Main Specification](OSAL_SPECIFICATION.md)

---

## Overview

**Purpose**: Provides thread-safe message passing between tasks and ISRs using FIFO queues.

**Location**:
- Header: `osal/osal_queue.h`
- Platform-specific types: `osal_impl_queue.h` within each platform directory

---

## Platform-Specific Types (`osal_impl_queue.h`)

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

**FreeRTOS Implementation Notes**:
- Items are **queued by copy**, not by reference
- Prefer queuing pointers to large structures for efficiency
- Queue operations can be performed from both task and ISR contexts

---

## Key Concepts

**ISR Context Differences**:
- ISR functions use `FromISR` suffix
- Platform-specific context switching is handled automatically internally
- No need to manually check or request context switches

---

## Queue Functions

```c
/**
 * @brief Create a message queue
 * @param[out] queue_id    Returned queue ID
 * @param[in]  name        Queue name for debugging (optional, may be NULL)
 * @param[in]  max_items   Maximum number of items queue can hold
 * @param[in]  item_size   Size of each item in bytes
 * @return OSAL status code
 * @retval OSAL_SUCCESS            Queue created successfully
 * @retval OSAL_ERROR              Creation failed
 * @retval OSAL_INVALID_POINTER    queue_id is NULL
 * @retval OSAL_ERR_NAME_TOO_LONG  name exceeds OSAL_MAX_NAME_LEN
 * @retval OSAL_QUEUE_INVALID_SIZE Invalid max_items or item_size
 */
osal_status_t osal_queue_create(osal_queue_id_t *queue_id,
                                const char *name,
                                uint32_t max_items, 
                                uint32_t item_size);

/**
 * @brief Send/post an item to queue (task context)
 * @param[in] queue_id     Queue ID
 * @param[in] item         Pointer to item to copy into queue
 * @param[in] timeout_ms   Timeout in milliseconds
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Item sent successfully
 * @retval OSAL_QUEUE_FULL    Queue is full
 * @retval OSAL_QUEUE_TIMEOUT Timeout expired
 */
osal_status_t osal_queue_send(osal_queue_id_t queue_id, 
                              const void *item, 
                              uint32_t timeout_ms);

/**
 * @brief Receive an item from queue (task context)
 * @param[in]  queue_id    Queue ID
 * @param[out] buffer      Pointer to buffer to copy received item into
 * @param[in]  timeout_ms  Timeout in milliseconds
 * @return OSAL status code
 * @retval OSAL_SUCCESS        Item received successfully
 * @retval OSAL_QUEUE_EMPTY    Queue is empty
 * @retval OSAL_QUEUE_TIMEOUT  Timeout expired
 */
osal_status_t osal_queue_receive(osal_queue_id_t queue_id, 
                                 void *buffer, 
                                 uint32_t timeout_ms);

/**
 * @brief Delete/destroy a queue
 * @param[in] queue_id  Queue ID
 * @return OSAL status code
 */
osal_status_t osal_queue_delete(osal_queue_id_t queue_id);

/**
 * @brief Get number of items currently in queue
 * @param[in] queue_id  Queue ID
 * @return Number of items in queue
 */
uint32_t osal_queue_get_count(osal_queue_id_t queue_id);

/**
 * @brief Send an item to queue from ISR context
 * @param[in] queue_id  Queue ID
 * @param[in] item      Pointer to item to send
 * @return OSAL status code
 * @note No timeout parameter — ISR functions must never block.
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_queue_send_from_isr(osal_queue_id_t queue_id, 
                                       const void *item);

/**
 * @brief Receive an item from queue from ISR context
 * @param[in]  queue_id  Queue ID
 * @param[out] buffer    Pointer to buffer for received item
 * @return OSAL status code
 * @note No timeout parameter — ISR functions must never block.
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_queue_receive_from_isr(osal_queue_id_t queue_id, 
                                          void *buffer);
```

---

## Usage Examples

### Basic Task-to-Task Message Passing

```c
#include "osal_queue.h"
#include "osal_task.h"

typedef struct {
    uint32_t sensor_id;
    float value;
} sensor_data_t;

osal_queue_id_t sensor_queue;

void sensor_task(void *arg) {
    sensor_data_t data;
    
    // Create queue for 10 sensor readings
    osal_queue_create(&sensor_queue, "SensorQ", 10, sizeof(sensor_data_t));
    
    while (1) {
        // Read sensor
        data.sensor_id = 1;
        data.value = read_sensor();
        
        // Send to queue (wait up to 100ms if full)
        if (osal_queue_send(sensor_queue, &data, 100) == OSAL_SUCCESS) {
            // Successfully sent
        }
        
        osal_task_delay_ms(1000);
    }
}

void processing_task(void *arg) {
    sensor_data_t received;
    
    while (1) {
        // Wait for data (infinite timeout)
        if (osal_queue_receive(sensor_queue, &received, OSAL_MAX_DELAY) == OSAL_SUCCESS) {
            // Process received data
            process_sensor_data(received.sensor_id, received.value);
        }
    }
}
```

### Queuing Pointers (Efficient for Large Structures)

```c
typedef struct {
    uint8_t buffer[1024];  // Large data structure
    size_t length;
} large_data_t;

osal_queue_id_t data_ptr_queue;

void producer(void *arg) {
    // Create queue of pointers
    osal_queue_create(&data_ptr_queue, "DataPtrQ", 5, sizeof(large_data_t*));
    
    while (1) {
        // Allocate data
        large_data_t *data = malloc(sizeof(large_data_t));
        if (data != NULL) {
            // Fill data
            fill_data(data);
            
            // Send pointer (not the whole structure)
            osal_queue_send(data_ptr_queue, &data, OSAL_MAX_DELAY);
        }
        
        osal_task_delay_ms(500);
    }
}

void consumer(void *arg) {
    large_data_t *received_ptr;
    
    while (1) {
        // Receive pointer
        if (osal_queue_receive(data_ptr_queue, &received_ptr, OSAL_MAX_DELAY) == OSAL_SUCCESS) {
            // Use data
            use_data(received_ptr);
            
            // Free memory
            free(received_ptr);
        }
    }
}
```

### ISR to Task Communication

```c
osal_queue_id_t event_queue;

typedef struct {
    uint32_t timestamp;
    uint8_t event_type;
} event_t;

void uart_isr_handler(void) {
    event_t event;
    
    // Prepare event data
    event.timestamp = get_timestamp();
    event.event_type = UART_RX_EVENT;
    
    // Send from ISR (platform handles context switching)
    osal_queue_send_from_isr(event_queue, &event);
}

void event_handler_task(void *arg) {
    event_t event;
    
    osal_queue_create(&event_queue, "EventQ", 20, sizeof(event_t));
    
    while (1) {
        if (osal_queue_receive(event_queue, &event, OSAL_MAX_DELAY) == OSAL_SUCCESS) {
            // Handle event
            handle_event(&event);
        }
    }
}
```

### Checking Queue Status

```c
void monitor_task(void *arg) {
    osal_queue_id_t queue;
    osal_queue_create(&queue, "MonitorQ", 10, sizeof(uint32_t));
    
    while (1) {
        uint32_t count = osal_queue_get_count(queue);
        
        if (count > 8) {
            // Queue nearly full - log warning
            osal_log_warning("Queue %d%% full", (count * 100) / 10);
        }
        
        osal_task_delay_ms(1000);
    }
}
```

---

## Best Practices

1. **Choose appropriate queue size** - balance memory usage with throughput needs
2. **Use pointers for large structures** to avoid excessive copying
3. **Set reasonable timeouts** - avoid infinite waits in critical paths
4. **Monitor queue levels** to detect bottlenecks or overflows
5. **Clean up queues** with `osal_queue_delete()` when no longer needed
6. **Check return values** to handle full/empty conditions
7. **Use ISR-safe variants** only in interrupt context
8. **Remember items are copied** - changes to original after send won't affect queued item

---

## Common Pitfalls

### ❌ Wrong: Sending large structure by value
```c
typedef struct {
    uint8_t data[4096];  // 4KB structure
} big_data_t;

big_data_t data;
osal_queue_send(queue, &data, 100);  // Inefficient - copies all 4KB
```

### ✅ Correct: Send pointer instead
```c
big_data_t *data = malloc(sizeof(big_data_t));
osal_queue_send(queue, &data, 100);  // Efficient - copies only pointer
```

### ❌ Wrong: Modifying data after sending
```c
int value = 42;
osal_queue_send(queue, &value, 100);
value = 99;  // This doesn't affect queued value (copied)
```

### ✅ Correct: Understanding copy semantics
```c
int value = 42;
osal_queue_send(queue, &value, 100);  // Queue contains copy of 42
// value can be reused or modified now
```

### ❌ Wrong: Using task function from ISR
```c
void isr_handler(void) {
    osal_queue_send(queue, &data);  // WRONG! Can cause system hang
}
```

### ✅ Correct: Using ISR-safe function
```c
void isr_handler(void) {
    osal_queue_send_from_isr(queue, &data);
}
```

---

## Performance Considerations

**Queue Size**:
- Larger queues: More buffering, but higher memory usage
- Smaller queues: Less memory, but may cause blocking/drops

**Item Size**:
- Small items (≤16 bytes): OK to copy directly
- Large items (>16 bytes): Consider using pointers

**Timeout Values**:
- 0: Non-blocking, returns immediately
- OSAL_MAX_DELAY: Infinite wait, task may block indefinitely
- Finite value: Good for responsive systems with fallback logic

---

## Platform-Specific Notes

### POSIX Implementation
- May use `mqueue` (POSIX message queues) or custom ring buffer
- All `_from_isr()` variants return `OSAL_ERR_NOT_IMPLEMENTED` — POSIX has no ISR context
- Consider using condition variables with mutexes for task-to-task

### FreeRTOS/ESP32 Implementation
- Native FreeRTOS queue implementation
- Full ISR support with automatic context switching
- Platform implementation manages `portYIELD_FROM_ISR()` internally
- Queues are interrupt-safe without additional locking

---

## Argument Validation

All public queue functions validate their arguments:

- **Pointer checks**: Queue IDs and item/buffer pointers validated as non-NULL
- **Size validation**: Item count and size parameters checked for valid ranges
- **Handle validation**: Queue IDs verified as valid handles

**Implementation Requirements**:
```c
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_queue_create(osal_queue_id_t *queue_id,
                                const char *name,
                                uint32_t max_items, 
                                uint32_t item_size) {
    OSAL_CHECK_POINTER(queue_id);
    
    if (name != NULL) {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }
    
    ARGCHECK(max_items > 0, OSAL_QUEUE_INVALID_SIZE);
    ARGCHECK(item_size > 0, OSAL_ERR_INVALID_SIZE);
    // Implementation...
}
```

See [OSAL Assertions](OSAL_Assertions.md) for complete validation guidelines.

---

[← Back to Main Specification](OSAL_SPECIFICATION.md)
