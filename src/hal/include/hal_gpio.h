/**
 * @file hal_gpio.h
 * @brief Portable GPIO HAL public API (TASK-004)
 *
 * Normative public API contract for GPIO control.  This header defines the
 * portable, target-independent GPIO interface of the hq_platform HAL.
 *
 * Scope of this contract:
 *   - selecting a GPIO pin,
 *   - configuring the active polarity (logical level mapping),
 *   - configuring an optional internal pull (none/up/down),
 *   - configuring the pin as input or output,
 *   - reading and writing the pin's *logical* state,
 *   - deinitializing (releasing) the pin.
 *
 * Logical values
 * --------------
 * The API exchanges **logical values**, never raw electrical levels.
 * "ACTIVE" is whatever electrical level is defined by the configured
 * polarity; the pin's configured polarity is applied on both write and
 * read:
 *
 *   | Polarity            | logical ACTIVE (true) | logical INACTIVE (false) |
 *   |---------------------|-----------------------|--------------------------|
 *   | HAL_POLARITY_ACTIVE_HIGH | physical HIGH       | physical LOW              |
 *   | HAL_POLARITY_ACTIVE_LOW  | physical LOW        | physical HIGH             |
 *
 * Therefore:
 *   - hal_gpio_write(pin, true)  drives the pin to its configured *logical
 *     ACTIVE* state (which may be physical HIGH or physical LOW),
 *   - hal_gpio_write(pin, false) drives the pin to its configured *logical
 *     INACTIVE* state,
 *   - hal_gpio_read() returns the *logical* state after applying the
 *     configured polarity.
 *
 * Ownership and initialization
 * ----------------------------
 *   - The application owns the pin identifier and the init/deinit
 *     lifecycle: a pin must be initialized once with hal_gpio_init()
 *     before any hal_gpio_write()/hal_gpio_read() call, and released with
 *     hal_gpio_deinit() when no longer needed.
 *   - Initializing a pin that is already initialized returns
 *     #HAL_ERR_ALREADY_INITIALIZED and leaves the existing configuration
 *     unchanged.  Reconfiguration requires an explicit deinit first.
 *   - Deinitializing a pin that is not initialized returns
 *     #HAL_ERR_NOT_INITIALIZED.  After a successful deinit the pin is free
 *     and may be initialized again.
 *   - Unless a backend documents otherwise, HAL functions are not required
 *     to be thread-safe; applications must serialize access per pin.
 *   - #hal_pin_t identifiers are portable integers; the mapping to the
 *     physical GPIO is an implementation detail of each backend.  A pin
 *     value of #HAL_PIN_NONE is invalid on every backend.
 *
 * The GPIO pull configuration is portable (#hal_gpio_pull_t).  The exact
 * target-specific pull resistors/registers remain an implementation
 * detail of the backends.
 */

#ifndef HAL_GPIO_H
#define HAL_GPIO_H

#include <stdbool.h>

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* hal_gpio_pull_t - internal pull configuration                         */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable internal pull configuration for a GPIO pin.
 *
 * Controls the optional internal pull resistor of the pin when it is
 * configured as an input (and, where the target supports it, as an
 * output).  The exact electrical implementation (which resistor is
 * enabled, and whether it is available in all modes) is backend-specific;
 * the portable semantics below are guaranteed.
 */
typedef enum hal_gpio_pull {
    HAL_GPIO_PULL_NONE  = 0, /**< No internal pull resistor. */
    HAL_GPIO_PULL_UP    = 1, /**< Enable the internal pull-up resistor. */
    HAL_GPIO_PULL_DOWN  = 2  /**< Enable the internal pull-down resistor. */
} hal_gpio_pull_t;

/* --------------------------------------------------------------------- */
/* hal_gpio_config_t - initialization configuration                      */
/* --------------------------------------------------------------------- */

/**
 * @brief GPIO initialization configuration.
 *
 * Supplied to hal_gpio_init().  The application must populate every field.
 */
typedef struct hal_gpio_config {
    hal_pin_t       pin;      /**< Pin to initialize.  Must not be #HAL_PIN_NONE. */
    hal_polarity_t  polarity; /**< Active polarity applied to all logical values
                                   (see the truth table in the file comment). */
    hal_gpio_pull_t pull;     /**< Internal pull configuration. */
    bool            output;   /**< true = configure the pin as an output driver,
                                   false = configure the pin as an input. */
} hal_gpio_config_t;

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialize and configure a GPIO pin.
 *
 * @param[in] config  Non-NULL pointer to the pin configuration.
 *
 * @return
 *  - #HAL_OK on success; the pin is now initialized and ready,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p config is NULL or contains an
 *    out-of-range #hal_polarity_t / #hal_gpio_pull_t value,
 *  - #HAL_ERR_INVALID_PIN if @c config->pin is #HAL_PIN_NONE or is not a
 *    valid pin on this platform,
 *  - #HAL_ERR_ALREADY_INITIALIZED if @c config->pin is already initialized
 *    (the existing configuration is left unchanged),
 *  - #HAL_ERR_NOT_SUPPORTED if the requested direction/pull combination is
 *    not supported by the backend,
 *  - #HAL_ERR_NO_RESOURCE if no hardware resource is available,
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note Configuring a pin as an *output* drives it to the logical
 *       INACTIVE state immediately, so the electrical level after init is
 *       well defined (see the truth table above).  Configuring it as an
 *       *input* leaves it high-impedance apart from any enabled pull.
 */
hal_status_t hal_gpio_init(const hal_gpio_config_t *config);

/**
 * @brief Drive a GPIO pin to a logical state.
 *
 * @param[in] pin     Initialized pin identifier.
 * @param[in] active  Logical state to drive:
 *                      - true  = logical ACTIVE,
 *                      - false = logical INACTIVE.
 *                    The configured polarity is applied, so the physical
 *                    level on the wire depends on
 *                    @c config.polarity from hal_gpio_init().
 *
 * @return
 *  - #HAL_OK on success,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized,
 *  - #HAL_ERR_NOT_SUPPORTED if @p pin was configured as an input and the
 *    backend cannot drive it,
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note After a successful write, hal_gpio_read() on an output pin returns
 *       the same logical state, because the output latch is read back with
 *       the same polarity applied.
 */
hal_status_t hal_gpio_write(hal_pin_t pin, bool active);

/**
 * @brief Read the logical state of a GPIO pin.
 *
 * @param[in]  pin     Initialized pin identifier.
 * @param[out] active  Non-NULL pointer that receives the logical state
 *                     after applying the configured polarity:
 *                       - true  = logical ACTIVE,
 *                       - false = logical INACTIVE.
 *
 * @return
 *  - #HAL_OK on success and @c *active is valid,
 *  - #HAL_ERR_INVALID_ARGUMENT if @p active is NULL,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized,
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note On an input pin this samples the electrical level and applies the
 *       polarity.  On an output pin this returns the logical state of the
 *       output latch (see hal_gpio_write()).
 */
hal_status_t hal_gpio_read(hal_pin_t pin, bool *active);

/**
 * @brief Deinitialize a GPIO pin and release it.
 *
 * @param[in] pin  Initialized pin identifier to release.
 *
 * @return
 *  - #HAL_OK on success; the pin is no longer initialized and its
 *    backend resources (register configuration, handle, ...) are freed,
 *  - #HAL_ERR_INVALID_PIN if @p pin is #HAL_PIN_NONE or not valid on this
 *    platform,
 *  - #HAL_ERR_NOT_INITIALIZED if @p pin is not initialized (for example a
 *    second hal_gpio_deinit() call without a re-init in between),
 *  - #HAL_ERR_INTERNAL on an unexpected backend failure.
 *
 * @note Deinitialization is not idempotent: calling it twice in a row
 *       returns #HAL_ERR_NOT_INITIALIZED on the second call.  After a
 *       successful deinit the pin identifier may be initialized again.
 */
hal_status_t hal_gpio_deinit(hal_pin_t pin);

#ifdef __cplusplus
}
#endif

#endif /* HAL_GPIO_H */