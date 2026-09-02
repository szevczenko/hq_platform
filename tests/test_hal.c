/*
 * HAL API contract host unit tests (TASK-007)
 *
 * Validates the portable HAL contract against the real POSIX GPIO/PWM
 * backends (src/hal/posix).  These tests run entirely on the host: the POSIX
 * backends are deterministic, in-memory simulations, so no ESP-IDF
 * installation and no physical ESP hardware is required.
 *
 * The tests cover the full lifecycle and the normative polarity behavior:
 *
 *   GPIO:
 *     - initialization (and the init / write / read / deinit lifecycle),
 *     - active-high and active-low configuration,
 *     - pull configuration (none / up / down),
 *     - logical ACTIVE and logical INACTIVE writes,
 *     - logical reads,
 *     - raw electrical state for both polarities,
 *     - invalid pin / configuration,
 *     - operations before initialization,
 *     - deinitialization,
 *     - operations after deinitialization.
 *
 *   PWM:
 *     - initialization,
 *     - 0% / 50% / 100% duty cycles,
 *     - duty values above 100% (rejected),
 *     - negative duty values (the float representation permits them, but the
 *       contract rejects them with HAL_ERR_OUT_OF_RANGE),
 *     - configured frequency,
 *     - force-inactive,
 *     - deinitialization,
 *     - operations before and after initialization.
 *
 * Expected polarity behavior (normative, from hal_types.h / hal_gpio.h):
 *
 *   ACTIVE_HIGH:
 *       ACTIVE   == raw HIGH
 *       INACTIVE == raw LOW
 *
 *   ACTIVE_LOW:
 *       ACTIVE   == raw LOW
 *       INACTIVE == raw HIGH
 *
 * The tests below assert this truth table directly through the public API
 * (logical reads) and through the host/test-only inspection helpers in
 * hal_posix_inspect.h, which expose backend-internal state that the public
 * API cannot:
 *
 *   - hal_posix_gpio_get_raw_state()  -> raw electrical level after the
 *     polarity conversion,
 *   - hal_posix_gpio_get_pull()       -> the pull configuration retained by
 *     the backend (complementing the observable input/raw level checks in
 *     the pull tests),
 *   - hal_posix_pwm_get_duty()        -> the stored duty cycle, including
 *     the value retained across hal_pwm_force_inactive(),
 *   - hal_posix_pwm_get_frequency()   -> the frequency retained by the
 *     backend from hal_pwm_init(),
 *   - hal_posix_pwm_get_output_state() -> the driven raw output level and
 *     whether generation is running (false while forced inactive).
 *
 * The test uses the repository's established Unity host test framework and
 * links the static host library `hq_hal` (which compiles the POSIX backend
 * sources), exactly like the existing tests/hal tests.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_gpio.h"
#include "hal_pwm.h"
#include "hal_types.h"
#include "hal_posix_inspect.h"
#include "unity.h"

/* Valid pin identifiers used by these tests.  They must stay within the
 * backend's simulated range and must not collide with pins used by the other
 * HAL test executables (each executable is a separate process, so the
 * backend state is per-process and the specific numbers are free). */
#define TEST_GPIO_PIN_ACTIVE_HIGH 10U
#define TEST_GPIO_PIN_ACTIVE_LOW  11U
#define TEST_GPIO_PIN_INPUT       12U
#define TEST_PWM_PIN              13U

#define TEST_RAW_LOW  0
#define TEST_RAW_HIGH 1

/* -------------------------------------------------------------------- */
/* Small helpers                                                        */
/* -------------------------------------------------------------------- */

static hal_status_t init_gpio_output(hal_pin_t pin, hal_polarity_t polarity,
                                     hal_gpio_pull_t pull)
{
    hal_gpio_config_t config;

    config.pin = pin;
    config.polarity = polarity;
    config.pull = pull;
    config.output = true;
    return hal_gpio_init(&config);
}

static hal_status_t init_gpio_input(hal_pin_t pin, hal_polarity_t polarity,
                                    hal_gpio_pull_t pull)
{
    hal_gpio_config_t config;

    config.pin = pin;
    config.polarity = polarity;
    config.pull = pull;
    config.output = false;
    return hal_gpio_init(&config);
}

static hal_status_t read_raw(hal_pin_t pin, int *raw)
{
    return hal_posix_gpio_get_raw_state(pin, raw);
}

/* Assert the full polarity truth table for one polarity on an output pin:
 *
 *   - logical ACTIVE  write -> logical read ACTIVE  and the documented raw level
 *   - logical INACTIVE write -> logical read INACTIVE and the documented raw level
 *
 * raw_active / raw_inactive are the expected raw levels (0/1) for that
 * polarity as documented above.
 */
static void assert_polarity_truth_table(hal_pin_t pin, hal_polarity_t polarity,
                                        int raw_active, int raw_inactive)
{
    bool state = false;
    int raw = -1;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(pin, polarity, HAL_GPIO_PULL_NONE));

    /* Output pins start in the logical INACTIVE state. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(pin, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(pin, &raw));
    TEST_ASSERT_EQUAL_INT(raw_inactive, raw);

    /* Logical ACTIVE write. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(pin, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(pin, &state));
    TEST_ASSERT_TRUE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(pin, &raw));
    TEST_ASSERT_EQUAL_INT(raw_active, raw);

    /* Logical INACTIVE write. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(pin, false));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(pin, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(pin, &raw));
    TEST_ASSERT_EQUAL_INT(raw_inactive, raw);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(pin));
}

/* -------------------------------------------------------------------- */
/* GPIO tests                                                           */
/* -------------------------------------------------------------------- */

/* 1. Initialization. */
static void test_gpio_initialization(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 2. Active-high configuration. */
static void test_gpio_active_high_configuration(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 3. Active-low configuration. */
static void test_gpio_active_low_configuration(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_LOW,
                                                        HAL_POLARITY_ACTIVE_LOW,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_LOW));
}

/* 4..6. Pull configuration (none / up / down) on an active-high input pin.
 *
 * Each variant must (a) be retained by the backend (verified through the
 * stored-configuration inspection helper) and (b) actually shape the sampled
 * input behavior:
 *
 *   PULL_NONE -> sampled raw LOW,  logical read INACTIVE (active-high)
 *   PULL_UP   -> sampled raw HIGH, logical read ACTIVE   (active-high)
 *   PULL_DOWN -> sampled raw LOW,  logical read INACTIVE (active-high)
 */
static void assert_gpio_input_pull(hal_gpio_pull_t pull, int expected_raw,
                                   bool expected_active)
{
    hal_gpio_pull_t stored_pull = (hal_gpio_pull_t)0xFF;
    bool state = false;
    int raw = -1;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_input(TEST_GPIO_PIN_INPUT,
                                                       HAL_POLARITY_ACTIVE_HIGH,
                                                       pull));

    /* The backend must retain the requested pull configuration. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_gpio_get_pull(TEST_GPIO_PIN_INPUT,
                                                               &stored_pull));
    TEST_ASSERT_EQUAL_INT((int)pull, (int)stored_pull);

    /* The retained pull must shape the sampled raw level, which the
     * active-high polarity then converts into the expected logical state. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_INPUT, &state));
    TEST_ASSERT_EQUAL(expected_active, state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_INPUT, &raw));
    TEST_ASSERT_EQUAL_INT(expected_raw, raw);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_INPUT));
}

/* 4. No pull.  An undriven input with no pull enabled samples LOW. */
static void test_gpio_pull_none(void)
{
    assert_gpio_input_pull(HAL_GPIO_PULL_NONE, TEST_RAW_LOW, false);
}

/* 5. Pull-up.  The simulated input is held HIGH. */
static void test_gpio_pull_up(void)
{
    assert_gpio_input_pull(HAL_GPIO_PULL_UP, TEST_RAW_HIGH, true);
}

/* 6. Pull-down.  The simulated input is held LOW. */
static void test_gpio_pull_down(void)
{
    assert_gpio_input_pull(HAL_GPIO_PULL_DOWN, TEST_RAW_LOW, false);
}

/* 7. Logical ACTIVE write. */
static void test_gpio_logical_active_write(void)
{
    bool state = false;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_HIGH, &state));
    TEST_ASSERT_TRUE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 8. Logical INACTIVE write. */
static void test_gpio_logical_inactive_write(void)
{
    bool state = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, false));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_HIGH, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 9. Logical read. */
static void test_gpio_logical_read(void)
{
    bool state = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_HIGH, &state));
    TEST_ASSERT_TRUE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, false));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_HIGH, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 10. Raw state for active-high.
 *
 *   ACTIVE_HIGH:
 *       ACTIVE   == raw HIGH
 *       INACTIVE == raw LOW
 */
static void test_gpio_raw_state_active_high(void)
{
    bool state = false;
    int raw = -1;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));

    /* Initial: logical INACTIVE == raw LOW. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_HIGH, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_ACTIVE_HIGH, &raw));
    TEST_ASSERT_EQUAL_INT(TEST_RAW_LOW, raw);

    /* ACTIVE == raw HIGH. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_ACTIVE_HIGH, &raw));
    TEST_ASSERT_EQUAL_INT(TEST_RAW_HIGH, raw);

    /* INACTIVE == raw LOW. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, false));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_ACTIVE_HIGH, &raw));
    TEST_ASSERT_EQUAL_INT(TEST_RAW_LOW, raw);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 11. Raw state for active-low.
 *
 *   ACTIVE_LOW:
 *       ACTIVE   == raw LOW
 *       INACTIVE == raw HIGH
 */
static void test_gpio_raw_state_active_low(void)
{
    bool state = true;
    int raw = -1;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_LOW,
                                                        HAL_POLARITY_ACTIVE_LOW,
                                                        HAL_GPIO_PULL_NONE));

    /* Initial: logical INACTIVE == raw HIGH. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_LOW, &state));
    TEST_ASSERT_FALSE(state);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_ACTIVE_LOW, &raw));
    TEST_ASSERT_EQUAL_INT(TEST_RAW_HIGH, raw);

    /* ACTIVE == raw LOW. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_LOW, true));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_ACTIVE_LOW, &raw));
    TEST_ASSERT_EQUAL_INT(TEST_RAW_LOW, raw);

    /* INACTIVE == raw HIGH. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_LOW, false));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)read_raw(TEST_GPIO_PIN_ACTIVE_LOW, &raw));
    TEST_ASSERT_EQUAL_INT(TEST_RAW_HIGH, raw);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_LOW));
}

/* Explicit full polarity truth table, both polarities. */
static void test_gpio_polarity_truth_table(void)
{
    /* ACTIVE_HIGH: ACTIVE == raw HIGH, INACTIVE == raw LOW. */
    assert_polarity_truth_table(TEST_GPIO_PIN_ACTIVE_HIGH, HAL_POLARITY_ACTIVE_HIGH,
                                TEST_RAW_HIGH, TEST_RAW_LOW);

    /* ACTIVE_LOW: ACTIVE == raw LOW, INACTIVE == raw HIGH. */
    assert_polarity_truth_table(TEST_GPIO_PIN_ACTIVE_LOW, HAL_POLARITY_ACTIVE_LOW,
                                TEST_RAW_LOW, TEST_RAW_HIGH);
}

/* 12. Invalid pin / configuration. */
static void test_gpio_invalid_pin_and_configuration(void)
{
    hal_gpio_config_t config;

    /* NULL config. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_init(NULL));

    /* HAL_PIN_NONE. */
    config.pin = HAL_PIN_NONE;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_init(&config));

    /* Out-of-range pin. */
    config.pin = 100U;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_init(&config));

    /* Out-of-range polarity. */
    config.pin = 0U;
    config.polarity = (hal_polarity_t)99;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_init(&config));

    /* Out-of-range pull. */
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = (hal_gpio_pull_t)99;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_init(&config));

    /* Operations on an invalid pin. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_write(100U, true));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_write(HAL_PIN_NONE, true));
    {
        bool state = false;
        TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_read(100U, &state));
    }
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, (int)hal_gpio_deinit(100U));

    /* NULL read output. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(0U, HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)hal_gpio_read(0U, NULL));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(0U));
}

/* 13. Operation before initialization. */
static void test_gpio_operation_before_initialization(void)
{
    bool state = false;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_write(0U, true));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_read(0U, &state));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_deinit(0U));
}

/* 14. Deinitialization. */
static void test_gpio_deinitialization(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
    /* Deinit is not idempotent. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));

    /* After deinit the pin may be initialized again. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));
}

/* 15. Operation after deinitialization. */
static void test_gpio_operation_after_deinitialization(void)
{
    bool state = false;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_gpio_output(TEST_GPIO_PIN_ACTIVE_HIGH,
                                                        HAL_POLARITY_ACTIVE_HIGH,
                                                        HAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_gpio_deinit(TEST_GPIO_PIN_ACTIVE_HIGH));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_write(TEST_GPIO_PIN_ACTIVE_HIGH, true));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_gpio_read(TEST_GPIO_PIN_ACTIVE_HIGH, &state));
}

/* -------------------------------------------------------------------- */
/* PWM tests                                                            */
/* -------------------------------------------------------------------- */

static hal_status_t init_pwm(hal_pin_t pin, uint32_t frequency_hz)
{
    hal_pwm_config_t config;

    config.pin = pin;
    config.frequency_hz = frequency_hz;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    return hal_pwm_init(&config);
}

/* 1. Initialization. */
static void test_pwm_initialization(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 2. 0% duty. */
static void test_pwm_duty_zero_percent(void)
{
    float duty = -1.0f;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 0.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, duty);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 3. 50% duty. */
static void test_pwm_duty_fifty_percent(void)
{
    float duty = -1.0f;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, duty);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 4. 100% duty. */
static void test_pwm_duty_hundred_percent(void)
{
    float duty = -1.0f;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 100.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, duty);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 5. Duty values above 100%. */
static void test_pwm_duty_above_max(void)
{
    float duty = 0.0f;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 50.0f));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(TEST_PWM_PIN, 100.1f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(TEST_PWM_PIN, INFINITY));

    /* The rejected value must leave the previous duty unchanged. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, duty);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 6. Negative duty values.  The float representation permits them, but the
 *    contract rejects them with HAL_ERR_OUT_OF_RANGE. */
static void test_pwm_duty_negative(void)
{
    float duty = 0.0f;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 50.0f));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(TEST_PWM_PIN, -1.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, (int)hal_pwm_set_duty(TEST_PWM_PIN, -INFINITY));

    /* The rejected value must leave the previous duty unchanged. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, duty);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 7. Configured frequency. */
static void test_pwm_configured_frequency(void)
{
    uint32_t frequency_hz = 0U;

    /* Init must accept a non-zero, positive frequency... */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 5000U));

    /* ...and the backend must retain exactly the requested value. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_frequency(TEST_PWM_PIN,
                                                                   &frequency_hz));
    TEST_ASSERT_EQUAL_UINT32(5000U, frequency_hz);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));

    /* A zero frequency is invalid. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, (int)init_pwm(TEST_PWM_PIN, 0U));
}

/* 8. Force inactive.  The output must be held at the logical INACTIVE raw
 *    level and generation halted, even though the stored duty remains
 *    non-zero (active-high PWM: logical INACTIVE == raw LOW). */
static void test_pwm_force_inactive(void)
{
    float duty = -1.0f;
    bool generating = false;
    int raw = -1;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));

    /* A freshly initialized output: duty 0.0%, generation running, line held
     * at the logical INACTIVE raw level (active-high -> raw LOW). */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_output_state(TEST_PWM_PIN,
                                                                      &raw, &generating));
    TEST_ASSERT_TRUE(generating);
    TEST_ASSERT_EQUAL_INT(TEST_RAW_LOW, raw);

    /* A non-zero duty drives the line to the logical ACTIVE raw level. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 75.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_output_state(TEST_PWM_PIN,
                                                                      &raw, &generating));
    TEST_ASSERT_TRUE(generating);
    TEST_ASSERT_EQUAL_INT(TEST_RAW_HIGH, raw);

    /* Force inactive: generation halts and the line is held at the logical
     * INACTIVE raw level even though the stored duty is still non-zero. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_force_inactive(TEST_PWM_PIN));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_output_state(TEST_PWM_PIN,
                                                                      &raw, &generating));
    TEST_ASSERT_FALSE(generating);
    TEST_ASSERT_EQUAL_INT(TEST_RAW_LOW, raw);

    /* The duty is retained across force-inactive. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(75.0f, duty);

    /* Setting a duty resumes normal generation (clears the forced state). */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_set_duty(TEST_PWM_PIN, 25.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_output_state(TEST_PWM_PIN,
                                                                      &raw, &generating));
    TEST_ASSERT_TRUE(generating);
    TEST_ASSERT_EQUAL_INT(TEST_RAW_HIGH, raw);

    /* Force inactive again: held at INACTIVE with the freshly set duty kept. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_force_inactive(TEST_PWM_PIN));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_output_state(TEST_PWM_PIN,
                                                                      &raw, &generating));
    TEST_ASSERT_FALSE(generating);
    TEST_ASSERT_EQUAL_INT(TEST_RAW_LOW, raw);
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_posix_pwm_get_duty(TEST_PWM_PIN, &duty));
    TEST_ASSERT_EQUAL_FLOAT(25.0f, duty);

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 9. Deinitialization. */
static void test_pwm_deinitialization(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
    /* Deinit is not idempotent. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_deinit(TEST_PWM_PIN));

    /* After deinit the pin may be initialized again. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 10. Operations before initialization. */
static void test_pwm_operation_before_initialization(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_set_duty(TEST_PWM_PIN, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_force_inactive(TEST_PWM_PIN));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_deinit(TEST_PWM_PIN));
}

/* 11. Operations after deinitialization. */
static void test_pwm_operation_after_deinitialization(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)init_pwm(TEST_PWM_PIN, 1000U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_deinit(TEST_PWM_PIN));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_set_duty(TEST_PWM_PIN, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, (int)hal_pwm_force_inactive(TEST_PWM_PIN));
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

    /* GPIO */
    RUN_TEST(test_gpio_initialization);
    RUN_TEST(test_gpio_active_high_configuration);
    RUN_TEST(test_gpio_active_low_configuration);
    RUN_TEST(test_gpio_pull_none);
    RUN_TEST(test_gpio_pull_up);
    RUN_TEST(test_gpio_pull_down);
    RUN_TEST(test_gpio_logical_active_write);
    RUN_TEST(test_gpio_logical_inactive_write);
    RUN_TEST(test_gpio_logical_read);
    RUN_TEST(test_gpio_raw_state_active_high);
    RUN_TEST(test_gpio_raw_state_active_low);
    RUN_TEST(test_gpio_polarity_truth_table);
    RUN_TEST(test_gpio_invalid_pin_and_configuration);
    RUN_TEST(test_gpio_operation_before_initialization);
    RUN_TEST(test_gpio_deinitialization);
    RUN_TEST(test_gpio_operation_after_deinitialization);

    /* PWM */
    RUN_TEST(test_pwm_initialization);
    RUN_TEST(test_pwm_duty_zero_percent);
    RUN_TEST(test_pwm_duty_fifty_percent);
    RUN_TEST(test_pwm_duty_hundred_percent);
    RUN_TEST(test_pwm_duty_above_max);
    RUN_TEST(test_pwm_duty_negative);
    RUN_TEST(test_pwm_configured_frequency);
    RUN_TEST(test_pwm_force_inactive);
    RUN_TEST(test_pwm_deinitialization);
    RUN_TEST(test_pwm_operation_before_initialization);
    RUN_TEST(test_pwm_operation_after_deinitialization);

    return UNITY_END();
}
