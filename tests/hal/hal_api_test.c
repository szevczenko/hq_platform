/*
 * HAL public API contract tests (TASK-004)
 *
 * Verifies the normative HAL API contract defined by:
 *   - src/hal/include/hal_types.h
 *   - src/hal/include/hal_gpio.h
 *   - src/hal/include/hal_pwm.h
 *
 * Because TASK-004 only defines the public contract (backends arrive in
 * TASK-005..TASK-010), this test validates everything that can be checked
 * without a backend implementation:
 *   1. the headers compile standalone on the host as valid C (they are
 *      self-contained and free of ESP-IDF/POSIX/Linux platform includes),
 *   2. the portable types exist with their documented widths,
 *   3. the documented enum values and constants are preserved,
 *   4. the configuration structures expose the documented fields.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_types.h"
#include "hal_gpio.h"
#include "hal_pwm.h"
#include "unity.h"

/* -------------------------------------------------------------------- */
/* hal_status_t                                                         */
/* -------------------------------------------------------------------- */

static void test_hal_status_success_value(void)
{
    TEST_ASSERT_EQUAL_INT(0, (int)HAL_OK);
}

static void test_hal_status_error_is_negative(void)
{
    /* Every non-OK status must be distinguishable from success. */
    TEST_ASSERT_TRUE((int)HAL_ERROR < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_INVALID_ARGUMENT < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_INVALID_PIN < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_NOT_INITIALIZED < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_ALREADY_INITIALIZED < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_OUT_OF_RANGE < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_NOT_SUPPORTED < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_NO_RESOURCE < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_BUSY < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_PERMISSION < 0);
    TEST_ASSERT_TRUE((int)HAL_ERR_INTERNAL < 0);
}

static void test_hal_status_codes_are_distinct(void)
{
    const hal_status_t codes[] = {
        HAL_OK, HAL_ERROR, HAL_ERR_INVALID_ARGUMENT, HAL_ERR_INVALID_PIN,
        HAL_ERR_NOT_INITIALIZED, HAL_ERR_ALREADY_INITIALIZED,
        HAL_ERR_OUT_OF_RANGE, HAL_ERR_NOT_SUPPORTED, HAL_ERR_NO_RESOURCE,
        HAL_ERR_BUSY, HAL_ERR_PERMISSION, HAL_ERR_INTERNAL,
    };
    const size_t count = sizeof(codes) / sizeof(codes[0]);
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            TEST_ASSERT_NOT_EQUAL_INT((int)codes[i], (int)codes[j]);
        }
    }
}

/* -------------------------------------------------------------------- */
/* hal_pin_t                                                            */
/* -------------------------------------------------------------------- */

static void test_hal_pin_type_width(void)
{
    TEST_ASSERT_EQUAL_UINT32(sizeof(uint32_t), (uint32_t)sizeof(hal_pin_t));
}

static void test_hal_pin_none_sentinel(void)
{
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, (uint32_t)HAL_PIN_NONE);
}

/* -------------------------------------------------------------------- */
/* hal_polarity_t                                                       */
/* -------------------------------------------------------------------- */

static void test_hal_polarity_values(void)
{
    TEST_ASSERT_EQUAL_INT(0, (int)HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_EQUAL_INT(1, (int)HAL_POLARITY_ACTIVE_LOW);
}

/* -------------------------------------------------------------------- */
/* GPIO contract                                                        */
/* -------------------------------------------------------------------- */

static void test_hal_gpio_pull_values(void)
{
    TEST_ASSERT_EQUAL_INT(0, (int)HAL_GPIO_PULL_NONE);
    TEST_ASSERT_EQUAL_INT(1, (int)HAL_GPIO_PULL_UP);
    TEST_ASSERT_EQUAL_INT(2, (int)HAL_GPIO_PULL_DOWN);
}

static void test_hal_gpio_config_fields(void)
{
    hal_gpio_config_t cfg;

    cfg.pin = 7u;
    cfg.polarity = HAL_POLARITY_ACTIVE_LOW;
    cfg.pull = HAL_GPIO_PULL_UP;
    cfg.output = true;

    TEST_ASSERT_EQUAL_UINT32(7u, (uint32_t)cfg.pin);
    TEST_ASSERT_EQUAL_INT((int)HAL_POLARITY_ACTIVE_LOW, (int)cfg.polarity);
    TEST_ASSERT_EQUAL_INT((int)HAL_GPIO_PULL_UP, (int)cfg.pull);
    TEST_ASSERT_TRUE(cfg.output);

    /* Struct must be usable in aggregate zero-initialization contexts. */
    {
        hal_gpio_config_t zeroed = {0};
        TEST_ASSERT_FALSE(zeroed.output);
        TEST_ASSERT_EQUAL_INT((int)HAL_POLARITY_ACTIVE_HIGH,
                              (int)zeroed.polarity);
        TEST_ASSERT_EQUAL_INT((int)HAL_GPIO_PULL_NONE, (int)zeroed.pull);
    }
}

/* -------------------------------------------------------------------- */
/* PWM contract                                                         */
/* -------------------------------------------------------------------- */

static void test_hal_pwm_duty_constants(void)
{
    TEST_ASSERT_EQUAL_FLOAT(HAL_PWM_DUTY_MIN_PERCENT, 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(HAL_PWM_DUTY_MAX_PERCENT, 100.0f);
    TEST_ASSERT_TRUE(HAL_PWM_DUTY_MIN_PERCENT < HAL_PWM_DUTY_MAX_PERCENT);
}

static void test_hal_pwm_config_fields(void)
{
    hal_pwm_config_t cfg;

    cfg.pin = 9u;
    cfg.frequency_hz = 1000u;
    cfg.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_UINT32(9u, (uint32_t)cfg.pin);
    TEST_ASSERT_EQUAL_UINT32(1000u, (uint32_t)cfg.frequency_hz);
    TEST_ASSERT_EQUAL_INT((int)HAL_POLARITY_ACTIVE_HIGH, (int)cfg.polarity);
}

/* -------------------------------------------------------------------- */
/* Runner                                                               */
/* -------------------------------------------------------------------- */

void setUp(void)
{
}

void tearDown(void)
{
}

static void hal_api_tests_run(void)
{
    RUN_TEST(test_hal_status_success_value);
    RUN_TEST(test_hal_status_error_is_negative);
    RUN_TEST(test_hal_status_codes_are_distinct);
    RUN_TEST(test_hal_pin_type_width);
    RUN_TEST(test_hal_pin_none_sentinel);
    RUN_TEST(test_hal_polarity_values);
    RUN_TEST(test_hal_gpio_pull_values);
    RUN_TEST(test_hal_gpio_config_fields);
    RUN_TEST(test_hal_pwm_duty_constants);
    RUN_TEST(test_hal_pwm_config_fields);
}

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    UNITY_BEGIN();
    hal_api_tests_run();
#ifndef ESP_PLATFORM
    return UNITY_END();
#else
    (void)UNITY_END();
    return 0;
#endif
}