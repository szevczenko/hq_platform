#include "hq_button.h"

#include <string.h>

#include "hal_gpio.h"

static uint32_t elapsed_ms(uint32_t now_ms, uint32_t since_ms)
{
    return (uint32_t)(now_ms - since_ms);
}

static void emit_event(hq_button_t *button, hq_button_event_t event)
{
    if (button->config.on_event != NULL) {
        button->config.on_event(event, button->config.context);
    }
}

hal_status_t hq_button_init(hq_button_t *button, const hq_button_config_t *config)
{
    hal_gpio_config_t gpio_config;

    if (button == NULL || config == NULL || config->pin == HAL_PIN_NONE) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (button->initialized) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }

    gpio_config.pin = config->pin;
    gpio_config.polarity = config->polarity;
    gpio_config.pull = config->polarity == HAL_POLARITY_ACTIVE_LOW
                           ? HAL_GPIO_PULL_UP
                           : HAL_GPIO_PULL_DOWN;
    gpio_config.output = false;
    if (hal_gpio_init(&gpio_config) != HAL_OK) {
        return HAL_ERR_INTERNAL;
    }

    memset(button, 0, sizeof(*button));
    button->config = *config;
    button->initialized = true;
    return HAL_OK;
}

void hq_button_poll(hq_button_t *button, uint32_t now_ms)
{
    bool raw_pressed;

    if (button == NULL || !button->initialized ||
        hal_gpio_read(button->config.pin, &raw_pressed) != HAL_OK) {
        return;
    }

    if (!button->sample_initialized || raw_pressed != button->raw_pressed) {
        button->sample_initialized = true;
        button->raw_pressed = raw_pressed;
        button->raw_changed_at_ms = now_ms;
    }

    if (button->raw_pressed != button->pressed &&
        elapsed_ms(now_ms, button->raw_changed_at_ms) >= button->config.debounce_ms) {
        button->pressed = button->raw_pressed;
        if (button->pressed) {
            button->pressed_at_ms = button->raw_changed_at_ms + button->config.debounce_ms;
            button->long_press_reported = false;
            emit_event(button, HQ_BUTTON_EVENT_PRESSED);
        } else {
            button->long_press_reported = false;
            emit_event(button, HQ_BUTTON_EVENT_RELEASED);
        }
    }

    if (button->pressed && !button->long_press_reported &&
        button->config.long_press_ms != 0u &&
        elapsed_ms(now_ms, button->pressed_at_ms) >= button->config.long_press_ms) {
        button->long_press_reported = true;
        emit_event(button, HQ_BUTTON_EVENT_LONG_PRESS);
    }
}

void hq_button_deinit(hq_button_t *button)
{
    if (button == NULL || !button->initialized) {
        return;
    }
    (void)hal_gpio_deinit(button->config.pin);
    memset(button, 0, sizeof(*button));
}