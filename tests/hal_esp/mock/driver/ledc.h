/*
 * Test double for ESP-IDF driver/ledc.h (TASK-009)
 *
 * Host-side mock of the legacy ESP-IDF LEDC driver API surface used by the
 * ESP PWM backend.  The declarations mirror the real header, including the
 * ESP-IDF-version dependent structure members:
 *
 *   - ledc_timer_config_t.deconfigure  (ESP-IDF >= 5.2)
 *   - ledc_channel_config_t.sleep_mode (ESP-IDF >= 5.4)
 *
 * The simulated IDF version is selected by HAL_PWM_MOCK_IDF_MAJOR / MINOR /
 * PATCH (see mock/esp_idf_version.h), so the backend can be compiled and
 * tested against every supported ESP-IDF release line.  The runtime behavior
 * of the modelled driver (context creation, timer configuration, channel
 * attachment, duty/stop semantics, GPIO reset) is implemented in
 * ledc_mock.c.
 */

#ifndef MOCK_DRIVER_LEDC_H
#define MOCK_DRIVER_LEDC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "soc/soc_caps.h"

#ifndef HAL_PWM_MOCK_IDF_MAJOR
#define HAL_PWM_MOCK_IDF_MAJOR 5
#endif
#ifndef HAL_PWM_MOCK_IDF_MINOR
#define HAL_PWM_MOCK_IDF_MINOR 5
#endif

#define MOCK_LEDC_IDF_VERSION_GE(major, minor) \
    ((HAL_PWM_MOCK_IDF_MAJOR > (major)) || \
     (HAL_PWM_MOCK_IDF_MAJOR == (major) && HAL_PWM_MOCK_IDF_MINOR >= (minor)))

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEDC_LOW_SPEED_MODE, /*!< LEDC low speed speed_mode */
    LEDC_SPEED_MODE_MAX
} ledc_mode_t;

typedef enum {
    LEDC_TIMER_0 = 0, /*!< LEDC timer 0 */
    LEDC_TIMER_1,     /*!< LEDC timer 1 */
    LEDC_TIMER_2,     /*!< LEDC timer 2 */
    LEDC_TIMER_3,     /*!< LEDC timer 3 */
    LEDC_TIMER_MAX
} ledc_timer_t;

typedef enum {
    LEDC_CHANNEL_0 = 0, /*!< LEDC channel 0 */
    LEDC_CHANNEL_1,     /*!< LEDC channel 1 */
    LEDC_CHANNEL_2,     /*!< LEDC channel 2 */
    LEDC_CHANNEL_3,     /*!< LEDC channel 3 */
    LEDC_CHANNEL_4,     /*!< LEDC channel 4 */
    LEDC_CHANNEL_5,     /*!< LEDC channel 5 */
    LEDC_CHANNEL_6,     /*!< LEDC channel 6 */
    LEDC_CHANNEL_7,     /*!< LEDC channel 7 */
    LEDC_CHANNEL_MAX
} ledc_channel_t;

typedef enum {
    LEDC_INTR_DISABLE = 0, /*!< Disable LEDC interrupt */
    LEDC_INTR_FADE_END,    /*!< Enable LEDC interrupt */
    LEDC_INTR_MAX
} ledc_intr_type_t;

typedef enum {
    LEDC_TIMER_1_BIT = 1, /*!< LEDC PWM duty resolution of 1 bits */
    LEDC_TIMER_2_BIT,     /*!< LEDC PWM duty resolution of 2 bits */
    LEDC_TIMER_3_BIT,     /*!< LEDC PWM duty resolution of 3 bits */
    LEDC_TIMER_4_BIT,     /*!< LEDC PWM duty resolution of 4 bits */
    LEDC_TIMER_5_BIT,     /*!< LEDC PWM duty resolution of 5 bits */
    LEDC_TIMER_6_BIT,     /*!< LEDC PWM duty resolution of 6 bits */
    LEDC_TIMER_7_BIT,     /*!< LEDC PWM duty resolution of 7 bits */
    LEDC_TIMER_8_BIT,     /*!< LEDC PWM duty resolution of 8 bits */
    LEDC_TIMER_9_BIT,     /*!< LEDC PWM duty resolution of 9 bits */
    LEDC_TIMER_10_BIT,    /*!< LEDC PWM duty resolution of 10 bits */
    LEDC_TIMER_11_BIT,    /*!< LEDC PWM duty resolution of 11 bits */
    LEDC_TIMER_12_BIT,    /*!< LEDC PWM duty resolution of 12 bits */
    LEDC_TIMER_13_BIT,    /*!< LEDC PWM duty resolution of 13 bits */
    LEDC_TIMER_14_BIT,    /*!< LEDC PWM duty resolution of 14 bits */
    LEDC_TIMER_15_BIT,    /*!< LEDC PWM duty resolution of 15 bits */
    LEDC_TIMER_16_BIT,    /*!< LEDC PWM duty resolution of 16 bits */
    LEDC_TIMER_17_BIT,    /*!< LEDC PWM duty resolution of 17 bits */
    LEDC_TIMER_18_BIT,    /*!< LEDC PWM duty resolution of 18 bits */
    LEDC_TIMER_19_BIT,    /*!< LEDC PWM duty resolution of 19 bits */
    LEDC_TIMER_20_BIT,    /*!< LEDC PWM duty resolution of 20 bits */
    LEDC_TIMER_BIT_MAX
} ledc_timer_bit_t;

typedef enum {
    LEDC_REF_TICK = 0, /*!< LEDC timer clock divided from reference tick (1MHz) */
    LEDC_APB_CLK,      /*!< LEDC timer clock divided from APB clock */
    LEDC_AUTO_CLK      /*!< Select LEDC source clock automatically */
} ledc_clk_cfg_t;

#if MOCK_LEDC_IDF_VERSION_GE(5, 4)
/**
 * Strategies to be applied to the LEDC channel during system Light-sleep.
 */
typedef enum {
    LEDC_SLEEP_MODE_NO_ALIVE_NO_PD = 0,
    LEDC_SLEEP_MODE_NO_ALIVE_ALLOW_PD,
    LEDC_SLEEP_MODE_KEEP_ALIVE,
    LEDC_SLEEP_MODE_INVALID
} ledc_sleep_mode_t;
#endif

/**
 * Configuration parameters of LEDC channel for ledc_channel_config().
 */
typedef struct {
    int gpio_num;                   /*!< the LEDC output gpio_num */
    ledc_mode_t speed_mode;         /*!< LEDC speed speed_mode */
    ledc_channel_t channel;         /*!< LEDC channel (0 - LEDC_CHANNEL_MAX-1) */
    ledc_intr_type_t intr_type;     /*!< configure interrupt */
    ledc_timer_t timer_sel;         /*!< Select the timer source of channel */
    uint32_t duty;                  /*!< LEDC channel duty, [0, (2**duty_resolution)] */
    int hpoint;                     /*!< LEDC channel hpoint value */
#if MOCK_LEDC_IDF_VERSION_GE(5, 4)
    ledc_sleep_mode_t sleep_mode;   /*!< Light-sleep strategy for the channel */
#endif
    struct {
        unsigned int output_invert: 1; /*!< Enable (1) or disable (0) gpio output invert */
    } flags;                        /*!< LEDC flags */
} ledc_channel_config_t;

/**
 * Configuration parameters of LEDC timer for ledc_timer_config().
 */
typedef struct {
    ledc_mode_t speed_mode;                /*!< LEDC speed speed_mode */
    ledc_timer_bit_t duty_resolution;      /*!< LEDC channel duty resolution */
    ledc_timer_t timer_num;                /*!< The timer source of channel */
    uint32_t freq_hz;                      /*!< LEDC timer frequency (Hz) */
    ledc_clk_cfg_t clk_cfg;                /*!< Configure LEDC source clock */
#if MOCK_LEDC_IDF_VERSION_GE(5, 2)
    bool deconfigure;                      /*!< De-configure a previously configured timer */
#endif
} ledc_timer_config_t;

esp_err_t ledc_timer_config(const ledc_timer_config_t *timer_conf);
esp_err_t ledc_timer_pause(ledc_mode_t speed_mode, ledc_timer_t timer_sel);
esp_err_t ledc_channel_config(const ledc_channel_config_t *ledc_conf);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel,
                        uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel,
                    uint32_t idle_level);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_DRIVER_LEDC_H */