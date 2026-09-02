/**
 * @file hal_gpio_posix.c
 * @brief POSIX/host GPIO HAL backend (TASK-006)
 *
 * Simulated GPIO backend for host/POSIX builds.  It is fully self-contained
 * (no Linux sysfs/gpiod, no ESP-IDF and no other target-specific headers),
 * so the component builds on any POSIX host.  No real host GPIO hardware is
 * ever accessed; all state is kept in deterministic in-memory records, one
 * per initialized pin.
 *
 * Each record tracks:
 *   - the pin identifier,
 *   - the initialization state,
 *   - the active polarity,
 *   - the internal pull configuration,
 *   - the logical state (logical ACTIVE/INACTIVE),
 *   - the raw electrical state (physical LOW/HIGH) of the simulated line.
 *
 * The backend implements the portable contract from hal_gpio.h:
 *
 *   - logical values are exchanged (never raw electrical levels),
 *   - the configured polarity (TASK-004) is applied on write and read with
 *     exactly this conversion:
 *
 *       ACTIVE_HIGH + logical ACTIVE   -> raw HIGH (1)
 *       ACTIVE_HIGH + logical INACTIVE -> raw LOW  (0)
 *       ACTIVE_LOW  + logical ACTIVE   -> raw LOW  (0)
 *       ACTIVE_LOW  + logical INACTIVE -> raw HIGH (1)
 *
 *   - output pins start in the logical INACTIVE state,
 *   - input pins sample the electrical level determined by the pull
 *     configuration (pull-up -> HIGH, otherwise LOW) and apply the polarity,
 *   - init/deinit are not idempotent and return the documented errors,
 *   - writing to an input pin is not supported.
 *
 * The state is intentionally not thread-safe: the public contract only
 * requires applications to serialize access per pin.  No external state is
 * visible, which makes the backend deterministic and unit-testable.
 *
 * The host/test-only inspection helper hal_posix_gpio_get_raw_state() (see
 * hal_posix_inspect.h) exposes the raw electrical level after the polarity
 * conversion so unit tests can verify the truth table above directly.
 */

#include <stdbool.h>
#include <stddef.h>

#include "hal_gpio.h"
#include "hal_posix_inspect.h"

/* Number of simulated pins exposed by this backend.  Any pin identifier in
 * [0, HAL_GPIO_POSIX_MAX_PINS) is valid. */
#define HAL_GPIO_POSIX_MAX_PINS 64U

typedef struct hal_gpio_posix_pin {
    hal_pin_t pin;              /**< Pin identifier (mirrors the array index). */
    bool initialized;           /**< Pin was initialized and not yet deinitialized. */
    bool output;                /**< true = output driver, false = input. */
    hal_polarity_t polarity;    /**< Active polarity applied to logical values. */
    hal_gpio_pull_t pull;       /**< Internal pull configuration. */
    bool logical_active;        /**< Logical state (output latch / sampled input). */
    int raw_level;              /**< Raw electrical state: 0 = LOW, 1 = HIGH. */
} hal_gpio_posix_pin_t;

static hal_gpio_posix_pin_t s_pins[HAL_GPIO_POSIX_MAX_PINS];

static bool hal_gpio_posix_is_valid_pin(hal_pin_t pin)
{
    return (pin != HAL_PIN_NONE) && (pin < HAL_GPIO_POSIX_MAX_PINS);
}

/* Convert a logical state to the raw electrical level for the given active
 * polarity (TASK-004 truth table). */
static int hal_gpio_posix_raw_for_logical(hal_polarity_t polarity, bool active)
{
    const bool invert = (polarity == HAL_POLARITY_ACTIVE_LOW);

    return (active != invert) ? 1 : 0;
}

/* Convert a raw electrical level to the logical state for the given active
 * polarity (the inverse of the TASK-004 truth table). */
static bool hal_gpio_posix_logical_for_raw(hal_polarity_t polarity, int raw_level)
{
    const bool invert = (polarity == HAL_POLARITY_ACTIVE_LOW);

    return ((raw_level != 0) != invert);
}

/* The simulated electrical level sampled by an undriven input pin is
 * determined by its pull configuration: a pull-up holds the line HIGH,
 * everything else (none/pull-down) reads LOW. */
static int hal_gpio_posix_input_raw_level(hal_gpio_pull_t pull)
{
    return (pull == HAL_GPIO_PULL_UP) ? 1 : 0;
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

    pin->pin = config->pin;
    pin->initialized = true;
    pin->output = config->output;
    pin->polarity = config->polarity;
    pin->pull = config->pull;

    if (config->output) {
        /* Output pins start in the logical INACTIVE state, so the simulated
         * raw electrical level after init is well defined: it is the
         * physical level of logical INACTIVE for the configured polarity. */
        pin->logical_active = false;
        pin->raw_level = hal_gpio_posix_raw_for_logical(config->polarity, false);
    } else {
        /* Input pins sample the level determined by the pull configuration
         * and present it through the polarity as a logical state. */
        pin->raw_level = hal_gpio_posix_input_raw_level(config->pull);
        pin->logical_active =
            hal_gpio_posix_logical_for_raw(config->polarity, pin->raw_level);
    }

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

    /* Apply the TASK-004 polarity conversion to derive the simulated raw
     * electrical level from the requested logical state. */
    pin->logical_active = active;
    pin->raw_level = hal_gpio_posix_raw_for_logical(pin->polarity, active);

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

    if (pin->output) {
        /* Output pins read back the logical state of the output latch. */
        *active = pin->logical_active;
    } else {
        /* Input pins sample the simulated raw electrical level and apply the
         * configured polarity. */
        *active = hal_gpio_posix_logical_for_raw(pin->polarity, pin->raw_level);
        pin->logical_active = *active;
    }

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

    pin->pin = HAL_PIN_NONE;
    pin->initialized = false;
    pin->output = false;
    pin->logical_active = false;
    pin->polarity = HAL_POLARITY_ACTIVE_HIGH;
    pin->pull = HAL_GPIO_PULL_NONE;
    pin->raw_level = 0;

    return HAL_OK;
}

hal_status_t hal_posix_gpio_get_raw_state(hal_pin_t pin_id, int *raw_level)
{
    hal_gpio_posix_pin_t *pin;

    if (raw_level == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_gpio_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *raw_level = pin->raw_level;

    return HAL_OK;
}

hal_status_t hal_posix_gpio_get_pull(hal_pin_t pin_id, hal_gpio_pull_t *pull)
{
    hal_gpio_posix_pin_t *pin;

    if (pull == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_gpio_posix_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    pin = &s_pins[pin_id];
    if (!pin->initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *pull = pin->pull;

    return HAL_OK;
}