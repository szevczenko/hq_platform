# OSAL Aggregated Tests for ESP32

This ESP-IDF project runs all OSAL tests in one firmware image.

## Building

From this directory (`tests/osal_tests_esp`):

```bash
# Set up ESP-IDF environment
C:\projekty\esp-idf\export.ps1

# Build
idf.py build

# Flash and monitor
idf.py -p COM4 flash monitor
```

## Tests Included

- Task creation and timing
- Synchronization (mutex/semaphores)
- Queue operations and overflow
- Timer operations (period change/reset)

## Expected Output

The runner prints each suite summary and a final aggregated summary.
