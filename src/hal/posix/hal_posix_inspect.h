/**
 * @file hal_posix_inspect.h
 * @brief Host/POSIX HAL inspection helpers (TASK-006)
 *
 * Test/backend-only inspection API for the simulated POSIX GPIO and PWM
 * backends (src/hal/posix/hal_gpio_posix.c and
 * src/hal/posix/hal_pwm_posix.c).
 *
 * IMPORTANT: these functions are **not** part of the portable HAL API.  They
 * are deliberately declared in a POSIX/test-only header outside
 * src/hal/include/, so portable application code can never depend on them:
 * the ESP-IDF backend never compiles them, src/hal/include never exposes
 * them, and the header is never part of the public interface of the `hq_hal`
 * library.
 *
 * They exist so unit tests can read backend-internal state that the public
 * API cannot expose and verify the deterministic behavior of the simulation:
 *
 *   - hal_posix_gpio_get_raw_state() returns the raw electrical level of an
 *     initialized GPIO pin, i.e. the physical level *after* the configured
 *     polarity has been applied.  This is what lets tests check the TASK-004
 *     polarity truth table directly (ACTIVE_HIGH + logical ACTIVE -> raw
 *     HIGH, ACTIVE_LOW + logical ACTIVE -> raw LOW, ...).
 *   - hal_posix_gpio_get_pull() returns the internal pull configuration the
 *     backend retained from hal_gpio_init(); tests use it to prove that the
 *     requested pull setting actually reached the backend state (and not
 *     merely that init accepted it), complementing the observable input/raw
 *     level checks.
 *   - hal_posix_pwm_get_duty() returns the normalized duty cycle stored by
 *     the PWM backend, including the duty value retained across
 *     hal_pwm_force_inactive().
 *   - hal_posix_pwm_get_frequency() returns the frequency the backend
 *     retained from hal_pwm_init(), so tests can verify that the requested
 *     value was actually configured rather than silently dropped.
 *   - hal_posix_pwm_get_output_state() returns the raw electrical level
 *     currently driven on the simulated PWM line and whether generation is
 *     running (false while forced inactive), so tests can verify that
 *     hal_pwm_force_inactive() really holds the output at the logical
 *     INACTIVE level.
 *
 * All helpers follow the same error contract as the public API: NULL
 * pointer -> #HAL_ERR_INVALID_ARGUMENT, invalid/none pin ->
 * #HAL_ERR_INVALID_PIN, uninitialized pin -> #HAL_ERR_NOT_INITIALIZED.
 */

#ifndef HAL_POSIX_INSPECT_H
#define HAL_POSIX_INSPECT_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_gpio.h"
#include "hal_pwm.h"
#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the raw electrical level of an initialized GPIO pin.
 *
 * @param[in]  pin       Initialized pin identifier.
 * @param[out] raw_level Non-NULL pointer receiving the raw electrical level
 *                       after the configured polarity conversion:
 *                         0 = physical LOW,
 *                         1 = physical HIGH.
 *
 * @return
 *  - #HAL_OK on success and @c *raw_level is valid,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p raw_level is NULL,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized.
 *
 * @note Host/test-only helper, not part of the portable HAL API.
 */
hal_status_t hal_posix_gpio_get_raw_state(hal_pin_t pin, int *raw_level);

/**
 * @brief Read the pull configuration retained by an initialized GPIO pin.
 *
 * @param[in]  pin  Initialized pin identifier.
 * @param[out] pull Non-NULL pointer receiving the #hal_gpio_pull_t value that
 *                  was supplied at hal_gpio_init() time and retained by the
 *                  backend.
 *
 * @return
 *  - #HAL_OK on success and @c *pull is valid,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p pull is NULL,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized.
 *
 * @note Host/test-only helper, not part of the portable HAL API.
 */
hal_status_t hal_posix_gpio_get_pull(hal_pin_t pin, hal_gpio_pull_t *pull);

/**
 * @brief Read the normalized duty cycle stored by the PWM backend.
 *
 * @param[in]  pin          Initialized PWM pin identifier.
 * @param[out] duty_percent Non-NULL pointer receiving the stored normalized
 *                          duty cycle in percent (closed interval
 *                          [0.0, 100.0]).  When the output is forced
 *                          inactive the previously set duty is retained, so
 *                          this returns that retained value.
 *
 * @return
 *  - #HAL_OK on success and @c *duty_percent is valid,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p duty_percent is NULL,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized.
 *
 * @note Host/test-only helper, not part of the portable HAL API.
 */
hal_status_t hal_posix_pwm_get_duty(hal_pin_t pin, float *duty_percent);

/**
 * @brief Read the frequency retained by an initialized PWM output.
 *
 * @param[in]  pin           Initialized PWM pin identifier.
 * @param[out] frequency_hz  Non-NULL pointer receiving the frequency value
 *                           that was supplied at hal_pwm_init() time and
 *                           retained by the backend.
 *
 * @return
 *  - #HAL_OK on success and @c *frequency_hz is valid,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p frequency_hz is NULL,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized.
 *
 * @note Host/test-only helper, not part of the portable HAL API.
 */
hal_status_t hal_posix_pwm_get_frequency(hal_pin_t pin, uint32_t *frequency_hz);

/**
 * @brief Read the raw output level and generation state of a PWM output.
 *
 * @param[in]  pin         Initialized PWM pin identifier.
 * @param[out] raw_level   Non-NULL pointer receiving the raw electrical level
 *                         currently driven on the simulated line, after the
 *                         active polarity conversion: 0 = physical LOW,
 *                         1 = physical HIGH.  While the output is forced
 *                         inactive (or the duty is 0.0%) the line is held at
 *                         the raw level of logical INACTIVE; with generation
 *                         running and a non-zero duty the line is at the raw
 *                         level of logical ACTIVE (the active phase of the
 *                         PWM cycle).
 * @param[out] generating  Non-NULL pointer receiving true while PWM
 *                         generation is running, or false while the output
 *                         is forced inactive by hal_pwm_force_inactive().
 *
 * @return
 *  - #HAL_OK on success and both outputs are valid,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p raw_level or @p generating is NULL,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized.
 *
 * @note Host/test-only helper, not part of the portable HAL API.
 */
hal_status_t hal_posix_pwm_get_output_state(hal_pin_t pin, int *raw_level,
                                            bool *generating);

#ifdef __cplusplus
}
#endif

#endif /* HAL_POSIX_INSPECT_H */