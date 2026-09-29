#include "hq_factory_reset.h"

#include <string.h>

static void set_indicator(hq_factory_reset_t *service, bool active)
{
    if (service->indicator_active == active) {
        return;
    }
    service->indicator_active = active;
    if (service->config.indicator != NULL) {
        service->config.indicator(active, service->config.context);
    }
}

static void on_button_event(hq_button_event_t event, void *context)
{
    hq_factory_reset_t *service = context;

    if (event == HQ_BUTTON_EVENT_PRESSED) {
        if (!service->wait_release && !service->triggered) {
            set_indicator(service, true);
        }
        return;
    }
    if (event == HQ_BUTTON_EVENT_RELEASED) {
        service->wait_release = false;
        set_indicator(service, false);
        return;
    }
    if (event != HQ_BUTTON_EVENT_LONG_PRESS || service->wait_release ||
        service->triggered) {
        return;
    }

    service->triggered = true;
    set_indicator(service, true);
    if (!service->config.erase(service->config.context)) {
        service->triggered = false;
        service->wait_release = true;
        set_indicator(service, false);
        return;
    }
    service->config.restart(service->config.context);
}

hq_factory_reset_status_t hq_factory_reset_init(
    hq_factory_reset_t *service,
    const hq_factory_reset_config_t *config)
{
    hq_button_config_t button_config;
    hal_status_t status;

    if (service == NULL || config == NULL || config->pin == HAL_PIN_NONE ||
        config->hold_ms == 0u || config->erase == NULL || config->restart == NULL) {
        return HQ_FACTORY_RESET_ERR_INVALID_ARGUMENT;
    }
    if (service->initialized) {
        return HQ_FACTORY_RESET_ERR_ALREADY_INITIALIZED;
    }

    memset(service, 0, sizeof(*service));
    service->config = *config;
    button_config.pin = config->pin;
    button_config.polarity = config->polarity;
    button_config.debounce_ms = config->debounce_ms;
    button_config.long_press_ms = config->hold_ms;
    button_config.on_event = on_button_event;
    button_config.context = service;

    status = hq_button_init(&service->button, &button_config);
    if (status != HAL_OK) {
        memset(service, 0, sizeof(*service));
        return status == HAL_ERR_INVALID_ARGUMENT
                   ? HQ_FACTORY_RESET_ERR_INVALID_ARGUMENT
                   : HQ_FACTORY_RESET_ERR_GPIO;
    }
    service->initialized = true;
    return HQ_FACTORY_RESET_OK;
}

void hq_factory_reset_poll(hq_factory_reset_t *service, uint32_t now_ms)
{
    if (service == NULL || !service->initialized || service->triggered ||
        (service->config.is_enabled != NULL &&
         !service->config.is_enabled(service->config.context))) {
        return;
    }
    hq_button_poll(&service->button, now_ms);
}

void hq_factory_reset_deinit(hq_factory_reset_t *service)
{
    if (service == NULL || !service->initialized) {
        return;
    }
    hq_button_deinit(&service->button);
    memset(service, 0, sizeof(*service));
}

bool hq_factory_reset_is_triggered(const hq_factory_reset_t *service)
{
    return service != NULL && service->triggered;
}