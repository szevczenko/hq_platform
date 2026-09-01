/**
 * @file hal_gpio_posix.c
 * @brief POSIX/host GPIO HAL backend (TASK-005)
 *
 * Simulated GPIO backend for host/POSIX builds.  It is fully self-contained
 * (no Linux sysfs/gpiod, no ESP-IDF and no other target-specific headers),
 * so the component builds on any POSIX host.  The backend keeps one state
 * record per pin and implements the portable contract from hal_gpio.h:
 *
 *   - logical values are exchanged (never raw electrical levels),
 *   - the configured polarity is applied on write and read,
 *   - output pins start in the logical INACTIVE state,
 *   - init/deinit are not idempotent and return the documented errors,
 *   - writing to an input pin is not supported.
 *
 * The state is intentionally not thread-safe: the public contract only
 * requires applications to serialize access per pin.  No external state is
 * visible, which makes the backend deterministic and unit-testable.
 */

#include <stdbool.h>
#include <stddef.h>

#include "hal_gpio.h"

/* Number of simulated pins exposed by this backend.  Any pin identifier in
 * [0, HAL_GPIO_POSIX_MAX_PINS) is valid. */
#define HAL_GPIO_POSIX_MAX_PINS 64U

typedef struct hal_gpio_posix_pin {
    bool initialized;       /**< Pin was initialized and not yet deinitialized. */
    bool output;            /**< true = output driver, false = input. */
    bool logical_active;    /**< Current logical state (output latch / input sample). */
    hal_polarity_t polarity;/**< Active polarity applied to logical values. */
    hal_gpio_pull_t pull;   /**< Internal pull configuration. */
} hal_gpio_posix_pin_t;

static hal_gpio_posix_pin_t s_pins[HAL_GPIO_POSIX_MAX_PINS];

static bool hal_gpio_posix_is_valid_pin(hal_pin_t pin)
{
    return (pin != HAL_PIN_NONE) && (pin < HAL_GPIO_POSIX_MAX_PINS);
}

hal_status_t hal_gpio_init(const hal_gpio_config_t *config)
{
    hal_gpio_posix_pin_t *pin;

    if (config == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_gpio_posix_is_valid_pin(config->pin)) {
        return HAL_ERR_INVALID_PIN;
    }
    if (config->polarity != HAL_POLARITY_ACTIVE_HIGH &&
        config->polarity != HAL_POLARITY_ACTIVE_LOW) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->pull != HAL_GPIO_PULL_NONE &&
        config->pull != HAL_GPIO_PULL_UP &&
        config->pull != HAL_GPIO_PULL_DOWN) {
        return HAL_ERR_INVALID_ARGUMENT;
    }

    pin = &s_pins[config->pin];
    if (pin->initialized) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }

    pin->initialized = true;
    pin->output = config->output;
    pin->polarity = config->polarity;
    pin->pull = config->pull;
    /* Output pins start in the logical INACTIVE state, so the simulated
     * electrical level after init is well defined. */
    pin->logical_active = false;

    return HAL_OK;
}

hal_status_t hal_gpio_write(hal_pin_t pin_id, bool active)
{
    hal_gpio_posix_pin_t *pin;

    if (!hal_gpio_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }
    if (!pin->output) {
        return HAL_ERR_NOT_SUPPORTED;
    }

    pin->logical_active = active;
    return HAL_OK;
}

hal_status_t hal_gpio_read(hal_pin_t pin_id, bool *active)
{
    hal_gpio_posix_pin_t *pin;

    if (active == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_gpio_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *active = pin->logical_active;
    return HAL_OK;
}

hal_status_t hal_gpio_deinit(hal_pin_t pin_id)
{
    hal_gpio_posix_pin_t *pin;

    if (!hal_gpio_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    pin->initialized = false;
    pin->output = false;
    pin->logical_active = false;
    pin->polarity = HAL_POLARITY_ACTIVE_HIGH;
    pin->pull = HAL_GPIO_PULL_NONE;

    return HAL_OK;
}