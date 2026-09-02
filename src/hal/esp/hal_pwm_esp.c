/**
 * @file hal_pwm_esp.c
 * @brief ESP-IDF PWM HAL backend (TASK-009)
 *
 * Real PWM backend for ESP-IDF builds.  It drives the ESP32-family LEDC
 * peripheral through the legacy `driver/ledc.h` API (available on every
 * supported ESP SoC: ESP32, ESP32-S3 and ESP32-C6) and implements the
 * portable contract from hal_pwm.h:
 *
 *   - one LEDC channel is allocated per initialized pin; channels share an
 *     LEDC timer only while the requested frequency is identical (the duty
 *     resolution, speed mode and clock source are fixed for the whole
 *     backend), so a new pin never changes the frequency of an
 *     already-initialized pin,
 *   - the duty cycle is normalized percent and is translated linearly to
 *     LEDC duty ticks of the fixed 13-bit resolution: 0.0% maps to 0 ticks,
 *     50.0% to exactly half of the period and 100.0% to a permanently
 *     active output,
 *   - the active polarity is applied through the LEDC pad output inversion
 *     (output_invert): with HAL_POLARITY_ACTIVE_LOW the duty portion of
 *     every period is physical LOW, matching the portable "active"
 *     semantics for all duty cycles,
 *   - the output starts in the logical INACTIVE state (duty 0.0%),
 *   - hal_pwm_force_inactive() halts the channel at the logical INACTIVE
 *     level and retains the last duty; a later hal_pwm_set_duty() resumes
 *     generation,
 *   - init/deinit are not idempotent and return the documented errors,
 *   - resource allocation is failure-atomic: no timer or channel is marked
 *     owned until every hardware configuration step has succeeded, and a
 *     failed init rolls back any provisionally configured timer,
 *   - every ESP-IDF esp_err_t result is mapped to the portable hal_status_t
 *     result type.
 *
 * The public HAL headers themselves are pure C99 and include no ESP-IDF
 * headers; all target-specific types (LEDC timers, channels and
 * resolutions) stay inside this backend source.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "hal_pwm.h"

/* Every supported ESP target (ESP32, ESP32-S3, ESP32-C6) provides the LEDC
 * peripheral; this backend is only compiled for targets where it exists. */
#if !SOC_LEDC_SUPPORTED
#error "hal_pwm_esp.c requires a target with the LEDC peripheral"
#endif

#define HAL_PWM_ESP_CHANNEL_COUNT  LEDC_CHANNEL_MAX
#define HAL_PWM_ESP_TIMER_COUNT    LEDC_TIMER_MAX

/* Fixed 13-bit duty resolution.  The LEDC timer bit width is at least 14 on
 * every supported SoC (ESP32: 20, ESP32-S3: 14, ESP32-C6: 20), so the same
 * resolution works everywhere and keeps timer sharing simple.  Duty values
 * are scaled linearly over the 2**13 ticks of one period.  The full-period
 * endpoint 2**13 is deliberately not programmed for normal PWM operation:
 * 100% duty is instead represented by stopping the channel at its logical
 * ACTIVE level, which sidesteps per-target behavior at the resolution
 * boundary. */
#define HAL_PWM_ESP_DUTY_RESOLUTION LEDC_TIMER_13_BIT
#define HAL_PWM_ESP_DUTY_BITS        13U
#define HAL_PWM_ESP_DUTY_PERIOD      (1U << HAL_PWM_ESP_DUTY_BITS)

/* Low-speed mode is available on every ESP32-family target that has LEDC;
 * high-speed mode exists only on the classic ESP32, so the backend always
 * uses the low-speed group for uniform behavior across targets. */
#define HAL_PWM_ESP_SPEED_MODE LEDC_LOW_SPEED_MODE

typedef struct hal_pwm_esp_channel {
    bool in_use;            /**< Resource is allocated to an initialized pin. */
    hal_pin_t pin;          /**< Portable pin identifier using this channel. */
    hal_polarity_t polarity;/**< Active polarity of the output. */
    bool output_invert;     /**< LEDC pad output inversion (from polarity). */
    bool stopped;           /**< Output is held at an idle level by ledc_stop(). */
    ledc_channel_t channel; /**< Allocated LEDC channel. */
    ledc_timer_t timer;     /**< Allocated LEDC timer. */
} hal_pwm_esp_channel_t;

/* One record per LEDC timer.  A timer may be shared by several channels,
 * but only while the configured frequency stays the same; the duty
 * resolution and speed mode are fixed for the whole backend, so the
 * frequency is the only parameter that must match for sharing. */
typedef struct hal_pwm_esp_timer {
    bool in_use;            /**< Timer is allocated to at least one channel. */
    uint32_t frequency_hz;  /**< Frequency the timer is configured to. */
} hal_pwm_esp_timer_t;

static hal_pwm_esp_channel_t s_channels[HAL_PWM_ESP_CHANNEL_COUNT];
static hal_pwm_esp_timer_t s_timers[HAL_PWM_ESP_TIMER_COUNT];

static bool hal_pwm_esp_is_valid_pin(hal_pin_t pin)
{
    /* The < GPIO_NUM_MAX bound must be checked before the SoC mask macro,
     * whose shift operand is only defined for the numbered pads. */
    return (pin != HAL_PIN_NONE) && (pin < GPIO_NUM_MAX) &&
           GPIO_IS_VALID_GPIO((gpio_num_t)pin);
}

static bool hal_pwm_esp_is_valid_duty(float duty_percent)
{
    /* NaN fails both comparisons; +-inf fail the upper/lower bound. */
    return (duty_percent >= HAL_PWM_DUTY_MIN_PERCENT) &&
           (duty_percent <= HAL_PWM_DUTY_MAX_PERCENT);
}

static hal_pwm_esp_channel_t *hal_pwm_esp_find_channel(hal_pin_t pin)
{
    size_t i;

    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (s_channels[i].in_use && s_channels[i].pin == pin) {
            return &s_channels[i];
        }
    }

    return NULL;
}

static uint32_t hal_pwm_esp_inactive_level(const hal_pwm_esp_channel_t *ch)
{
    /* The pad output level equals the LEDC idle level XOR the channel's
     * output inversion (set from the configured polarity):
     *
     *   pad = idle ^ output_invert
     *
     * Logical INACTIVE maps to physical LOW (0) for active-high and to
     * physical HIGH (1) for active-low, so solving idle ^ invert = physical
     * yields idle = 0 in both polarity configurations.  The mapping is kept
     * explicit so the portable polarity contract stays visible here. */
    const uint32_t inactive_physical =
        (ch->polarity == HAL_POLARITY_ACTIVE_LOW) ? 1U : 0U;
    const uint32_t invert = ch->output_invert ? 1U : 0U;

    return inactive_physical ^ invert;
}

static uint32_t hal_pwm_esp_active_level(const hal_pwm_esp_channel_t *ch)
{
    const uint32_t active_physical =
        (ch->polarity == HAL_POLARITY_ACTIVE_LOW) ? 0U : 1U;
    const uint32_t invert = ch->output_invert ? 1U : 0U;

    /* ledc_stop() receives the level before the LEDC output inversion. */
    return active_physical ^ invert;
}

static uint32_t hal_pwm_esp_duty_to_ticks(float duty_percent)
{
    /* Linear percent -> ticks over the 2**13 period: 0.0% maps to 0 ticks
     * and 50.0% to exactly half of the period.  This helper is only used
     * for duty below 100%, so the result stays below 2**13 (the
     * full-period endpoint is handled by hal_pwm_set_duty() instead). */
    return (uint32_t)((duty_percent / HAL_PWM_DUTY_MAX_PERCENT) *
                      (float)HAL_PWM_ESP_DUTY_PERIOD);
}

hal_status_t hal_pwm_init(const hal_pwm_config_t *config)
{
    hal_pwm_esp_channel_t *ch = NULL;
    ledc_timer_config_t timer_conf;
    ledc_channel_config_t channel_conf;
    ledc_channel_t channel_num = LEDC_CHANNEL_0;
    ledc_timer_t timer_num = LEDC_TIMER_0;
    bool timer_reuse = false;  /**< Reuse an already-configured compatible timer. */
    bool timer_fresh = false;  /**< Configure a fresh (previously free) timer. */
    esp_err_t err;
    size_t i;

    if (config == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->frequency_hz == 0U) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->polarity != HAL_POLARITY_ACTIVE_HIGH &&
        config->polarity != HAL_POLARITY_ACTIVE_LOW) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!hal_pwm_esp_is_valid_pin(config->pin)) {
        return HAL_ERR_INVALID_PIN;
    }
    /* Input-only pads (for example GPIO 34..39 on the classic ESP32) have
     * no output driver and cannot produce a PWM waveform.  Reject them
     * before touching any timer/channel resource so a failed init never
     * needs a hardware rollback and the portable result reports the
     * unsupported configuration. */
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)config->pin)) {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (hal_pwm_esp_find_channel(config->pin) != NULL) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }

    /* Allocate a free LEDC channel. */
    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (!s_channels[i].in_use) {
            ch = &s_channels[i];
            break;
        }
    }
    if (ch == NULL) {
        return HAL_ERR_NO_RESOURCE;
    }
    channel_num = (ledc_channel_t)i;

    /* Choose the LEDC timer for the channel.  A timer may be shared by
     * several channels, but only when the frequency (and the fixed duty
     * resolution/speed mode) match — reconfiguring a timer would change the
     * frequency of every pin already using it.  Prefer reusing a compatible
     * timer; otherwise configure a free one; if neither exists, report
     * resource exhaustion. */
    for (i = 0U; i < HAL_PWM_ESP_TIMER_COUNT; ++i) {
        if (s_timers[i].in_use) {
            if (s_timers[i].frequency_hz == config->frequency_hz) {
                timer_num = (ledc_timer_t)i;
                timer_reuse = true;
                timer_fresh = false;  /* reuse wins over a remembered free timer */
                break;
            }
            continue;
        }
        if (!timer_fresh) {
            timer_num = (ledc_timer_t)i;
            timer_fresh = true;
        }
    }
    if (!timer_reuse && !timer_fresh) {
        return HAL_ERR_NO_RESOURCE;
    }

    if (timer_fresh) {
        /* Only freshly allocated timers are reconfigured, so an existing
         * pin's frequency is never clobbered. */
        timer_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
        timer_conf.duty_resolution = HAL_PWM_ESP_DUTY_RESOLUTION;
        timer_conf.timer_num = timer_num;
        timer_conf.freq_hz = config->frequency_hz;
        timer_conf.clk_cfg = LEDC_AUTO_CLK;
        timer_conf.deconfigure = false;

        err = ledc_timer_config(&timer_conf);
        if (err != ESP_OK) {
            /* ESP_FAIL means no clock source/divider produces the requested
             * frequency at the fixed duty resolution: the portable result
             * is HAL_ERR_NOT_SUPPORTED.  Everything else is unexpected
             * because every argument was pre-validated. */
            if (err == ESP_FAIL) {
                return HAL_ERR_NOT_SUPPORTED;
            }
            if (err == ESP_ERR_INVALID_ARG) {
                return HAL_ERR_INVALID_ARGUMENT;
            }
            return HAL_ERR_INTERNAL;
        }
    }

    /* Timer ownership is provisional until channel configuration succeeds.
     * In particular, do not mark a fresh timer in use before the operation
     * below: a failed channel configuration must leave the allocator able to
     * reuse the timer. */
    channel_conf.gpio_num = (int)config->pin;
    channel_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
    channel_conf.channel = channel_num;
    channel_conf.intr_type = LEDC_INTR_DISABLE;
    channel_conf.timer_sel = timer_num;
    channel_conf.duty = 0U; /* output starts at logical INACTIVE (duty 0.0%) */
    channel_conf.hpoint = 0;
    channel_conf.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
    /* Active polarity is applied through the pad output inversion: with
     * HAL_POLARITY_ACTIVE_LOW the duty portion of every period becomes
     * physical LOW, exactly as the portable contract defines. */
    channel_conf.flags.output_invert =
        (config->polarity == HAL_POLARITY_ACTIVE_LOW) ? 1 : 0;

    err = ledc_channel_config(&channel_conf);
    if (err != ESP_OK) {
        if (timer_fresh) {
            /* Deconfiguring a timer requires it to be paused first, so pause
             * then deconfigure the provisionally created timer.  Cleanup
             * errors are ignored because the channel configuration error is
             * the API failure; importantly, the software allocator is rolled
             * back either way, so no resource leaks and a later
             * hal_pwm_init() may reconfigure the same timer. */
            (void)ledc_timer_pause(HAL_PWM_ESP_SPEED_MODE, timer_num);
            timer_conf.deconfigure = true;
            (void)ledc_timer_config(&timer_conf);
            s_timers[timer_num].in_use = false;
            s_timers[timer_num].frequency_hz = 0U;
        }
        return HAL_ERR_INTERNAL;
    }

    /* Commit timer ownership only after the channel has been configured. */
    s_timers[timer_num].in_use = true;
    s_timers[timer_num].frequency_hz = config->frequency_hz;

    ch->in_use = true;
    ch->pin = config->pin;
    ch->polarity = config->polarity;
    ch->output_invert =
        (config->polarity == HAL_POLARITY_ACTIVE_LOW);
    ch->stopped = false;
    ch->channel = channel_num;
    ch->timer = timer_num;

    return HAL_OK;
}

hal_status_t hal_pwm_set_duty(hal_pin_t pin_id, float duty_percent)
{
    hal_pwm_esp_channel_t *ch;
    esp_err_t err;

    if (!hal_pwm_esp_is_valid_duty(duty_percent)) {
        return HAL_ERR_OUT_OF_RANGE;
    }
    if (!hal_pwm_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    ch = hal_pwm_esp_find_channel(pin_id);
    if (ch == NULL) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    if (duty_percent == HAL_PWM_DUTY_MAX_PERCENT) {
        /* The LEDC maximum-resolution endpoint is unsafe on several ESP
         * targets: duty == 2**resolution can overflow the hardware counter.
         * Stop the channel and hold the pad at logical ACTIVE instead. */
        err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, ch->channel,
                        hal_pwm_esp_active_level(ch));
        if (err != ESP_OK) {
            return HAL_ERR_INTERNAL;
        }
        ch->stopped = true;
        return HAL_OK;
    }

    err = ledc_set_duty(HAL_PWM_ESP_SPEED_MODE, ch->channel,
                        hal_pwm_esp_duty_to_ticks(duty_percent));
    if (err != ESP_OK) {
        return HAL_ERR_INTERNAL;
    }
    /* ledc_update_duty() also re-enables a channel previously stopped by
     * hal_pwm_force_inactive() or the 100% endpoint handling. */
    err = ledc_update_duty(HAL_PWM_ESP_SPEED_MODE, ch->channel);
    if (err != ESP_OK) {
        return HAL_ERR_INTERNAL;
    }
    ch->stopped = false;

    return HAL_OK;
}

hal_status_t hal_pwm_force_inactive(hal_pin_t pin_id)
{
    hal_pwm_esp_channel_t *ch;
    esp_err_t err;

    if (!hal_pwm_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    ch = hal_pwm_esp_find_channel(pin_id);
    if (ch == NULL) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    /* Halt the channel at the logical INACTIVE level; the last duty value
     * is retained by the hardware and by hal_pwm_set_duty(). */
    err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, ch->channel,
                    hal_pwm_esp_inactive_level(ch));
    if (err != ESP_OK) {
        return HAL_ERR_INTERNAL;
    }
    ch->stopped = true;

    return HAL_OK;
}

hal_status_t hal_pwm_deinit(hal_pin_t pin_id)
{
    hal_pwm_esp_channel_t *ch;
    ledc_timer_t timer_num;
    esp_err_t err;
    size_t i;

    if (!hal_pwm_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    ch = hal_pwm_esp_find_channel(pin_id);
    if (ch == NULL) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    /* Stop the channel at the logical INACTIVE level and release the pad. */
    err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, ch->channel,
                    hal_pwm_esp_inactive_level(ch));
    if (err != ESP_OK) {
        return HAL_ERR_INTERNAL;
    }
    gpio_reset_pin((gpio_num_t)ch->pin);

    timer_num = ch->timer;

    ch->in_use = false;
    ch->pin = HAL_PIN_NONE;
    ch->polarity = HAL_POLARITY_ACTIVE_HIGH;
    ch->output_invert = false;
    ch->stopped = false;
    ch->channel = LEDC_CHANNEL_0;
    ch->timer = LEDC_TIMER_0;

    /* Free the timer if no remaining channel still uses it, so a later
     * hal_pwm_init() with a different frequency can reconfigure it freely. */
    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (s_channels[i].in_use && s_channels[i].timer == timer_num) {
            return HAL_OK; /* timer is still shared */
        }
    }
    s_timers[timer_num].in_use = false;
    s_timers[timer_num].frequency_hz = 0U;

    return HAL_OK;
}