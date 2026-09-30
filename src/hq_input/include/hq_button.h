#ifndef HQ_BUTTON_H
#define HQ_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_types.h"

#define HQ_BUTTON_INITIALIZER {0}

typedef enum hq_button_event {
    HQ_BUTTON_EVENT_PRESSED = 0,
    HQ_BUTTON_EVENT_RELEASED,
    HQ_BUTTON_EVENT_LONG_PRESS
} hq_button_event_t;

typedef void (*hq_button_event_fn_t)(hq_button_event_t event, void *context);

typedef struct hq_button_config {
    hal_pin_t pin;
    hal_polarity_t polarity;
    uint32_t debounce_ms;
    uint32_t long_press_ms;
    hq_button_event_fn_t on_event;
    void *context;
} hq_button_config_t;

typedef struct hq_button {
    bool initialized;
    bool sample_initialized;
    bool raw_pressed;
    bool pressed;
    bool long_press_reported;
    uint32_t raw_changed_at_ms;
    uint32_t pressed_at_ms;
    hq_button_config_t config;
} hq_button_t;

hal_status_t hq_button_init(hq_button_t *button, const hq_button_config_t *config);
void hq_button_poll(hq_button_t *button, uint32_t now_ms);
void hq_button_deinit(hq_button_t *button);

#endif