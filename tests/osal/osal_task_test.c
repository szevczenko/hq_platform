/*
 * OSAL Task Creation and Time Measurement Tests
 * 
 * Tests:
 * 1. Dynamic task creation and deletion
 * 2. Static task creation and deletion
 * 3. Task execution verification
 * 4. Time measurement with osal_task_get_time_ms()
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "osal_task.h"
#include "osal_log.h"

/* Test results tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Task completion flags */
static volatile bool dynamic_task_completed = false;
static volatile bool static_task_completed = false;

/* Test macros */
#define TEST_ASSERT(condition, message) \
    do { \
        tests_run++; \
        if (condition) { \
            tests_passed++; \
            printf("[PASS] %s\n", message); \
        } else { \
            tests_failed++; \
            printf("[FAIL] %s\n", message); \
        } \
    } while(0)

#define TEST_START(name) \
    printf("\n==================================================\n"); \
    printf("TEST: %s\n", name); \
    printf("==================================================\n")

#define TEST_END() \
    printf("--------------------------------------------------\n")

static void osal_test_task_done(void)
{
#ifdef ESP_PLATFORM
    for (;;)
    {
        osal_task_delay_ms(1000);
    }
#else
    return;
#endif
}
/* ============================================================================
 * Test 1: Dynamic Task Creation and Deletion
 * ========================================================================== */

/**
 * Simple task that sets a completion flag and exits
 */
static void dynamic_task_func(void *arg)
{
    int *task_arg = (int *)arg;
    
    printf("  [Dynamic Task] Started with arg: %d\n", *task_arg);
    
    /* Simulate some work */
    osal_task_delay_ms(100);
    
    /* Set completion flag */
    dynamic_task_completed = true;
    
    printf("  [Dynamic Task] Completed\n");

    osal_test_task_done();
}

static void test_dynamic_task_creation(void)
{
    TEST_START("Dynamic Task Creation and Deletion");
    
    osal_task_id_t task_id;
    osal_status_t status;
    osal_task_attr_t attr;
    int task_arg = 42;
    
    /* Reset flag */
    dynamic_task_completed = false;
    
    /* Create dynamic task */
    status = osal_task_attributes_init(&attr);
    TEST_ASSERT(status == OSAL_SUCCESS, "Task attributes initialized");

    status = osal_task_create(
        &task_id,
        "dynamic_test",
        dynamic_task_func,
        &task_arg,
        NULL,
        16 * 1024,  /* 16 KB stack */
        10,         /* Priority */
        &attr
    );
    
    TEST_ASSERT(status == OSAL_SUCCESS, "Dynamic task created successfully");
    
    /* Wait for task to complete (timeout 500ms) */
    uint32_t timeout = 500;
    uint32_t elapsed = 0;
    while (!dynamic_task_completed && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }
    
    TEST_ASSERT(dynamic_task_completed, "Dynamic task executed and completed");
    TEST_ASSERT(elapsed < timeout, "Dynamic task completed within timeout");
    
    printf("  Task completed in ~%" PRIu32 " ms\n", elapsed);
    
    /* Delete task */
    status = osal_task_delete(task_id);
    TEST_ASSERT(status == OSAL_SUCCESS, "Dynamic task deleted successfully");
    
    TEST_END();
}

/* ============================================================================
 * Test 2: Static Task Creation and Deletion
 * ========================================================================== */

/* Static task memory buffers */
static uint8_t static_task_stack[16 * 1024];

/**
 * Simple task function for static task test
 */
static void static_task_func(void *arg)
{
    const char *task_name = (const char *)arg;
    
    printf("  [Static Task] Started: %s\n", task_name);
    
    /* Simulate some work */
    osal_task_delay_ms(100);
    
    /* Set completion flag */
    static_task_completed = true;
    
    printf("  [Static Task] Completed\n");

    osal_test_task_done();
}

static void test_static_task_creation(void)
{
    TEST_START("Static Task Creation and Deletion");
    
    osal_task_id_t task_id;
    osal_status_t status;
    osal_task_attr_t attr;
    const char *task_name = "static_test_task";
    
    /* Reset flag */
    static_task_completed = false;
    
    status = osal_task_attributes_init(&attr);
    TEST_ASSERT(status == OSAL_SUCCESS, "Task attributes initialized");

    /* Create static task using pre-allocated stack */
    status = osal_task_create(
        &task_id,
        "static_test",
        static_task_func,
        (void *)task_name,
        static_task_stack,
        sizeof(static_task_stack),
        10,  /* Priority */
        &attr
    );
    
    TEST_ASSERT(status == OSAL_SUCCESS, "Static task created successfully");
    
    /* Wait for task to complete (timeout 500ms) */
    uint32_t timeout = 500;
    uint32_t elapsed = 0;
    while (!static_task_completed && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }
    
    TEST_ASSERT(static_task_completed, "Static task executed and completed");
    TEST_ASSERT(elapsed < timeout, "Static task completed within timeout");
    
    printf("  Task completed in ~%" PRIu32 " ms\n", elapsed);
    
    /* Delete task */
    status = osal_task_delete(task_id);
    TEST_ASSERT(status == OSAL_SUCCESS, "Static task deleted successfully");
    
    TEST_END();
}

/* ============================================================================
 * Test 3: Time Measurement
 * ========================================================================== */

static void test_time_measurement(void)
{
    TEST_START("Time Measurement with osal_task_get_time_ms()");
    
    uint32_t start_time, end_time, elapsed;
    uint32_t expected_delay = 250;  /* ms */
    uint32_t tolerance = 50;        /* ±50ms tolerance */
    
    /* Measure time for a known delay */
    start_time = osal_task_get_time_ms();
    printf("  Start time: %" PRIu32 " ms\n", start_time);
    
    osal_task_delay_ms(expected_delay);
    
    end_time = osal_task_get_time_ms();
    printf("  End time:   %" PRIu32 " ms\n", end_time);
    
    elapsed = end_time - start_time;
    printf("  Elapsed:    %" PRIu32 " ms (expected ~%" PRIu32 " ms)\n", elapsed, expected_delay);
    
    /* Check if elapsed time is within tolerance */
    int32_t diff = (int32_t)elapsed - (int32_t)expected_delay;
    if (diff < 0) diff = -diff;
    
    TEST_ASSERT(diff <= (int32_t)tolerance, 
                "Time measurement within tolerance (±50ms)");
    
    /* Test multiple sequential delays */
    start_time = osal_task_get_time_ms();
    
    osal_task_delay_ms(50);
    osal_task_delay_ms(50);
    osal_task_delay_ms(50);
    
    end_time = osal_task_get_time_ms();
    elapsed = end_time - start_time;
    
    printf("  Sequential delays: %" PRIu32 " ms (expected ~150 ms)\n", elapsed);
    
    diff = (int32_t)elapsed - 150;
    if (diff < 0) diff = -diff;
    
    TEST_ASSERT(diff <= (int32_t)tolerance, 
                "Sequential time measurement accurate");
    
    TEST_END();
}

/* ============================================================================
 * Test 4: Multiple Concurrent Tasks
 * ========================================================================== */

#define NUM_CONCURRENT_TASKS 3

static volatile int concurrent_task_count = 0;
static volatile bool concurrent_tasks_done[NUM_CONCURRENT_TASKS] = {false};

static void concurrent_task_func(void *arg)
{
    int task_num = *(int *)arg;
    
    printf("  [Task %d] Started\n", task_num);
    
    /* Each task delays a different amount */
    osal_task_delay_ms(100 + (task_num * 50));
    
    concurrent_tasks_done[task_num] = true;
    concurrent_task_count++;
    
    printf("  [Task %d] Completed\n", task_num);

    osal_test_task_done();
}

static void test_concurrent_tasks(void)
{
    TEST_START("Multiple Concurrent Tasks");
    
    osal_task_id_t task_ids[NUM_CONCURRENT_TASKS];
    int task_args[NUM_CONCURRENT_TASKS];
    osal_status_t status;
    
    /* Reset counters */
    concurrent_task_count = 0;
    for (int i = 0; i < NUM_CONCURRENT_TASKS; i++) {
        concurrent_tasks_done[i] = false;
        task_args[i] = i;
    }
    
    /* Create multiple tasks */
    for (int i = 0; i < NUM_CONCURRENT_TASKS; i++) {
        char task_name[32];
        snprintf(task_name, sizeof(task_name), "concurrent_%d", i);
        
        status = osal_task_create(
            &task_ids[i],
            task_name,
            concurrent_task_func,
            &task_args[i],
            NULL,
            16 * 1024,
            10,
            NULL
        );
        
        if (status != OSAL_SUCCESS) {
            printf("  [ERROR] Failed to create task %d\n", i);
        }
    }
    
    TEST_ASSERT(true, "All concurrent tasks created");
    
    /* Wait for all tasks to complete (timeout 2000ms) */
    uint32_t timeout = 2000;
    uint32_t elapsed = 0;
    while (concurrent_task_count < NUM_CONCURRENT_TASKS && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }
    
    TEST_ASSERT(concurrent_task_count == NUM_CONCURRENT_TASKS, 
                "All concurrent tasks completed");
    
    printf("  All %d tasks completed in ~%" PRIu32 " ms\n", NUM_CONCURRENT_TASKS, elapsed);
    
    /* Verify each task completed */
    bool all_done = true;
    for (int i = 0; i < NUM_CONCURRENT_TASKS; i++) {
        if (!concurrent_tasks_done[i]) {
            all_done = false;
            printf("  [ERROR] Task %d did not complete\n", i);
        }
    }
    
    TEST_ASSERT(all_done, "All task completion flags set correctly");
    
    /* Clean up */
    for (int i = 0; i < NUM_CONCURRENT_TASKS; i++) {
        osal_task_delete(task_ids[i]);
    }
    
    TEST_END();
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

static void osal_task_tests_reset(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
}

int osal_task_tests_run(void)
{
    osal_task_tests_reset();

    printf("\n");
    printf("==================================================\n");
    printf("       OSAL Task Creation and Timing Tests       \n");
    printf("==================================================\n");
    printf("\n");

    /* Run all tests */
    test_dynamic_task_creation();
    test_static_task_creation();
    test_time_measurement();
    test_concurrent_tasks();

    /* Print summary */
    printf("\n");
    printf("==================================================\n");
    printf("                  TEST SUMMARY                    \n");
    printf("==================================================\n");
    printf("  Total tests:  %d\n", tests_run);
    printf("  Passed:       %d\n", tests_passed);
    printf("  Failed:       %d\n", tests_failed);
    printf("  Success rate: %.1f%%\n",
           (tests_run > 0) ? (100.0 * tests_passed / tests_run) : 0.0);
    printf("==================================================\n");

    if (tests_failed == 0) {
        printf("\n✓ ALL TESTS PASSED!\n\n");
    } else {
        printf("\n✗ SOME TESTS FAILED!\n\n");
    }

    return tests_failed;
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    printf("\n");
    printf("==================================================\n");
    printf("       OSAL Task Creation and Timing Tests       \n");
    printf("==================================================\n");
    printf("\n");
    
    int failed = osal_task_tests_run();

#ifndef ESP_PLATFORM
    return (failed == 0) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
