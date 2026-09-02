/**
 * @file hal_pwm.h
 * @brief Portable PWM HAL public API (TASK-004)
 *
 * Normative public API contract for PWM output.  This header defines the
 * portable, target-independent PWM interface of the hq_platform HAL.
 *
 * Scope of this contract:
 *   - selecting an output pin,
 *   - configuring the PWM frequency,
 *   - configuring the active polarity,
 *   - setting a normalized duty cycle,
 *   - forcing the output to the logical inactive state,
 *   - deinitializing (releasing) the PWM output.
 *
 * Duty cycle semantics
 * --------------------
 * The duty cycle is a normalized percentage represented as a float:
 *
 *     0.0% <= duty <= 100.0%
 *
 *   - duty == 0.0%  -> the output is permanently at the logical INACTIVE
 *                      level (no active portion in the period),
 *   - duty == 100.0% -> the output is permanently at the logical ACTIVE
 *                      level (the whole period is active),
 *   - otherwise, the output is at the logical ACTIVE level for
 *     @c duty/100.0 of every period and at the logical INACTIVE level for
 *     the remainder.  "Active" always refers to the level selected by the
 *     configured polarity:
 *       - with HAL_POLARITY_ACTIVE_HIGH the active portion is physical
 *         HIGH,
 *       - with HAL_POLARITY_ACTIVE_LOW the active portion is physical LOW.
 *
 * Any duty value outside [0.0, 100.0] (including negative values, values
 * above 100.0, NaN and infinities) is rejected with an error and leaves
 * the current output unchanged.
 *
 * Resource allocation
 * -------------------
 * The selection and management of hardware timers and channels (e.g.
 * ESP-IDF LEDC timers, LEDC channels and LEDC resolution types) is an
 * implementation detail of each backend.  This public API never exposes
 * timers, channels, resolution types or platform-specific PWM handles.
 *
 * Ownership and initialization
 * ----------------------------
 *   - The application owns the pin identifier and the init/deinit
 *     lifecycle: a pin must be initialized once with hal_pwm_init()
 *     before any hal_pwm_set_duty()/hal_pwm_force_inactive() call, and
 *     released with hal_pwm_deinit() when no longer needed.
 *   - hal_pwm_init() starts the output in the logical INACTIVE state
 *     (duty 0.0%); the application then sets the desired duty.
 *   - Initializing a pin that is already initialized returns
 *     #HAL_ERR_ALREADY_INITIALIZED and leaves the existing configuration
 *     unchanged.  Reconfiguration requires an explicit deinit first.
 *   - Deinitializing a pin that is not initialized returns
 *     #HAL_ERR_NOT_INITIALIZED.  After a successful deinit the pin is free
 *     and may be initialized again.
 *   - Unless a backend documents otherwise, HAL functions are not required
 *     to be thread-safe; applications must serialize access per pin.
 *   - #hal_pin_t identifiers are portable integers; the mapping to the
 *     physical output pin is an implementation detail of each backend.  A
 *     pin value of #HAL_PIN_NONE is invalid on every backend.
 *
 * Frequency range
 * ---------------
 * The supported frequency range is backend-dependent: each backend must
 * document the minimum/maximum frequency (and effective resolution) it can
 * produce on its hardware.  This contract only requires that
 * @c frequency_hz is non-zero and positive.  A frequency the backend
 * cannot produce is reported with #HAL_ERR_NOT_SUPPORTED or
 * #HAL_ERR_OUT_OF_RANGE.
 */

#ifndef HAL_PWM_H
#define HAL_PWM_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Minimum allowed normalized duty cycle (0.0%). */
#define HAL_PWM_DUTY_MIN_PERCENT 0.0f

/** @brief Maximum allowed normalized duty cycle (100.0%). */
#define HAL_PWM_DUTY_MAX_PERCENT 100.0f

/* --------------------------------------------------------------------- */
/* hal_pwm_config_t - initialization configuration                       */
/* --------------------------------------------------------------------- */

/**
 * @brief PWM initialization configuration.
 *
 * Supplied to hal_pwm_init().  The application must populate every field.
 */
typedef struct hal_pwm_config {
    hal_pin_t      pin;           /**< Output pin.  Must not be #HAL_PIN_NONE. */
    uint32_t       frequency_hz;  /**< PWM frequency in hertz.  Must be
                                       greater than zero; the valid range and
                                       achievable resolution are
                                       backend-dependent (see file comment). */
    hal_polarity_t polarity;      /**< Active polarity of the output (see
                                       duty semantics in the file comment). */
} hal_pwm_config_t;

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialize a PWM output on a pin.
 *
 * @param[in] config  Non-NULL pointer to the PWM configuration.
 *
 * @return
 *  - #HAL_OK on success; the pin is now initialized, the backend has
 *    allocated a timer/channel resource for it and the output starts in
 *    the logical INACTIVE state (duty 0.0%),
 *  - #HAL_ERR_INVALID_ARGUMENT if @p config is NULL, @c config->frequency_hz
 *    is zero, or @c config->polarity is out of range,
 *  - #HAL_ERR_INVALID_PIN if @c config->pin is #HAL_PIN_NONE or is not a
 *    valid pin on this platform,
 *  - #HAL_ERR_ALREADY_INITIALIZED if @c config->pin is already initialized
 *    (the existing configuration is left unchanged),
 *  - #HAL_ERR_NOT_SUPPORTED if the requested frequency is outside the
 *    range the backend can produce,
 *  - #HAL_ERR_NO_RESOURCE if no timer/channel resource is available,
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note After init the caller uses hal_pwm_set_duty() to set the desired
 *       output level.  The timer/channel selection and management stays
 *       inside the backend.
 */
hal_status_t hal_pwm_init(const hal_pwm_config_t *config);

/**
 * @brief Set the normalized duty cycle of a PWM output.
 *
 * @param[in] pin          Initialized PWM pin identifier.
 * @param[in] duty_percent Normalized duty cycle in percent, in the closed
 *                         interval [0.0, 100.0] (see duty semantics in the
 *                         file comment).  0.0 produces a permanently
 *                         inactive output, 100.0 a permanently active one.
 *
 * @return
 *  - #HAL_OK on success; the output now follows the new duty cycle,
 *  - #HAL_ERR_OUT_OF_RANGE if @p duty_percent is outside
 *    [0.0, 100.0] (including NaN and infinities); the current output is
 *    unchanged,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized,
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note Calling hal_pwm_set_duty() after hal_pwm_force_inactive() resumes
 *       normal PWM generation with the new duty cycle (the forced-inactive
 *       state is cleared).
 */
hal_status_t hal_pwm_set_duty(hal_pin_t pin, float duty_percent);

/**
 * @brief Force a PWM output to its logical INACTIVE state.
 *
 * Independently of the current (or previously set) duty cycle, the
 * physical output is driven to the configured logical INACTIVE level
 * (physical LOW for HAL_POLARITY_ACTIVE_HIGH, physical HIGH for
 * HAL_POLARITY_ACTIVE_LOW) and PWM generation is halted.
 *
 * @param[in] pin  Initialized PWM pin identifier.
 *
 * @return
 *  - #HAL_OK on success; the output is held at the logical INACTIVE level
 *    even if the configured duty would otherwise produce an active output,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized,
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note The previously set duty value is retained; a subsequent
 *       hal_pwm_set_duty() resumes PWM generation at the new duty (see
 *       hal_pwm_set_duty()).  Calling hal_pwm_force_inactive() on an
 *       already forced/inactive output succeeds and returns #HAL_OK.
 */
hal_status_t hal_pwm_force_inactive(hal_pin_t pin);

/**
 * @brief Deinitialize a PWM output and release its resources.
 *
 * @param[in] pin  Initialized PWM pin identifier to release.
 *
 * @return
 *  - #HAL_OK on success; the output stops, the pin is no longer
 *    initialized, and the backend frees its timer/channel resources,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized (for example a
 *    second hal_pwm_deinit() call without a re-init in between),
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note Deinitialization is not idempotent: calling it twice in a row
 *       returns #HAL_ERR_NOT_INITIALIZED on the second call.  After a
 *       successful deinit the pin identifier may be initialized again.
 */
hal_status_t hal_pwm_deinit(hal_pin_t pin);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PWM_H */