/*
 * HAL POSIX backend tests (TASK-005)
 *
 * Verifies the simulated host/POSIX GPIO and PWM backends
 * (src/hal/posix/hal_gpio_posix.c and src/hal/posix/hal_pwm_posix.c)
 * against the portable contract from hal_gpio.h / hal_pwm.h:
 *
 *   - argument validation (NULL, out-of-range enums, zero frequency),
 *   - pin validation (HAL_PIN_NONE and out-of-range identifiers),
 *   - the init / write / read / deinit lifecycle (not idempotent),
 *   - logical-value round trips with both active polarities,
 *   - writes to input pins are rejected,
 *   - duty-cycle range validation (including NaN and infinities).
 *
 * The backends are deterministic and fully self-contained, so the whole
 * contract can be exercised on the host without any target hardware.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_gpio.h"
#include "hal_pwm.h"
#include "hal_types.h"
#include "unity.h"

/* -------------------------------------------------------------------- */
/* GPIO backend                                                         */
/* -------------------------------------------------------------------- */

static void test_gpio_init_null_config(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_init(NULL));
}

static void test_gpio_init_invalid_pin(void)
{
    hal_gpio_config_t config;

    config.pin = HAL_PIN_NONE;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_init(&config));
}

static void test_gpio_init_invalid_polarity(void)
{
    hal_gpio_config_t config;

    config.pin = 0;
    config.polarity = (hal_polarity_t)99;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_init(&config));
}

static void test_gpio_init_invalid_pull(void)
{
    hal_gpio_config_t config;

    config.pin = 0;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = (hal_gpio_pull_t)99;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_init(&config));
}

static void test_gpio_double_init_rejected(void)
{
    hal_gpio_config_t config;

    config.pin = 0;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_ALREADY_INITIALIZED, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(0));
}

static void test_gpio_ops_on_uninitialized_pin(void)
{
    bool state = false;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_write(0, true));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_read(0, &state));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_deinit(0));
}

static void test_gpio_invalid_pin_ops(void)
{
    bool state = false;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_write(64, true));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_read(64, &state));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_deinit(64));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN,
                          (int)hal_gpio_write(HAL_PIN_NONE, true));
}

static void test_gpio_read_null_output(void)
{
    hal_gpio_config_t config;

    config.pin = 0;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_read(0, NULL));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(0));
}

static void test_gpio_output_roundtrip_active_high(void)
{
    hal_gpio_config_t config;
    bool state = false;

    config.pin = 1;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));

    /* Output pins start in the logical INACTIVE state. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(1, &state));
    TEST_ASSERT_FALSE(state);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(1, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(1, &state));
    TEST_ASSERT_TRUE(state);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(1, false));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(1, &state));
    TEST_ASSERT_FALSE(state);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(1));
}

static void test_gpio_output_roundtrip_active_low(void)
{
    hal_gpio_config_t config;
    bool state = true;

    config.pin = 2;
    config.polarity = HAL_POLARITY_ACTIVE_LOW;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));

    /* Logical INACTIVE is preserved as such regardless of polarity. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(2, &state));
    TEST_ASSERT_FALSE(state);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(2, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(2, &state));
    TEST_ASSERT_TRUE(state);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(2));
}

static void test_gpio_write_to_input_rejected(void)
{
    hal_gpio_config_t config;

    config.pin = 3;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_UP;
    config.output = false;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_SUPPORTED, (int)hal_gpio_write(3, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(3));
}

static void test_gpio_input_read_starts_inactive(void)
{
    hal_gpio_config_t config;
    bool state = true;

    config.pin = 4;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_DOWN;
    config.output = false;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(4, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(4));
}

static void test_gpio_deinit_twice_rejected(void)
{
    hal_gpio_config_t config;

    config.pin = 5;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(5));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_deinit(5));

    /* After deinit the pin may be initialized again. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(5));
}

/* -------------------------------------------------------------------- */
/* PWM backend                                                          */
/* -------------------------------------------------------------------- */

static void test_pwm_init_null_config(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_pwm_init(NULL));
}

static void test_pwm_init_zero_frequency(void)
{
    hal_pwm_config_t config;

    config.pin = 0;
    config.frequency_hz = 0;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_pwm_init(&config));
}

static void test_pwm_init_invalid_polarity(void)
{
    hal_pwm_config_t config;

    config.pin = 0;
    config.frequency_hz = 1000;
    config.polarity = (hal_polarity_t)99;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_pwm_init(&config));
}

static void test_pwm_init_invalid_pin(void)
{
    hal_pwm_config_t config;

    config.pin = HAL_PIN_NONE;
    config.frequency_hz = 1000;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_pwm_init(&config));
}

static void test_pwm_double_init_rejected(void)
{
    hal_pwm_config_t config;

    config.pin = 0;
    config.frequency_hz = 1000;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_ALREADY_INITIALIZED, (int)hal_pwm_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(0));
}

static void test_pwm_ops_on_uninitialized_pin(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_set_duty(0, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_force_inactive(0));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_deinit(0));
}

static void test_pwm_invalid_pin_ops(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_pwm_set_duty(64, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_pwm_force_inactive(64));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_pwm_deinit(64));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN,
                          (int)hal_pwm_set_duty(HAL_PIN_NONE, 50.0f));
}

static void test_pwm_set_duty_range(void)
{
    hal_pwm_config_t config;

    config.pin = 6;
    config.frequency_hz = 1000;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_init(&config));

    /* Closed interval [0.0, 100.0] including both boundaries. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(6, 0.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(6, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(6, 100.0f));

    /* Out-of-range and non-finite values are rejected. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(6, -1.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(6, 100.1f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(6, NAN));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(6, INFINITY));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(6, -INFINITY));

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(6));
}

static void test_pwm_force_inactive_lifecycle(void)
{
    hal_pwm_config_t config;

    config.pin = 7;
    config.frequency_hz = 5000;
    config.polarity = HAL_POLARITY_ACTIVE_LOW;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(7, 25.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_force_inactive(7));
    /* Setting a duty resumes normal generation. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(7, 75.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_force_inactive(7));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(7));
}

static void test_pwm_deinit_twice_rejected(void)
{
    hal_pwm_config_t config;

    config.pin = 8;
    config.frequency_hz = 1000;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(8));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_deinit(8));

    /* After deinit the pin may be initialized again. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_init(&config));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(8));
}

/* -------------------------------------------------------------------- */
/* Unity runner                                                         */
/* -------------------------------------------------------------------- */

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_gpio_init_null_config);
    RUN_TEST(test_gpio_init_invalid_pin);
    RUN_TEST(test_gpio_init_invalid_polarity);
    RUN_TEST(test_gpio_init_invalid_pull);
    RUN_TEST(test_gpio_double_init_rejected);
    RUN_TEST(test_gpio_ops_on_uninitialized_pin);
    RUN_TEST(test_gpio_invalid_pin_ops);
    RUN_TEST(test_gpio_read_null_output);
    RUN_TEST(test_gpio_output_roundtrip_active_high);
    RUN_TEST(test_gpio_output_roundtrip_active_low);
    RUN_TEST(test_gpio_write_to_input_rejected);
    RUN_TEST(test_gpio_input_read_starts_inactive);
    RUN_TEST(test_gpio_deinit_twice_rejected);

    RUN_TEST(test_pwm_init_null_config);
    RUN_TEST(test_pwm_init_zero_frequency);
    RUN_TEST(test_pwm_init_invalid_polarity);
    RUN_TEST(test_pwm_init_invalid_pin);
    RUN_TEST(test_pwm_double_init_rejected);
    RUN_TEST(test_pwm_ops_on_uninitialized_pin);
    RUN_TEST(test_pwm_invalid_pin_ops);
    RUN_TEST(test_pwm_set_duty_range);
    RUN_TEST(test_pwm_force_inactive_lifecycle);
    RUN_TEST(test_pwm_deinit_twice_rejected);

    return UNITY_END();
}