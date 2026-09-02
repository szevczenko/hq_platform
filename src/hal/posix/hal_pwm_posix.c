/**
 * @file hal_pwm_posix.c
 * @brief POSIX/host PWM HAL backend (TASK-006)
 *
 * Simulated PWM backend for host/POSIX builds.  Like the GPIO backend it is
 * fully self-contained (no Linux/ESP-IDF headers) so the component builds on
 * any POSIX host and the backend is deterministic and unit-testable.  No real
 * host PWM hardware is ever accessed; all state is kept in deterministic
 * in-memory records, one per initialized output.
 *
 * Each record tracks:
 *   - the pin identifier,
 *   - the initialization state,
 *   - the configured frequency,
 *   - the normalized duty cycle in percent,
 *   - the active polarity,
 *   - a "forced inactive" flag that halts generation at the logical
 *     INACTIVE level until hal_pwm_set_duty() is called again.
 *
 * Per the contract, hal_pwm_init() starts the output at duty 0.0%
 * (logical INACTIVE) and hal_pwm_force_inactive() retains the last duty
 * value so a later hal_pwm_set_duty() resumes normal generation.
 *
 * The host/test-only inspection helper hal_posix_pwm_get_duty() (see
 * hal_posix_inspect.h) exposes the stored normalized duty cycle so unit tests
 * can verify that every public API operation is reflected in the backend
 * state, including the duty value retained across hal_pwm_force_inactive().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_pwm.h"
#include "hal_posix_inspect.h"

/* Number of simulated PWM outputs exposed by this backend. */
#define HAL_PWM_POSIX_MAX_PINS 64U

typedef struct hal_pwm_posix_pin {
    hal_pin_t pin;           /**< Pin identifier (mirrors the array index). */
    bool initialized;        /**< Output was initialized and not yet deinitialized. */
    hal_polarity_t polarity; /**< Active polarity of the output. */
    uint32_t frequency_hz;   /**< Configured PWM frequency. */
    float duty_percent;      /**< Normalized duty in percent [0.0, 100.0]. */
    bool forced_inactive;    /**< Output is halted at logical INACTIVE. */
} hal_pwm_posix_pin_t;

static hal_pwm_posix_pin_t s_pins[HAL_PWM_POSIX_MAX_PINS];

static bool hal_pwm_posix_is_valid_pin(hal_pin_t pin)
{
    return (pin != HAL_PIN_NONE) && (pin < HAL_PWM_POSIX_MAX_PINS);
}

static bool hal_pwm_posix_is_valid_duty(float duty_percent)
{
    /* NaN fails both comparisons; +-inf fail the upper/lower bound. */
    return (duty_percent >= HAL_PWM_DUTY_MIN_PERCENT) &&
           (duty_percent <= HAL_PWM_DUTY_MAX_PERCENT);
}

hal_status_t hal_pwm_init(const hal_pwm_config_t *config)
{
    hal_pwm_posix_pin_t *pin;

    if (config == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->frequency_hz == 0U) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->polarity != HAL_POLARITY_ACTIVE_HIGH &&
        config->polarity != HAL_POLARITY_ACTIVE_LOW) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_pwm_posix_is_valid_pin(config->pin)) {
        return HAL_ERR_INVALID_PIN;
    }

    pin = &s_pins[config->pin];
    if (pin->initialized) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }

    pin->pin = config->pin;
    pin->initialized = true;
    pin->polarity = config->polarity;
    pin->frequency_hz = config->frequency_hz;
    /* The output starts in the logical INACTIVE state (duty 0.0%). */
    pin->duty_percent = HAL_PWM_DUTY_MIN_PERCENT;
    pin->forced_inactive = false;

    return HAL_OK;
}

hal_status_t hal_pwm_set_duty(hal_pin_t pin_id, float duty_percent)
{
    hal_pwm_posix_pin_t *pin;

    if (!hal_pwm_posix_is_valid_duty(duty_percent)) {
        return HAL_ERR_OUT_OF_RANGE;
    }
    if (!hal_pwm_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    pin->duty_percent = duty_percent;
    /* Setting a duty resumes normal generation (clears force_inactive). */
    pin->forced_inactive = false;

    return HAL_OK;
}

hal_status_t hal_pwm_force_inactive(hal_pin_t pin_id)
{
    hal_pwm_posix_pin_t *pin;

    if (!hal_pwm_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    /* The previously set duty is retained; only generation is halted. */
    pin->forced_inactive = true;

    return HAL_OK;
}

hal_status_t hal_pwm_deinit(hal_pin_t pin_id)
{
    hal_pwm_posix_pin_t *pin;

    if (!hal_pwm_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    pin->pin = HAL_PIN_NONE;
    pin->initialized = false;
    pin->polarity = HAL_POLARITY_ACTIVE_HIGH;
    pin->frequency_hz = 0U;
    pin->duty_percent = HAL_PWM_DUTY_MIN_PERCENT;
    pin->forced_inactive = false;

    return HAL_OK;
}

hal_status_t hal_posix_pwm_get_duty(hal_pin_t pin_id, float *duty_percent)
{
    hal_pwm_posix_pin_t *pin;

    if (duty_percent == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_pwm_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *duty_percent = pin->duty_percent;

    return HAL_OK;
}