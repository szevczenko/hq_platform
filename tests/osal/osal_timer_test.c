/*
 * OSAL Timer Tests
 *
 * Tests:
 * 1. One-shot timer expiry timing and context
 * 2. Auto-reload timer period change and reset
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "osal_task.h"
#include "osal_timer.h"
#include "osal_log.h"
#include "unity.h"


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
    osal_timer_id_t timer_id;
    osal_status_t status;
    timer_context_t ctx = {0};

    ctx.expected_ms = 200;
    ctx.tolerance_ms = 60;

    status = osal_timer_create(&timer_id, "oneshot", ctx.expected_ms, false,
                               oneshot_timer_callback, NULL, NULL, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "One-shot timer created");

    status = osal_timer_set_context(timer_id, &ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Timer context set");

    ctx.start_ms = osal_task_get_time_ms();
    status = osal_timer_start(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "One-shot timer started");
    TEST_ASSERT_TRUE_MESSAGE(osal_timer_is_active(timer_id), "Timer is active after start");

    /* Wait for callback */
    uint32_t timeout = 1000;
    uint32_t elapsed = 0;
    while (!ctx.fired && elapsed < timeout) {
        osal_task_delay_ms(10);
        elapsed += 10;
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.fired, "One-shot timer callback fired");
    TEST_ASSERT_TRUE_MESSAGE(ctx.context_ok, "Timer context available in callback");

    int32_t diff = (int32_t)ctx.elapsed_ms - (int32_t)ctx.expected_ms;
    if (diff < 0) diff = -diff;
    TEST_ASSERT_TRUE_MESSAGE(diff <= (int32_t)ctx.tolerance_ms, "One-shot timer timing within tolerance");

    status = osal_timer_stop(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "One-shot timer stopped");
    TEST_ASSERT_FALSE_MESSAGE(osal_timer_is_active(timer_id), "Timer inactive after stop");

    status = osal_timer_delete(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "One-shot timer deleted");
}

static void test_auto_timer_change_reset(void)
{
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Auto-reload timer created");

    status = osal_timer_set_context(timer_id, &ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Auto timer context set");

    status = osal_timer_start(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Auto timer started");

    /* Wait for at least 3 callbacks */
    uint32_t timeout = 2000;
    uint32_t elapsed = 0;
    while (auto_callback_count < 3 && elapsed < timeout) {
        osal_task_delay_ms(20);
        elapsed += 20;
    }

    TEST_ASSERT_TRUE_MESSAGE(auto_callback_count >= 3, "Auto timer produced callbacks");
    TEST_ASSERT_TRUE_MESSAGE(ctx.context_ok, "Auto timer context available in callback");

    if (auto_prev_ms > 0 && auto_last_ms > auto_prev_ms) {
        uint32_t period = auto_last_ms - auto_prev_ms;
        int32_t diff = (int32_t)period - (int32_t)ctx.expected_ms;
        if (diff < 0) diff = -diff;
        TEST_ASSERT_TRUE_MESSAGE(diff <= (int32_t)ctx.tolerance_ms, "Auto timer period within tolerance");
    } else {
        TEST_ASSERT_MESSAGE(false, "Auto timer period measurement available");
    }

    /* Change period to 200ms */
    ctx.expected_ms = 200;
    ctx.tolerance_ms = 80;
    uint32_t change_start = osal_task_get_time_ms();
    status = osal_timer_change_period(timer_id, ctx.expected_ms, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Timer period changed");

    /* Wait for one callback after change */
    uint32_t target_count = auto_callback_count + 1;
    elapsed = 0;
    while (auto_callback_count < target_count && elapsed < timeout) {
        osal_task_delay_ms(20);
        elapsed += 20;
    }

    TEST_ASSERT_TRUE_MESSAGE(auto_callback_count >= target_count, "Callback after period change");
    if (auto_last_ms > change_start) {
        uint32_t period = auto_last_ms - change_start;
        int32_t diff = (int32_t)period - (int32_t)ctx.expected_ms;
        if (diff < 0) diff = -diff;
        TEST_ASSERT_TRUE_MESSAGE(diff <= (int32_t)ctx.tolerance_ms, "Changed period within tolerance");
    } else {
        TEST_ASSERT_MESSAGE(false, "Changed period measurement available");
    }

    /* Reset timer and verify next expiry relative to reset time */
    uint32_t reset_start = osal_task_get_time_ms();
    status = osal_timer_reset(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Timer reset");

    target_count = auto_callback_count + 1;
    elapsed = 0;
    while (auto_callback_count < target_count && elapsed < timeout) {
        osal_task_delay_ms(20);
        elapsed += 20;
    }

    TEST_ASSERT_TRUE_MESSAGE(auto_callback_count >= target_count, "Callback after reset");
    if (auto_last_ms > reset_start) {
        uint32_t period = auto_last_ms - reset_start;
        int32_t diff = (int32_t)period - (int32_t)ctx.expected_ms;
        if (diff < 0) diff = -diff;
        TEST_ASSERT_TRUE_MESSAGE(diff <= (int32_t)ctx.tolerance_ms, "Reset period within tolerance");
    } else {
        TEST_ASSERT_MESSAGE(false, "Reset period measurement available");
    }

    status = osal_timer_stop(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Auto timer stopped");

    status = osal_timer_delete(timer_id, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OSAL_SUCCESS, status, "Auto timer deleted");
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

void osal_timer_tests_run(void)
{
    RUN_TEST(test_oneshot_timer);
    RUN_TEST(test_auto_timer_change_reset);
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
    printf("           OSAL Timer Tests                       \n");
    printf("==================================================\n");
    printf("\n");

    osal_timer_tests_run();

#ifndef ESP_PLATFORM
    return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
