#ifndef HQ_FACTORY_RESET_H
#define HQ_FACTORY_RESET_H

#include <stdbool.h>
#include <stdint.h>

#include "hq_button.h"

#define HQ_FACTORY_RESET_INITIALIZER {0}

typedef enum hq_factory_reset_status {
    HQ_FACTORY_RESET_OK = 0,
    HQ_FACTORY_RESET_ERR_INVALID_ARGUMENT = -1,
    HQ_FACTORY_RESET_ERR_ALREADY_INITIALIZED = -2,
    HQ_FACTORY_RESET_ERR_GPIO = -3
} hq_factory_reset_status_t;

typedef bool (*hq_factory_reset_erase_fn_t)(void *context);
typedef void (*hq_factory_reset_indicator_fn_t)(bool active, void *context);
typedef void (*hq_factory_reset_restart_fn_t)(void *context);
typedef bool (*hq_factory_reset_enabled_fn_t)(void *context);

typedef struct hq_factory_reset_config {
    hal_pin_t pin;
    hal_polarity_t polarity;
    uint32_t hold_ms;
    uint32_t debounce_ms;
    hq_factory_reset_erase_fn_t erase;
    hq_factory_reset_indicator_fn_t indicator;
    hq_factory_reset_restart_fn_t restart;
    hq_factory_reset_enabled_fn_t is_enabled;
    void *context;
} hq_factory_reset_config_t;

typedef struct hq_factory_reset {
    bool initialized;
    bool triggered;
    bool wait_release;
    bool indicator_active;
    hq_button_t button;
    hq_factory_reset_config_t config;
} hq_factory_reset_t;

hq_factory_reset_status_t hq_factory_reset_init(
    hq_factory_reset_t *service,
    const hq_factory_reset_config_t *config);
void hq_factory_reset_poll(hq_factory_reset_t *service, uint32_t now_ms);
void hq_factory_reset_deinit(hq_factory_reset_t *service);
bool hq_factory_reset_is_triggered(const hq_factory_reset_t *service);

#endif