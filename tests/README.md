# OSAL Functional Tests

Comprehensive functional tests for the Operating System Abstraction Layer (OSAL) that run on both POSIX and ESP32 platforms.

```bash
# Aggregated tests
cd tests/osal_tests_esp

# Set up ESP-IDF environment
C:\projekty\esp-idf\export.ps1   # Windows
source ~/esp-idf/export.sh        # Linux/macOS

# Build and flash
idf.py build
idf.py -p COM4 flash monitor      # Windows
idf.py -p /dev/ttyUSB0 flash monitor  # Linux
```

# Run tests
./build_posix/tests/osal_task_test
./build_posix/tests/osal_sync_test
./build_posix/tests/osal_queue_test
./build_posix/tests/osal_timer_test

# Run all tests in one process
./build_posix/tests/osal_tests
```

### ESP32 Platform

Tests are built as standalone ESP-IDF projects:

```bash
# Navigate to the ESP32 test directory
cd tests/osal_task_test_esp

# Set up ESP-IDF environment
C:\projekty\esp-idf\export.ps1   # Windows
source ~/esp-idf/export.sh        # Linux/macOS

# Build and flash
idf.py build
idf.py -p COM4 flash monitor      # Windows
idf.py -p /dev/ttyUSB0 flash monitor  # Linux
```

```bash
# Synchronization tests
cd tests/osal_sync_test_esp
idf.py build
idf.py -p COM4 flash monitor
```

## Test Structure

Each test suite follows this structure:

**ESP32 Aggregated Build:**
```
tests/
├── osal/
│   └── osal_task_test.c      # Test source (shared)
└── osal_task_test_esp/       # ESP-IDF project
    ├── CMakeLists.txt
    ├── README.md
    └── main/
        └── CMakeLists.txt

```bash
# Aggregated tests
cd tests/osal_tests_esp
idf.py build
idf.py -p COM4 flash monitor
```
```

**ESP32 Aggregated Build:**
```
tests/
├── tests.c
├── osal/
│   ├── osal_task_test.c
│   ├── osal_sync_test.c
│   ├── osal_queue_test.c
│   └── osal_timer_test.c
└── osal_tests_esp/
  ├── CMakeLists.txt
  ├── README.md
  └── main/
    └── CMakeLists.txt
```

The same test source code in `tests/osal/` is used for both platforms with conditional compilation (`#ifdef ESP_PLATFORM`).

## Expected Output

All tests should pass with output like:

```
==================================================
       OSAL Task Creation and Timing Tests
==================================================

==================================================
TEST: Dynamic Task Creation and Deletion
==================================================
[PASS] Dynamic task created successfully
[PASS] Dynamic task executed and completed
[PASS] Dynamic task completed within timeout
[PASS] Dynamic task deleted successfully
--------------------------------------------------

... (more tests)

==================================================
                  TEST SUMMARY
==================================================
  Total tests:  12
  Passed:       12
  Failed:       0
  Success rate: 100.0%
==================================================

✓ ALL TESTS PASSED!
```

## Adding New Tests

To add a new test suite:

1. **Create test source**: `tests/osal/new_test.c`
   ```c
   #ifdef ESP_PLATFORM
   void app_main(void) { /* test code */ }
   #else
   int main(void) { /* test code */ }
   #endif
   ```

2. **Add to POSIX build**: Update `tests/CMakeLists.txt`
   ```cmake
   add_executable(new_test new_test.c)
   target_link_libraries(new_test hq_osal pthread)
   ```

3. **Create ESP32 project**: `tests/new_test_esp/`
   - `CMakeLists.txt` - Points to `EXTRA_COMPONENT_DIRS`
  - `main/CMakeLists.txt` - References `../osal/new_test.c`

## Planned Test Suites

- [x] Task creation and timing
- [x] Mutex, binary semaphore, counting semaphore
- [x] Queue operations and overflow
- [x] Timer operations, period changes, reset

## Test Coverage

Each test verifies:
- ✅ API return codes (OSAL_SUCCESS, error codes)
- ✅ Functional behavior (tasks execute, data transfers correctly)
- ✅ Timing accuracy (within tolerance)
- ✅ Resource cleanup (no leaks)
- ✅ Edge cases (null pointers, invalid parameters)

## Continuous Integration

Tests can be integrated into CI/CD pipelines:

```bash
# POSIX tests
cmake -B build_test -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON
cmake --build build_test
./build_test/tests/osal_task_test || exit 1

# ESP32 tests (requires hardware or emulator)
cd tests/osal_task_test_esp
idf.py build || exit 1
```

## Troubleshooting

### POSIX Issues

- **pthread errors**: Make sure pthread library is installed
  ```bash
  sudo apt-get install libpthread-stubs0-dev  # Ubuntu/Debian
  ```

- **Build errors**: Ensure tests are enabled
  ```bash
  cmake -B build -DHQ_BUILD_TESTS=ON ...
  ```

### ESP32 Issues

- **Component not found**: Check `EXTRA_COMPONENT_DIRS` points to OSAL
- **Flash errors**: Verify COM port and ESP32 connection
- **Build errors**: Run `idf.py fullclean` and rebuild

## Documentation

For complete OSAL API documentation, see:
- [OSAL_SPECIFICATION.md](../OSAL_SPECIFICATION.md)
- [OSAL_Task_Management.md](../OSAL_Task_Management.md)
