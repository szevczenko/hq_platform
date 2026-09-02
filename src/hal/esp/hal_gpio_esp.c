/**
 * @file hal_gpio_esp.c
 * @brief ESP-IDF GPIO HAL backend (TASK-008)
 *
 * GPIO backend for ESP-IDF builds.  It drives the ESP32-family GPIO hardware
 * through the `driver/gpio.h` API and implements the portable contract from
 * hal_gpio.h:
 *
 *   - hal_pin_t values in [0, GPIO_NUM_MAX) are translated to ESP-IDF
 *     gpio_num_t GPIO numbers; the SoC valid-pad mask rejects reserved
 *     numbers (e.g. 24 and 28..31 on the classic ESP32) as invalid pins,
 *   - logical values are exchanged; the configured polarity is applied on
 *     write and read (active-low inverts the electrical level),
 *   - output pins start in the logical INACTIVE state,
 *   - the internal pull configuration maps to the ESP pull resistors,
 *   - pads that cannot drive an output (input-only pads) reject output mode
 *     and any pull request as not supported,
 *   - init/deinit are not idempotent and return the documented errors,
 *   - writing to an input pin is not supported,
 *   - every ESP-IDF esp_err_t result is mapped to the portable hal_status_t
 *     result type.
 *
 * The public HAL headers themselves are pure C99 and include no ESP-IDF
 * headers; all target-specific types stay inside this backend source.
 */

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "hal_gpio.h"

/* Pin identifiers [0, GPIO_NUM_MAX) are numbered pads on ESP targets;
 * GPIO_NUM_MAX is the count of numbered pads.  The SoC valid-pad mask
 * further excludes reserved numbers. */
#define HAL_GPIO_ESP_MAX_PINS GPIO_NUM_MAX

typedef struct hal_gpio_esp_pin {
    bool initialized;        /**< Pin was initialized and not yet deinitialized. */
    bool output;             /**< true = output driver, false = input. */
    hal_polarity_t polarity; /**< Active polarity applied to logical values. */
} hal_gpio_esp_pin_t;

static hal_gpio_esp_pin_t s_pins[HAL_GPIO_ESP_MAX_PINS];

static bool hal_gpio_esp_is_valid_pin(hal_pin_t pin)
{
    /* The < GPIO_NUM_MAX bound must be checked before the SoC mask macro,
     * whose shift operand is only defined for the numbered pads. */
    return (pin != HAL_PIN_NONE) && (pin < HAL_GPIO_ESP_MAX_PINS) &&
           GPIO_IS_VALID_GPIO((gpio_num_t)pin);
}

/* Map an ESP-IDF esp_err_t result onto the portable hal_status_t result
 * type.  The portable layer pre-validates every argument before any driver
 * call, so an ESP_ERR_INVALID_ARG here means the driver rejected a pin the
 * portable layer cannot know about (for example a pad reserved by another
 * peripheral at the SoC level); everything else is unexpected and reported
 * as an internal backend failure. */
static hal_status_t hal_gpio_esp_map_error(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return HAL_OK;
    case ESP_ERR_INVALID_ARG:
        return HAL_ERR_INVALID_ARGUMENT;
    default:
        return HAL_ERR_INTERNAL;
    }
}

static int hal_gpio_esp_physical_level_for_polarity(hal_polarity_t polarity,
                                                       bool active)
{
    /* Logical ACTIVE maps to physical HIGH for active-high, physical LOW
     * for active-low (and vice versa for logical INACTIVE). */
    const bool invert = polarity == HAL_POLARITY_ACTIVE_LOW;

    return (active != invert) ? 1 : 0;
}

static int hal_gpio_esp_physical_level(hal_pin_t pin, bool active)
{
    return hal_gpio_esp_physical_level_for_polarity(s_pins[pin].polarity, active);
}

hal_status_t hal_gpio_init(const hal_gpio_config_t *config)
{
    gpio_config_t io_conf = {0};
    esp_err_t err;

    if (config == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_gpio_esp_is_valid_pin(config->pin)) {
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

    if (s_pins[config->pin].initialized) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }

    /* Input-only pads (GPIO 34..39 on the classic ESP32) have no output
     * driver and no internal pull resistors; reject both before the driver
     * call so the portable result reports the unsupported combination. */
    if ((config->output || config->pull != HAL_GPIO_PULL_NONE) &&
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)config->pin)) {
        return HAL_ERR_NOT_SUPPORTED;
    }

    io_conf.pin_bit_mask = (1ULL << config->pin);
    /* Output pins use GPIO_MODE_INPUT_OUTPUT instead of GPIO_MODE_OUTPUT so
     * gpio_get_level() can read back the driven output level: ESP-IDF
     * documents that reads return 0 on pads whose input path is disabled.
     * GPIO_MODE_INPUT_OUTPUT retains output-driver semantics while enabling
     * the read-back path required by the public hal_gpio_read() contract. */
    io_conf.mode = config->output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT;
    io_conf.pull_up_en =
        (config->pull == HAL_GPIO_PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en =
        (config->pull == HAL_GPIO_PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
    /* Disable the SoC input hysteresis filter explicitly: the portable HAL
     * has no hysteresis concept, and targets that support it would otherwise
     * consume an indeterminate value from the uninitialized struct. */
    io_conf.hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE;
#endif

    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return hal_gpio_esp_map_error(err);
    }

    if (config->output) {
        /* Drive the pin to the logical INACTIVE state immediately so the
         * electrical level after init is well defined.  Use the requested
         * polarity directly because the software state is committed only
         * after every hardware operation succeeds. */
        const bool inactive = false;
        err = gpio_set_level(
            (gpio_num_t)config->pin,
            hal_gpio_esp_physical_level_for_polarity(config->polarity, inactive));
        if (err != ESP_OK) {
            /* gpio_config() succeeded, but initialization did not.  Return
             * the pin to its default state so a retry does not inherit a
             * partially configured resource. */
            (void)gpio_reset_pin((gpio_num_t)config->pin);
            return hal_gpio_esp_map_error(err);
        }
    }

    s_pins[config->pin].initialized = true;
    s_pins[config->pin].output = config->output;
    s_pins[config->pin].polarity = config->polarity;

    return HAL_OK;
}

hal_status_t hal_gpio_write(hal_pin_t pin_id, bool active)
{
    esp_err_t err;

    if (!hal_gpio_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    if (!s_pins[pin_id].initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }
    if (!s_pins[pin_id].output) {
        return HAL_ERR_NOT_SUPPORTED;
    }

    err = gpio_set_level((gpio_num_t)pin_id,
                         hal_gpio_esp_physical_level(pin_id, active));
    if (err != ESP_OK) {
        return hal_gpio_esp_map_error(err);
    }

    return HAL_OK;
}

hal_status_t hal_gpio_read(hal_pin_t pin_id, bool *active)
{
    if (active == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_gpio_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    if (!s_pins[pin_id].initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *active = (gpio_get_level((gpio_num_t)pin_id) != 0) !=
              (s_pins[pin_id].polarity == HAL_POLARITY_ACTIVE_LOW);

    return HAL_OK;
}

hal_status_t hal_gpio_deinit(hal_pin_t pin_id)
{
    esp_err_t err;

    if (!hal_gpio_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    if (!s_pins[pin_id].initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    /* Reset the pad to its default (power-on) state and free the pin.  The
     * software record is cleared regardless of the driver result so a
     * failed pad reset never leaves the allocator stuck. */
    err = gpio_reset_pin((gpio_num_t)pin_id);

    s_pins[pin_id].initialized = false;
    s_pins[pin_id].output = false;
    s_pins[pin_id].polarity = HAL_POLARITY_ACTIVE_HIGH;

    return hal_gpio_esp_map_error(err);
}
