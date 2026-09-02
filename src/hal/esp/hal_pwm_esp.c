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
 *     owned until every hardware configuration step has succeeded.  A failed
 *     init retries rollback before returning; if a driver operation remains
 *     transiently unavailable, the provisional record is quarantined (reserved
 *     and not allocatable) and is retried before a later allocation,
 *   - deinit releases every resource: the channel is stopped, the pad is
 *     reset, and a timer that is no longer referenced by any channel is
 *     paused and deconfigured before its allocator record is cleared,
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
/* Driver failures are normally permanent argument/state errors, but retrying
 * teardown makes rollback safe in the presence of a transient IDF failure. */
#define HAL_PWM_ESP_CLEANUP_RETRIES 3U

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
    bool reserved;          /**< Resource is provisionally reserved by init. */
    bool cleanup_pending;   /**< Provisional hardware cleanup must be retried. */
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
    bool reserved;          /**< Timer is provisionally reserved by init. */
    bool cleanup_pending;   /**< Hardware teardown must be retried. */
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
        if (s_channels[i].in_use && !s_channels[i].cleanup_pending &&
            s_channels[i].pin == pin) {
            return &s_channels[i];
        }
    }

    return NULL;
}

static bool hal_pwm_esp_has_pending_channel_for_timer(ledc_timer_t timer_num)
{
    size_t i;

    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (s_channels[i].cleanup_pending &&
            s_channels[i].timer == timer_num) {
            return true;
        }
    }

    return false;
}

static hal_pwm_esp_channel_t *hal_pwm_esp_find_reserved_channel(hal_pin_t pin)
{
    size_t i;

    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if ((s_channels[i].in_use || s_channels[i].reserved ||
             s_channels[i].cleanup_pending) && s_channels[i].pin == pin) {
            return &s_channels[i];
        }
    }

    return NULL;
}

static void hal_pwm_esp_clear_channel_record(hal_pwm_esp_channel_t *ch)
{
    ch->in_use = false;
    ch->reserved = false;
    ch->cleanup_pending = false;
    ch->pin = HAL_PIN_NONE;
    ch->polarity = HAL_POLARITY_ACTIVE_HIGH;
    ch->output_invert = false;
    ch->stopped = false;
    ch->channel = LEDC_CHANNEL_0;
    ch->timer = LEDC_TIMER_0;
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

static hal_status_t hal_pwm_esp_map_err(esp_err_t err)
{
    /* Every LEDC/GPIO call below is made against pre-validated allocator
     * state, so in practice the driver only fails on internal or
     * environmental problems.  Map the error classes the LEDC driver
     * documents to the closest portable code and report everything else as
     * an unexpected backend failure.  In particular ESP_FAIL is what the
     * LEDC timer configuration returns when no clock source/divider can
     * produce the requested frequency at the selected duty resolution: the
     * portable result is HAL_ERR_NOT_SUPPORTED. */
    switch (err) {
    case ESP_OK:
        return HAL_OK;
    case ESP_ERR_INVALID_ARG:
        return HAL_ERR_INVALID_ARGUMENT;
    case ESP_ERR_NO_MEM:
    case ESP_ERR_NOT_FOUND:
        return HAL_ERR_NO_RESOURCE;
    case ESP_ERR_TIMEOUT:
        return HAL_ERR_BUSY;
    case ESP_ERR_NOT_SUPPORTED:
        return HAL_ERR_NOT_SUPPORTED;
    case ESP_FAIL:
        return HAL_ERR_NOT_SUPPORTED;
    default:
        return HAL_ERR_INTERNAL;
    }
}

/* A timer can only be deconfigured after it has been paused.  Keep this
 * operation in one helper so every failure path uses the same complete
 * rollback sequence.  A retry is useful for transient driver/clock errors;
 * importantly, no caller marks the timer free unless this helper succeeds. */
static esp_err_t hal_pwm_esp_cleanup_timer(ledc_timer_t timer_num,
                                            uint32_t frequency_hz)
{
    ledc_timer_config_t timer_conf;
    esp_err_t err = ESP_FAIL;
    esp_err_t attempt_err;
    unsigned int attempt;

    for (attempt = 0U; attempt < HAL_PWM_ESP_CLEANUP_RETRIES; ++attempt) {
        attempt_err = ledc_timer_pause(HAL_PWM_ESP_SPEED_MODE, timer_num);
        if (attempt_err == ESP_OK) {
            timer_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
            timer_conf.duty_resolution = HAL_PWM_ESP_DUTY_RESOLUTION;
            timer_conf.timer_num = timer_num;
            timer_conf.freq_hz = frequency_hz;
            timer_conf.clk_cfg = LEDC_AUTO_CLK;
            timer_conf.deconfigure = true;
            attempt_err = ledc_timer_config(&timer_conf);
            if (attempt_err == ESP_OK) {
                return ESP_OK;
            }
        }
        err = attempt_err;
    }

    return err;
}

/* Undo the channel side of ledc_channel_config().  The LEDC API has no
 * channel-delete operation; stopping it and disconnecting/resetting its GPIO
 * is the complete legacy-driver rollback.  Attempt both operations even when
 * the first one fails, so a failed init does not leave a pad connected. */
static esp_err_t hal_pwm_esp_cleanup_channel(hal_pin_t pin,
                                              hal_polarity_t polarity,
                                              ledc_channel_t channel_num)
{
    const uint32_t inactive_level =
        (polarity == HAL_POLARITY_ACTIVE_LOW ? 1U : 0U) ^
        ((polarity == HAL_POLARITY_ACTIVE_LOW) ? 1U : 0U);
    esp_err_t first_err = ESP_OK;
    esp_err_t last_err = ESP_FAIL;
    esp_err_t err;
    unsigned int attempt;

    for (attempt = 0U; attempt < HAL_PWM_ESP_CLEANUP_RETRIES; ++attempt) {
        err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, channel_num, inactive_level);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
        err = gpio_reset_pin((gpio_num_t)pin);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
        if (first_err == ESP_OK) {
            return ESP_OK;
        }
        last_err = first_err;
        /* A subsequent attempt is allowed to recover a transient failure.
         * Do not return early after the stop failure: GPIO cleanup is still
         * required. */
        first_err = ESP_OK;
    }

    return last_err;
}

/* Retry cleanup of hardware left behind by an earlier failed init.  The
 * provisional records remain reserved until every relevant driver operation
 * succeeds.  This is important for channels as well as timers: gpio_reset_pin
 * and ledc_stop can fail independently, and making a failed channel available
 * would allow a later init to reuse hardware that is still configured. */
static void hal_pwm_esp_retry_pending_cleanup(void)
{
    size_t i;

    /* Channels are cleaned first because a timer must not be deconfigured while
     * a provisional channel may still be connected to it. */
    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (s_channels[i].cleanup_pending &&
            hal_pwm_esp_cleanup_channel(s_channels[i].pin,
                                        s_channels[i].polarity,
                                        s_channels[i].channel) == ESP_OK) {
            hal_pwm_esp_clear_channel_record(&s_channels[i]);
        }
    }

    for (i = 0U; i < HAL_PWM_ESP_TIMER_COUNT; ++i) {
        if (s_timers[i].cleanup_pending &&
            !hal_pwm_esp_has_pending_channel_for_timer((ledc_timer_t)i) &&
            hal_pwm_esp_cleanup_timer((ledc_timer_t)i,
                                      s_timers[i].frequency_hz) == ESP_OK) {
            s_timers[i].in_use = false;
            s_timers[i].reserved = false;
            s_timers[i].cleanup_pending = false;
            s_timers[i].frequency_hz = 0U;
        }
    }
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
    bool timer_quarantined = false;
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

    /* Retry all pending rollback before selecting resources.  A provisional
     * channel/timer is not an available resource until this succeeds. */
    hal_pwm_esp_retry_pending_cleanup();
    {
        hal_pwm_esp_channel_t *reserved_ch =
            hal_pwm_esp_find_reserved_channel(config->pin);
        if (reserved_ch != NULL) {
            return reserved_ch->cleanup_pending ? HAL_ERR_BUSY
                                                : HAL_ERR_ALREADY_INITIALIZED;
        }
    }

    /* Allocate a free LEDC channel. */
    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (!s_channels[i].in_use && !s_channels[i].reserved &&
            !s_channels[i].cleanup_pending) {
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
     * resource exhaustion.  Quarantined timers are never considered free. */
    for (i = 0U; i < HAL_PWM_ESP_TIMER_COUNT; ++i) {
        if (s_timers[i].cleanup_pending || s_timers[i].reserved) {
            /* This timer is unavailable until teardown/init succeeds. */
            timer_quarantined = timer_quarantined ||
                                s_timers[i].cleanup_pending;
            continue;
        }
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
        /* A quarantined timer is a temporary cleanup condition, not ordinary
         * exhaustion.  Do not report it as HAL_ERR_NO_RESOURCE to the caller. */
        return timer_quarantined ? HAL_ERR_BUSY : HAL_ERR_NO_RESOURCE;
    }

    /* Reserve the selected channel before touching hardware.  A reservation
     * is not a committed HAL instance and is never offered to another init. */
    ch->reserved = true;
    ch->cleanup_pending = false;
    ch->pin = config->pin;
    ch->polarity = config->polarity;
    ch->output_invert = (config->polarity == HAL_POLARITY_ACTIVE_LOW);
    ch->stopped = false;
    ch->channel = channel_num;
    ch->timer = timer_num;

    if (timer_fresh) {
        /* Only freshly allocated timers are reconfigured, so an existing
         * pin's frequency is never clobbered.  Reserve the timer before the
         * call because a failed call may have modified driver state. */
        s_timers[timer_num].reserved = true;
        s_timers[timer_num].cleanup_pending = false;
        s_timers[timer_num].frequency_hz = config->frequency_hz;
        timer_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
        timer_conf.duty_resolution = HAL_PWM_ESP_DUTY_RESOLUTION;
        timer_conf.timer_num = timer_num;
        timer_conf.freq_hz = config->frequency_hz;
        timer_conf.clk_cfg = LEDC_AUTO_CLK;
        timer_conf.deconfigure = false;

        err = ledc_timer_config(&timer_conf);
        if (err != ESP_OK) {
            /* Roll back even a failed configuration call: the driver may have
             * installed a partial timer before returning its error. */
            esp_err_t cleanup_err = hal_pwm_esp_cleanup_timer(
                timer_num, config->frequency_hz);
            if (cleanup_err == ESP_OK) {
                s_timers[timer_num].in_use = false;
                s_timers[timer_num].reserved = false;
                s_timers[timer_num].cleanup_pending = false;
                s_timers[timer_num].frequency_hz = 0U;
                hal_pwm_esp_clear_channel_record(ch);
            } else {
                /* Keep the timer quarantined.  It is not a free resource, and
                 * the next init retries deconfiguration before allocation. */
                s_timers[timer_num].in_use = true;
                s_timers[timer_num].reserved = false;
                s_timers[timer_num].cleanup_pending = true;
                hal_pwm_esp_clear_channel_record(ch);
            }
            return (cleanup_err == ESP_OK) ? hal_pwm_esp_map_err(err)
                                           : hal_pwm_esp_map_err(cleanup_err);
        }
    }

    /* Timer and channel ownership remain provisional until channel
     * configuration succeeds. */
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
        hal_status_t rollback_status = hal_pwm_esp_map_err(err);
        esp_err_t cleanup_err;

        /* ledc_channel_config() has already touched the channel and GPIO in
         * the legacy driver before it can report an error.  Roll that work
         * back for both shared and fresh timers.  No software channel record
         * has been committed yet. */
        cleanup_err = hal_pwm_esp_cleanup_channel(config->pin,
                                                   config->polarity,
                                                   channel_num);
        if (cleanup_err != ESP_OK) {
            /* Keep the complete provisional channel record reserved.  It is
             * still possible that either ledc_stop() or gpio_reset_pin() left
             * hardware configured, so this channel/pin must not be reused. */
            ch->in_use = true;
            ch->reserved = false;
            ch->cleanup_pending = true;
            if (timer_fresh) {
                /* The timer cannot be deconfigured while its channel rollback
                 * is incomplete.  Quarantine both records and retry the
                 * channel first on the next init. */
                s_timers[timer_num].in_use = true;
                s_timers[timer_num].reserved = false;
                s_timers[timer_num].cleanup_pending = true;
                s_timers[timer_num].frequency_hz = config->frequency_hz;
            }
            return hal_pwm_esp_map_err(cleanup_err);
        }

        /* The channel is fully rolled back.  Only the timer remains to be
         * deconfigured when this init selected a fresh one. */
        hal_pwm_esp_clear_channel_record(ch);
        if (timer_fresh) {
            cleanup_err = hal_pwm_esp_cleanup_timer(timer_num,
                                                     config->frequency_hz);
            if (cleanup_err == ESP_OK) {
                s_timers[timer_num].in_use = false;
                s_timers[timer_num].reserved = false;
                s_timers[timer_num].cleanup_pending = false;
                s_timers[timer_num].frequency_hz = 0U;
            } else {
                s_timers[timer_num].in_use = true;
                s_timers[timer_num].reserved = false;
                s_timers[timer_num].cleanup_pending = true;
                s_timers[timer_num].frequency_hz = config->frequency_hz;
                rollback_status = hal_pwm_esp_map_err(cleanup_err);
            }
        }

        return rollback_status;
    }

    /* Commit timer ownership only after the channel has been configured. */
    s_timers[timer_num].in_use = true;
    s_timers[timer_num].reserved = false;
    s_timers[timer_num].cleanup_pending = false;
    s_timers[timer_num].frequency_hz = config->frequency_hz;

    ch->in_use = true;
    ch->reserved = false;
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
            return hal_pwm_esp_map_err(err);
        }
        ch->stopped = true;
        return HAL_OK;
    }

    err = ledc_set_duty(HAL_PWM_ESP_SPEED_MODE, ch->channel,
                        hal_pwm_esp_duty_to_ticks(duty_percent));
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }
    /* ledc_update_duty() also re-enables a channel previously stopped by
     * hal_pwm_force_inactive() or the 100% endpoint handling. */
    err = ledc_update_duty(HAL_PWM_ESP_SPEED_MODE, ch->channel);
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
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
        return hal_pwm_esp_map_err(err);
    }
    ch->stopped = true;

    return HAL_OK;
}

hal_status_t hal_pwm_deinit(hal_pin_t pin_id)
{
    hal_pwm_esp_channel_t *ch;
    ledc_timer_t timer_num;
    esp_err_t err;
    bool timer_shared = false;
    size_t i;

    if (!hal_pwm_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    ch = hal_pwm_esp_find_channel(pin_id);
    if (ch == NULL) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    /* Stop the channel at the logical INACTIVE level.  On failure nothing is
     * released and the pin stays initialized, so a later hal_pwm_deinit()
     * retry can complete the teardown. */
    err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, ch->channel,
                    hal_pwm_esp_inactive_level(ch));
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }

    timer_num = ch->timer;

    /* Release the pad, but keep the channel record until all teardown is
     * complete.  In particular, if the last channel's timer cleanup fails,
     * the same pin remains discoverable and a later deinit can retry it. */
    err = gpio_reset_pin((gpio_num_t)ch->pin);
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }

    /* If another channel still uses the timer it must keep running, so only
     * this channel is released.  Exclude `ch` because its ownership record is
     * intentionally still present while this decision is made. */
    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (&s_channels[i] != ch && s_channels[i].in_use &&
            s_channels[i].timer == timer_num) {
            timer_shared = true;
            break;
        }
    }
    if (!timer_shared) {
        /* The timer is no longer referenced: pause and deconfigure it before
         * clearing either allocator record.  A failure leaves `ch` intact so
         * the caller can retry deinitialization. */
        err = hal_pwm_esp_cleanup_timer(timer_num,
                                         s_timers[timer_num].frequency_hz);
        if (err != ESP_OK) {
            return hal_pwm_esp_map_err(err);
        }
        s_timers[timer_num].in_use = false;
        s_timers[timer_num].reserved = false;
        s_timers[timer_num].cleanup_pending = false;
        s_timers[timer_num].frequency_hz = 0U;
    }

    /* All hardware cleanup that can fail has succeeded.  Only now release the
     * channel record, making a subsequent deinit correctly report that the
     * pin is no longer initialized. */
    hal_pwm_esp_clear_channel_record(ch);

    return HAL_OK;
}