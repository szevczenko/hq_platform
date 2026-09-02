/*
 * LEDC driver test double - inspection API (TASK-009)
 *
 * Host-side mock of the ESP-IDF legacy LEDC driver used to exercise the ESP
 * PWM backend (src/hal/esp/hal_pwm_esp.c) without ESP hardware.  The mock
 * models the driver behavior documented for ESP-IDF 5.x, including the
 * important 5.5 lifecycle detail that ledc_timer_config() creates the
 * speed-mode driver context before validating the frequency divisor: an
 * unsupported frequency returns ESP_FAIL while leaving only a driver context
 * and no configured timer (so ledc_timer_pause() succeeds simply because the
 * context exists while deconfiguration returns ESP_ERR_INVALID_STATE).
 *
 * The backend is compiled against the mock headers in mock/; this header
 * exposes the mock's internal state so the unit tests can assert the exact
 * LEDC calls the backend makes (channel attachment, duty ticks, idle levels,
 * timer configuration/deconfiguration, GPIO reset).
 */

#ifndef LEDC_MOCK_H
#define LEDC_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- failure-injection hooks ------------------------------------------ */

/** Reset all mocked driver state and clear every failure hook. */
void ledc_mock_reset(void);

/**
 * @brief Select which frequencies ledc_timer_config() accepts.
 *
 * @param is_supported  Predicate returning true for frequencies the mock can
 *                      produce at 13-bit resolution; NULL means every
 *                      non-zero frequency is supported.
 */
void ledc_mock_set_freq_supported(bool (*is_supported)(uint32_t freq_hz));

/**
 * @brief Make speed-mode context creation fail.
 *
 * @param err  Non-OK esp_err_t returned by the next context creation, or
 *             ESP_OK to clear the hook.
 */
void ledc_mock_set_ctx_alloc_fail(esp_err_t err);

/**
 * @brief Make ledc_channel_config() fail AFTER attaching the channel/Gpio.
 *
 * Models the real driver touching the channel before it can report an error,
 * so the backend's rollback path is exercised.
 *
 * @param err  Non-OK esp_err_t returned after attach, or ESP_OK to clear.
 */
void ledc_mock_set_channel_config_fail_after_attach(esp_err_t err);

/**
 * @brief Make gpio_reset_pin() fail.
 *
 * @param err  Non-OK esp_err_t returned by the next gpio_reset_pin(), or
 *             ESP_OK to clear.
 */
void ledc_mock_set_gpio_reset_fail(esp_err_t err);

/* --- state inspection -------------------------------------------------- */

/** Whether the speed-mode driver context exists (created by any call). */
bool ledc_mock_ctx_exists(void);

bool ledc_mock_timer_configured(ledc_timer_t timer);
bool ledc_mock_timer_paused(ledc_timer_t timer);
uint32_t ledc_mock_timer_freq(ledc_timer_t timer);

bool ledc_mock_channel_attached(ledc_channel_t channel);
int ledc_mock_channel_gpio(ledc_channel_t channel);
ledc_timer_t ledc_mock_channel_timer(ledc_channel_t channel);
uint32_t ledc_mock_channel_duty(ledc_channel_t channel);
bool ledc_mock_channel_sig_en(ledc_channel_t channel);
uint32_t ledc_mock_channel_idle(ledc_channel_t channel);
bool ledc_mock_channel_invert(ledc_channel_t channel);

/* Search a channel attached to `gpio`; returns LEDC_CHANNEL_MAX if none. */
ledc_channel_t ledc_mock_find_channel_by_gpio(int gpio);

#ifdef __cplusplus
}
#endif

#endif /* LEDC_MOCK_H */