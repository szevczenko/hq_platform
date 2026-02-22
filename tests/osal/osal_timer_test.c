/*
 * OSAL Timer Tests
 *
 * Tests:
 * 1. One-shot timer expiry timing and context
 * 2. Auto-reload timer period change and reset
 * 3. Timer start/stop state
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "osal_task.h"
#include "osal_timer.h"
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

typedef struct {
    uint32_t expected_ms;
    uint32_t tolerance_ms;
    volatile bool fired;
    volatile bool context_ok;
    volatile uint32_t start_ms;
    volatile uint32_t elapsed_ms;
} timer_context_t;

static volatile uint32_t auto_callback_count = 0;
static volatile uint32_t auto_last_ms = 0;
static volatile uint32_t auto_prev_ms = 0;

static void oneshot_timer_callback(osal_timer_id_t timer_id)
{
    timer_context_t *ctx = (timer_context_t *)osal_timer_get_context(timer_id);
    uint32_t now = osal_task_get_time_ms();

    if (ctx) {
        ctx->elapsed_ms = now - ctx->start_ms;
        ctx->context_ok = true;
        ctx->fired = true;
    }
}

static void auto_timer_callback(osal_timer_id_t timer_id)
{
    timer_context_t *ctx = (timer_context_t *)osal_timer_get_context(timer_id);
    uint32_t now = osal_task_get_time_ms();

    auto_prev_ms = auto_last_ms;
    auto_last_ms = now;
    auto_callback_count++;

    if (ctx) {
        ctx->context_ok = true;
    }
}

static void test_oneshot_timer(void)
{
    TEST_START("One-shot Timer Timing and Context");

    osal_timer_id_t timer_id;
    osal_status_t status;
    timer_context_t ctx = {0};

    ctx.expected_ms = 200;
    ctx.tolerance_ms = 60;

    status = osal_timer_create(&timer_id, "oneshot", ctx.expected_ms, false,
                               oneshot_timer_callback, NULL, NULL, 0);
    TEST_ASSERT(status == OSAL_SUCCESS, "One-shot timer created");

    status = osal_timer_set_context(timer_id, &ctx);
    TEST_ASSERT(status == OSAL_SUCCESS, "Timer context set");

    ctx.start_ms = osal_task_get_time_ms();
    status = osal_timer_start(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "One-shot timer started");
    TEST_ASSERT(osal_timer_is_active(timer_id), "Timer is active after start");

    /* Wait for callback */
    uint32_t timeout = 1000;
    uint32_t elapsed = 0;
    while (!ctx.fired && elapsed < timeout) {
        osal_task_delay_ms(10);
        elapsed += 10;
    }

    TEST_ASSERT(ctx.fired, "One-shot timer callback fired");
    TEST_ASSERT(ctx.context_ok, "Timer context available in callback");

    int32_t diff = (int32_t)ctx.elapsed_ms - (int32_t)ctx.expected_ms;
    if (diff < 0) diff = -diff;
    TEST_ASSERT(diff <= (int32_t)ctx.tolerance_ms,
                "One-shot timer timing within tolerance");

    status = osal_timer_stop(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "One-shot timer stopped");
    TEST_ASSERT(!osal_timer_is_active(timer_id), "Timer inactive after stop");

    status = osal_timer_delete(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "One-shot timer deleted");

    TEST_END();
}

static void test_auto_timer_change_reset(void)
{
    TEST_START("Auto-reload Timer Change Period and Reset");

    osal_timer_id_t timer_id;
    osal_status_t status;
    timer_context_t ctx = {0};

    ctx.expected_ms = 100;
    ctx.tolerance_ms = 60;

    auto_callback_count = 0;
    auto_last_ms = 0;
    auto_prev_ms = 0;

    status = osal_timer_create(&timer_id, "auto", ctx.expected_ms, true,
                               auto_timer_callback, NULL, NULL, 0);
    TEST_ASSERT(status == OSAL_SUCCESS, "Auto-reload timer created");

    status = osal_timer_set_context(timer_id, &ctx);
    TEST_ASSERT(status == OSAL_SUCCESS, "Auto timer context set");

    status = osal_timer_start(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "Auto timer started");

    /* Wait for at least 3 callbacks */
    uint32_t timeout = 2000;
    uint32_t elapsed = 0;
    while (auto_callback_count < 3 && elapsed < timeout) {
        osal_task_delay_ms(20);
        elapsed += 20;
    }

    TEST_ASSERT(auto_callback_count >= 3, "Auto timer produced callbacks");
    TEST_ASSERT(ctx.context_ok, "Auto timer context available in callback");

    if (auto_prev_ms > 0 && auto_last_ms > auto_prev_ms) {
        uint32_t period = auto_last_ms - auto_prev_ms;
        int32_t diff = (int32_t)period - (int32_t)ctx.expected_ms;
        if (diff < 0) diff = -diff;
        TEST_ASSERT(diff <= (int32_t)ctx.tolerance_ms,
                    "Auto timer period within tolerance");
    } else {
        TEST_ASSERT(false, "Auto timer period measurement available");
    }

    /* Change period to 200ms */
    ctx.expected_ms = 200;
    ctx.tolerance_ms = 80;
    uint32_t change_start = osal_task_get_time_ms();
    status = osal_timer_change_period(timer_id, ctx.expected_ms, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "Timer period changed");

    /* Wait for one callback after change */
    uint32_t target_count = auto_callback_count + 1;
    elapsed = 0;
    while (auto_callback_count < target_count && elapsed < timeout) {
        osal_task_delay_ms(20);
        elapsed += 20;
    }

    TEST_ASSERT(auto_callback_count >= target_count, "Callback after period change");
    if (auto_last_ms > change_start) {
        uint32_t period = auto_last_ms - change_start;
        int32_t diff = (int32_t)period - (int32_t)ctx.expected_ms;
        if (diff < 0) diff = -diff;
        TEST_ASSERT(diff <= (int32_t)ctx.tolerance_ms,
                    "Changed period within tolerance");
    } else {
        TEST_ASSERT(false, "Changed period measurement available");
    }

    /* Reset timer and verify next expiry relative to reset time */
    uint32_t reset_start = osal_task_get_time_ms();
    status = osal_timer_reset(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "Timer reset");

    target_count = auto_callback_count + 1;
    elapsed = 0;
    while (auto_callback_count < target_count && elapsed < timeout) {
        osal_task_delay_ms(20);
        elapsed += 20;
    }

    TEST_ASSERT(auto_callback_count >= target_count, "Callback after reset");
    if (auto_last_ms > reset_start) {
        uint32_t period = auto_last_ms - reset_start;
        int32_t diff = (int32_t)period - (int32_t)ctx.expected_ms;
        if (diff < 0) diff = -diff;
        TEST_ASSERT(diff <= (int32_t)ctx.tolerance_ms,
                    "Reset period within tolerance");
    } else {
        TEST_ASSERT(false, "Reset period measurement available");
    }

    status = osal_timer_stop(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "Auto timer stopped");

    status = osal_timer_delete(timer_id, 1000);
    TEST_ASSERT(status == OSAL_SUCCESS, "Auto timer deleted");

    TEST_END();
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

static void osal_timer_tests_reset(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
}

int osal_timer_tests_run(void)
{
    osal_timer_tests_reset();

    printf("\n");
    printf("==================================================\n");
    printf("           OSAL Timer Tests (Test 4)             \n");
    printf("==================================================\n");
    printf("\n");

    test_oneshot_timer();
    test_auto_timer_change_reset();

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
    int failed = osal_timer_tests_run();

#ifndef ESP_PLATFORM
    return (failed == 0) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
