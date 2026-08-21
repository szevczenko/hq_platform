/*
 * OSAL Queue Tests
 *
 * Tests:
 * 1. Queue send/receive between tasks
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "osal_task.h"
#include "osal_queue.h"
#include "unity.h"

/* Task completion & state flags */
static volatile int produced_count = 0;
static volatile int consumed_count = 0;
static volatile bool overflow_detected = false;
static volatile bool producer_done = false;
static volatile bool consumer_done = false;

/**
 * Called at the end of a test task to prevent undefined behavior
 * on ESP32 where tasks must never return. On POSIX it is unnecessary.
 */
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

typedef struct {
    uint32_t value;
} queue_item_t;

#define QUEUE_DEPTH 3
#define TOTAL_ITEMS 5

static osal_queue_id_t queue_id;

static void producer_task_func(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < TOTAL_ITEMS; i++) {
        queue_item_t item = { .value = i + 1 };
        osal_status_t status;

        if (i == QUEUE_DEPTH) {
            /* Force overflow on the 4th item with no wait */
            status = osal_queue_send(queue_id, &item, 0);
            if (status == OSAL_QUEUE_FULL || status == OSAL_QUEUE_TIMEOUT) {
                overflow_detected = true;
            }
            /* Retry with timeout so all items eventually pass */
            status = osal_queue_send(queue_id, &item, 500);
        } else {
            status = osal_queue_send(queue_id, &item, 500);
        }

        if (status == OSAL_SUCCESS) {
            produced_count++;
        }

        osal_task_delay_ms(10);
    }

    producer_done = true;

    osal_test_task_done();
}

static void consumer_task_func(void *arg)
{
    (void)arg;

    /* Delay to allow producer to fill the queue */
    osal_task_delay_ms(200);

    for (uint32_t i = 0; i < TOTAL_ITEMS; i++) {
        queue_item_t item = {0};
        osal_status_t status = osal_queue_receive(queue_id, &item, 1000);

        if (status == OSAL_SUCCESS) {
            consumed_count++;
        }

        osal_task_delay_ms(5);
    }

    consumer_done = true;

    osal_test_task_done();
}

static void test_queue_send_receive(void)
{
    osal_status_t status;
    osal_task_id_t producer_id;
    osal_task_id_t consumer_id;

    produced_count = 0;
    consumed_count = 0;
    overflow_detected = false;
    producer_done = false;
    consumer_done = false;

    status = osal_queue_create(&queue_id, "test_queue", QUEUE_DEPTH, sizeof(queue_item_t));
    TEST_ASSERT_MESSAGE(status == OSAL_SUCCESS, "Queue created successfully");
    TEST_ASSERT_MESSAGE(osal_queue_get_count(queue_id) == 0, "Queue count starts at 0");

    status = osal_task_create(&producer_id, "queue_prod", producer_task_func,
                              NULL, NULL, OSAL_TASK_MIN_STACK_SIZE, 10, NULL);
    TEST_ASSERT_MESSAGE(status == OSAL_SUCCESS, "Producer task created");

    status = osal_task_create(&consumer_id, "queue_cons", consumer_task_func,
                              NULL, NULL, OSAL_TASK_MIN_STACK_SIZE, 10, NULL);
    TEST_ASSERT_MESSAGE(status == OSAL_SUCCESS, "Consumer task created");

    /* Wait for both tasks */
    uint32_t timeout = 3000;
    uint32_t elapsed = 0;
    while (!(producer_done && consumer_done) && elapsed < timeout) {
        osal_task_delay_ms(50);
        elapsed += 50;
    }

    TEST_ASSERT_MESSAGE(producer_done, "Producer task completed");
    TEST_ASSERT_MESSAGE(consumer_done, "Consumer task completed");
    TEST_ASSERT_MESSAGE(overflow_detected, "Queue overflow detected on full queue");
    TEST_ASSERT_MESSAGE(produced_count == TOTAL_ITEMS, "All items produced");
    TEST_ASSERT_MESSAGE(consumed_count == TOTAL_ITEMS, "All items consumed");
    TEST_ASSERT_MESSAGE(osal_queue_get_count(queue_id) == 0, "Queue count returns to 0");

    osal_task_delete(producer_id);
    osal_task_delete(consumer_id);
    status = osal_queue_delete(queue_id);
    TEST_ASSERT_MESSAGE(status == OSAL_SUCCESS, "Queue deleted successfully");
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

void osal_queue_tests_run(void)
{
    RUN_TEST(test_queue_send_receive);
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
    printf("           OSAL Queue Tests                      \n");
    printf("==================================================\n");
    printf("\n");

    osal_queue_tests_run();

#ifndef ESP_PLATFORM
    return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
