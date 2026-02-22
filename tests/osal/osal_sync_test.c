/*
 * OSAL Synchronization Tests
 *
 * Tests:
 * 1. Mutex protection with two tasks
 * 2. Binary semaphore task synchronization
 * 3. Counting semaphore producer/consumer
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "osal_task.h"
#include "osal_mutex.h"
#include "osal_bin_sem.h"
#include "osal_count_sem.h"
#include "osal_log.h"

/* Test results tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

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
 * Test 1: Mutex Protection
 * ========================================================================== */

static osal_mutex_id_t mutex_id;
static volatile int shared_counter = 0;
static volatile bool mutex_task_done[2] = {false, false};

static void mutex_task_func(void *arg)
{
    int task_num = *(int *)arg;

    for (int i = 0; i < 100; i++) {
        osal_mutex_take(mutex_id);
        shared_counter++;
        osal_mutex_give(mutex_id);
        osal_task_delay_ms(1);
    }

    mutex_task_done[task_num] = true;

    osal_test_task_done();
}

static void test_mutex_protection(void)
{
    TEST_START("Mutex Protection with Two Tasks");

    osal_status_t status;
    osal_task_id_t task_ids[2];
    int task_args[2] = {0, 1};

    shared_counter = 0;
    mutex_task_done[0] = false;
    mutex_task_done[1] = false;

    status = osal_mutex_create(&mutex_id, "test_mutex");
    TEST_ASSERT(status == OSAL_SUCCESS, "Mutex created successfully");

    status = osal_task_create(&task_ids[0], "mutex_task_0", mutex_task_func,
                              &task_args[0], NULL, 16 * 1024, 10, NULL);
    TEST_ASSERT(status == OSAL_SUCCESS, "Mutex task 0 created");

    status = osal_task_create(&task_ids[1], "mutex_task_1", mutex_task_func,
                              &task_args[1], NULL, 16 * 1024, 10, NULL);
    TEST_ASSERT(status == OSAL_SUCCESS, "Mutex task 1 created");

    /* Wait for both tasks */
    uint32_t timeout = 2000;
    uint32_t elapsed = 0;
    while (!(mutex_task_done[0] && mutex_task_done[1]) && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }

    TEST_ASSERT(mutex_task_done[0] && mutex_task_done[1],
                "Both mutex tasks completed");
    TEST_ASSERT(shared_counter == 200, "Shared counter is correct (200)");

    osal_task_delete(task_ids[0]);
    osal_task_delete(task_ids[1]);
    status = osal_mutex_delete(mutex_id);
    TEST_ASSERT(status == OSAL_SUCCESS, "Mutex deleted successfully");

    TEST_END();
}

/* ============================================================================
 * Test 2: Binary Semaphore Synchronization
 * ========================================================================== */

static osal_bin_sem_id_t bin_sem_id;
static volatile bool bin_waiter_done = false;
static volatile bool bin_signaler_done = false;

static void bin_waiter_task(void *arg)
{
    (void)arg;

    osal_status_t status = osal_bin_sem_timed_wait(bin_sem_id, 1000);
    if (status == OSAL_SUCCESS) {
        bin_waiter_done = true;
    }

    osal_test_task_done();
}

static void bin_signaler_task(void *arg)
{
    (void)arg;

    osal_task_delay_ms(100);
    osal_bin_sem_give(bin_sem_id);
    bin_signaler_done = true;

    osal_test_task_done();
}

static void test_binary_semaphore(void)
{
    TEST_START("Binary Semaphore Synchronization");

    osal_status_t status;
    osal_task_id_t waiter_id;
    osal_task_id_t signaler_id;

    bin_waiter_done = false;
    bin_signaler_done = false;

    status = osal_bin_sem_create(&bin_sem_id, "bin_sem", OSAL_SEM_EMPTY);
    TEST_ASSERT(status == OSAL_SUCCESS, "Binary semaphore created");

    status = osal_task_create(&waiter_id, "bin_waiter", bin_waiter_task,
                              NULL, NULL, 16 * 1024, 10, NULL);
    TEST_ASSERT(status == OSAL_SUCCESS, "Waiter task created");

    status = osal_task_create(&signaler_id, "bin_signaler", bin_signaler_task,
                              NULL, NULL, 16 * 1024, 10, NULL);
    TEST_ASSERT(status == OSAL_SUCCESS, "Signaler task created");

    /* Wait for both tasks */
    uint32_t timeout = 2000;
    uint32_t elapsed = 0;
    while (!(bin_waiter_done && bin_signaler_done) && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }

    TEST_ASSERT(bin_signaler_done, "Signaler task completed");
    TEST_ASSERT(bin_waiter_done, "Waiter task acquired semaphore");

    /* Verify timeout behavior */
    status = osal_bin_sem_timed_wait(bin_sem_id, 0);
    TEST_ASSERT(status == OSAL_SEM_TIMEOUT, "Binary semaphore timeout works (non-blocking)");

    osal_task_delete(waiter_id);
    osal_task_delete(signaler_id);
    status = osal_bin_sem_delete(bin_sem_id);
    TEST_ASSERT(status == OSAL_SUCCESS, "Binary semaphore deleted");

    TEST_END();
}

/* ============================================================================
 * Test 3: Counting Semaphore Producer/Consumer
 * ========================================================================== */

static osal_count_sem_id_t count_sem_id;
static volatile bool count_producer_done = false;
static volatile bool count_consumer_done = false;

static void count_producer_task(void *arg)
{
    (void)arg;

    /* Give semaphore 3 times */
    osal_count_sem_give(count_sem_id);
    osal_task_delay_ms(50);
    osal_count_sem_give(count_sem_id);
    osal_task_delay_ms(50);
    osal_count_sem_give(count_sem_id);

    count_producer_done = true;

    osal_test_task_done();
}

static void count_consumer_task(void *arg)
{
    (void)arg;

    /* Take semaphore 3 times */
    osal_count_sem_timed_wait(count_sem_id, 1000);
    osal_count_sem_timed_wait(count_sem_id, 1000);
    osal_count_sem_timed_wait(count_sem_id, 1000);

    count_consumer_done = true;

    osal_test_task_done();
}

static void test_counting_semaphore(void)
{
    TEST_START("Counting Semaphore Producer/Consumer");

    osal_status_t status;
    osal_task_id_t producer_id;
    osal_task_id_t consumer_id;

    count_producer_done = false;
    count_consumer_done = false;

    status = osal_count_sem_create(&count_sem_id, "count_sem", 0, 3);
    TEST_ASSERT(status == OSAL_SUCCESS, "Counting semaphore created");
    TEST_ASSERT(osal_count_sem_get_count(count_sem_id) == 0,
                "Initial count is 0");

    status = osal_task_create(&producer_id, "count_prod", count_producer_task,
                              NULL, NULL, 16 * 1024, 10, NULL);
    TEST_ASSERT(status == OSAL_SUCCESS, "Producer task created");

    status = osal_task_create(&consumer_id, "count_cons", count_consumer_task,
                              NULL, NULL, 16 * 1024, 10, NULL);
    TEST_ASSERT(status == OSAL_SUCCESS, "Consumer task created");

    /* Wait for both tasks */
    uint32_t timeout = 2000;
    uint32_t elapsed = 0;
    while (!(count_producer_done && count_consumer_done) && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }

    TEST_ASSERT(count_producer_done, "Producer task completed");
    TEST_ASSERT(count_consumer_done, "Consumer task completed");
    TEST_ASSERT(osal_count_sem_get_count(count_sem_id) == 0,
                "Final count is 0");

    /* Verify timeout on empty semaphore */
    status = osal_count_sem_timed_wait(count_sem_id, 0);
    TEST_ASSERT(status == OSAL_SEM_TIMEOUT, "Counting semaphore timeout works (non-blocking)");

    osal_task_delete(producer_id);
    osal_task_delete(consumer_id);
    status = osal_count_sem_delete(count_sem_id);
    TEST_ASSERT(status == OSAL_SUCCESS, "Counting semaphore deleted");

    TEST_END();
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

static void osal_sync_tests_reset(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
}

int osal_sync_tests_run(void)
{
    osal_sync_tests_reset();

    printf("\n");
    printf("==================================================\n");
    printf("     OSAL Synchronization Tests (Sem/Mutex)      \n");
    printf("==================================================\n");
    printf("\n");

    test_mutex_protection();
    test_binary_semaphore();
    test_counting_semaphore();

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
    printf("     OSAL Synchronization Tests (Sem/Mutex)      \n");
    printf("==================================================\n");
    printf("\n");

    int failed = osal_sync_tests_run();

#ifndef ESP_PLATFORM
    return (failed == 0) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
