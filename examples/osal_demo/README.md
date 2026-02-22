# OSAL Demo Application

This example demonstrates core OSAL functionality on ESP32:

- **Task Management**: Creating and managing multiple tasks
- **Message Queue**: Inter-task communication  
- **Binary Semaphore**: Task synchronization
- **Software Timer**: Periodic callbacks

## Scenario

Sensor data processing pipeline with:
- **Producer task**: Simulates sensor readings and sends to queue
- **Consumer task**: Receives readings and processes them
- **Monitor task**: Prints statistics
- **Timer**: Periodic status updates
- **Semaphore**: Synchronizes monitoring

## Building

From this directory (`examples/osal_demo`):

```bash
# Set up ESP-IDF environment
C:\projekty\esp-idf\export.ps1

# Clean and build
idf.py fullclean
idf.py build

# Flash to ESP32
idf.py -p COM3 flash monitor
```

Or use the shortcut:
```bash
idf.py fullclean build flash monitor
```

## Configuration

The demo uses OSAL components from `../../src/osal/`. The OSAL component is automatically included via `EXTRA_COMPONENT_DIRS` in the project's CMakeLists.txt.

## Expected Output

The demo will:
1. Create 3 tasks (producer, consumer, monitor)
2. Create a message queue for sensor data
3. Create a binary semaphore for synchronization
4. Start a timer for periodic updates
5. Run indefinitely, printing status messages

You'll see output like:
```
[INFO]: Producer: Sent reading #1 (T=22.3°C, H=58.5%)
[INFO]: Consumer: Processed reading #1
[INFO]: Monitor: Statistics - Produced: 10, Processed: 10
[INFO]: Timer callback at 3000 ms
```

## Notes

- The demo is designed to work on both POSIX and ESP32 platforms
- On ESP32, use `app_main()` as entry point
- On POSIX, use `main()` as entry point
- The same source code compiles for both platforms using conditional compilation
