/**
 * @file hal_pwm_esp.c
 * @brief ESP-IDF PWM HAL backend (TASK-009)
 *
 * Real PWM backend for ESP-IDF builds.  It drives the ESP32-family LEDC
 * peripheral through the legacy `driver/ledc.h` API (available on every
 * supported ESP SoC: ESP32, ESP32-S3 and ESP32-C6) and implements the
 * portable contract from hal_pwm.h:
 *
 *   - one exclusive LEDC channel is allocated per initialized pin,
 *   - a channel shares an LEDC timer only while the requested frequency is
 *     identical: the duty resolution, speed mode and clock source are fixed
 *     for the whole backend, so a new pin never changes the frequency or
 *     resolution of an already-initialized pin, and a timer with an
 *     incompatible frequency is never reconfigured while a channel depends
 *     on it,
 *   - the duty cycle is a normalized percent value that is translated
 *     linearly to LEDC duty ticks of the fixed 13-bit resolution: 0.0% maps
 *     to 0 ticks, 50.0% to exactly half a period,
 *   - 100% duty is implemented as a constant logical ACTIVE output (the
 *     channel is stopped at its active idle level) instead of programming
 *     duty == 2**resolution, which is unsafe (unreachable / miscomputed) on
 *     the supported SoCs at a maximum-width duty resolution,
 *   - the active polarity is applied through the LEDC pad output inversion
 *     (flags.output_invert): with HAL_POLARITY_ACTIVE_LOW the duty portion
 *     of every period is physical LOW, matching the portable "active"
 *     semantics for every duty cycle,
 *   - the output starts in the logical INACTIVE state (duty 0.0%),
 *   - hal_pwm_force_inactive() halts the channel at the logical INACTIVE
 *     level; a later hal_pwm_set_duty() resumes generation
 *     (ledc_update_duty() re-enables a stopped channel),
 *   - init/deinit are not idempotent and return the documented errors,
 *   - resource allocation is failure-atomic: no channel/timer record is
 *     committed until every hardware configuration call has succeeded, and a
 *     failed init only rolls back hardware it actually attached (a partially
 *     configured channel is stopped and its pad reset).  A failed call never
 *     quarantines a timer or channel: repeated unsupported-frequency
 *     requests cannot exhaust the LEDC resources, and a later supported
 *     request still succeeds.  On ESP-IDF 5.5 the LEDC driver creates its
 *     speed-mode context before validating the frequency divisor, so an
 *     unsupported frequency leaves a driver context but no configured timer;
 *     the HAL deliberately keeps no reservation for such a call,
 *   - deinit releases the HAL resources: the channel is stopped at logical
 *     INACTIVE, the pad is reset to its default state and the channel record
 *     is cleared; when no other channel still uses the timer the timer
 *     record is cleared too.  On ESP-IDF 5.2+ an unreferenced timer is
 *     additionally paused and deconfigured as a best-effort cleanup; on
 *     older versions (and when that cleanup is not possible) the
 *     driver-side timer configuration may remain and is safely reused or
 *     reconfigured by a later initialization - this is not a HAL resource
 *     leak because the HAL allocator no longer owns it,
 *   - every ESP-IDF esp_err_t result is mapped to the portable hal_status_t
 *     result type.
 *
 * This backend is compiled for the supported targets from ESP-IDF 5.0
 * onward.  ESP-IDF-version-specific structure members are guarded with the
 * version macros from `esp_idf_version.h`:
 *   - ledc_timer_config_t.deconfigure exists only from ESP-IDF 5.2,
 *   - ledc_channel_config_t.sleep_mode exists only from ESP-IDF 5.4.
 *
 * Supported frequency range
 * -------------------------
 * The backend accepts any frequency the LEDC peripheral can produce at the
 * fixed 13-bit duty resolution with an auto-selected clock source
 * (LEDC_AUTO_CLK).  On the supported targets (APB clock 80 MHz) that is
 * approximately 0.04 Hz to 9.7 kHz.  Frequencies outside the achievable set
 * are reported with #HAL_ERR_NOT_SUPPORTED; the exact achievable set stays
 * target/clock dependent and is detected by the LEDC timer configuration
 * call itself, so it is never hard-coded here.
 *
 * The public HAL headers are pure C99 and include no ESP-IDF headers; all
 * target-specific types (LEDC timers, channels, resolutions) stay inside
 * this backend source.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_idf_version.h"
#include "hal_pwm.h"

/* Every supported ESP target (ESP32, ESP32-S3, ESP32-C6) provides the LEDC
 * peripheral; this backend is only compiled for targets where it exists. */
#if !SOC_LEDC_SUPPORTED
#error "hal_pwm_esp.c requires a target with the LEDC peripheral"
#endif

#define HAL_PWM_ESP_CHANNEL_COUNT LEDC_CHANNEL_MAX
#define HAL_PWM_ESP_TIMER_COUNT   LEDC_TIMER_MAX

/* Fixed 13-bit duty resolution.  The LEDC timer bit width is at least 14 on
 * every supported SoC (ESP32: 20, ESP32-S3: 14, ESP32-C6: 20), so the same
 * resolution works everywhere and keeps timer sharing simple.  Duty values
 * are scaled linearly over the 2**13 ticks of one period.  The full-period
 * endpoint 2**13 is deliberately never programmed for normal PWM operation:
 * the supported SoCs cannot reliably reach 100% duty when `duty` equals the
 * resolution width (see the note in the LEDC driver), so 100% is instead
 * represented by stopping the channel at its logical ACTIVE level. */
#define HAL_PWM_ESP_DUTY_RESOLUTION LEDC_TIMER_13_BIT
#define HAL_PWM_ESP_DUTY_BITS        13U
#define HAL_PWM_ESP_DUTY_PERIOD      (1U << HAL_PWM_ESP_DUTY_BITS)

/* Low-speed mode is available on every ESP32-family target that has LEDC;
 * high-speed mode exists only on the classic ESP32, so the backend always
 * uses the low-speed group for uniform behavior across targets. */
#define HAL_PWM_ESP_SPEED_MODE LEDC_LOW_SPEED_MODE

/* One record per LEDC channel, indexed by the LEDC channel number. */
typedef struct hal_pwm_esp_channel {
    bool in_use;             /**< Channel is allocated to an initialized pin. */
    hal_pin_t pin;           /**< Portable pin identifier using this channel. */
    hal_polarity_t polarity; /**< Active polarity of the output. */
    ledc_timer_t timer;      /**< LEDC timer this channel is bound to. */
} hal_pwm_esp_channel_t;

/* One record per LEDC timer, indexed by the LEDC timer number.  A timer may
 * be shared by several channels, but only while the configured frequency is
 * identical; the duty resolution, speed mode and clock source are fixed for
 * the whole backend, so the frequency is the only parameter that must match
 * for sharing. */
typedef struct hal_pwm_esp_timer {
    bool in_use;           /**< Timer is referenced by at least one channel. */
    uint32_t frequency_hz; /**< Frequency the timer is configured to. */
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

/* Return true when a channel other than `except_idx` is still bound to
 * `timer_num`.  Used before releasing a timer so an active channel's
 * waveform is never disturbed. */
static bool hal_pwm_esp_timer_referenced(ledc_timer_t timer_num,
                                         size_t except_idx)
{
    size_t i;

    for (i = 0U; i < HAL_PWM_ESP_CHANNEL_COUNT; ++i) {
        if (i != except_idx && s_channels[i].in_use &&
            s_channels[i].timer == timer_num) {
            return true;
        }
    }

    return false;
}

static void hal_pwm_esp_clear_channel_record(hal_pwm_esp_channel_t *ch)
{
    ch->in_use = false;
    ch->pin = HAL_PIN_NONE;
    ch->polarity = HAL_POLARITY_ACTIVE_HIGH;
    ch->timer = LEDC_TIMER_0;
}

/* ledc_stop() receives the pad level before the LEDC output inversion is
 * applied, so the level that yields a given logical state on the pad is
 *
 *   idle = physical_state ^ output_invert
 *
 * Logical INACTIVE maps to physical LOW (0) for active-high and to physical
 * HIGH (1) for active-low; solving for both polarities gives idle = 0.
 * Logical ACTIVE maps to physical HIGH (1) / physical LOW (0) respectively,
 * giving idle = 1 in both configurations.  Both helpers keep the portable
 * polarity contract explicit. */
static uint32_t hal_pwm_esp_inactive_idle_level(hal_polarity_t polarity)
{
    const uint32_t physical_inactive =
        (polarity == HAL_POLARITY_ACTIVE_LOW) ? 1U : 0U;
    const uint32_t output_invert =
        (polarity == HAL_POLARITY_ACTIVE_LOW) ? 1U : 0U;

    return physical_inactive ^ output_invert;
}

static uint32_t hal_pwm_esp_active_idle_level(hal_polarity_t polarity)
{
    const uint32_t physical_active =
        (polarity == HAL_POLARITY_ACTIVE_LOW) ? 0U : 1U;
    const uint32_t output_invert =
        (polarity == HAL_POLARITY_ACTIVE_LOW) ? 1U : 0U;

    return physical_active ^ output_invert;
}

/* Linear percent -> ticks over the 2**13 period: 0.0% maps to 0 ticks, 50.0%
 * to exactly half of the period.  This helper is only used for duty below
 * 100%, so the result stays below 2**13 (the full-period endpoint is handled
 * by hal_pwm_set_duty() instead). */
static uint32_t hal_pwm_esp_duty_to_ticks(float duty_percent)
{
    return (uint32_t)((duty_percent / HAL_PWM_DUTY_MAX_PERCENT) *
                      (float)HAL_PWM_ESP_DUTY_PERIOD);
}

/* Map an ESP-IDF esp_err_t result onto the portable hal_status_t result
 * type.  Every driver call below is made against pre-validated allocator
 * state, so the driver only fails on the documented classes: an argument the
 * portable layer cannot know about, memory exhaustion, an unsupported LEDC
 * frequency (ESP_FAIL from ledc_timer_config() when no clock source/divisor
 * can produce the requested frequency at the selected resolution) and
 * unexpected state errors. */
static hal_status_t hal_pwm_esp_map_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return HAL_OK;
    case ESP_ERR_INVALID_ARG:
        return HAL_ERR_INVALID_ARGUMENT;
    case ESP_ERR_NO_MEM:
        return HAL_ERR_NO_RESOURCE;
    case ESP_ERR_NOT_SUPPORTED:
        return HAL_ERR_NOT_SUPPORTED;
    case ESP_FAIL:
        /* ledc_timer_config() returns ESP_FAIL when the requested frequency
         * is not achievable at the selected duty resolution. */
        return HAL_ERR_NOT_SUPPORTED;
    case ESP_ERR_INVALID_STATE:
    default:
        /* All calls are made against validated allocator state, so an
         * invalid-state result is unexpected here. */
        return HAL_ERR_INTERNAL;
    }
}

/* Best-effort driver-side removal of an unreferenced LEDC timer.  The timer
 * must be paused before it can be deconfigured.  Both results are ignored:
 * on ESP-IDF versions without timer deconfiguration this is a no-op, and when
 * the driver cannot remove the timer (for example a timer whose configuration
 * never completed) the configured-but-unreferenced timer is safely reused or
 * reconfigured by a later initialization, which is not a HAL resource leak. */
static void hal_pwm_esp_release_timer(ledc_timer_t timer_num)
{
    (void)ledc_timer_pause(HAL_PWM_ESP_SPEED_MODE, timer_num);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    {
        ledc_timer_config_t timer_conf;

        memset(&timer_conf, 0, sizeof(timer_conf));
        timer_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
        timer_conf.duty_resolution = HAL_PWM_ESP_DUTY_RESOLUTION;
        timer_conf.timer_num = timer_num;
        timer_conf.freq_hz = 0U;
        timer_conf.clk_cfg = LEDC_AUTO_CLK;
        /* When deconfigure is set the other timer fields are ignored. */
        timer_conf.deconfigure = true;
        (void)ledc_timer_config(&timer_conf);
    }
#endif
}

hal_status_t hal_pwm_init(const hal_pwm_config_t *config)
{
    hal_pwm_esp_channel_t *ch;
    ledc_timer_t timer_num = LEDC_TIMER_0;
    int free_timer = -1;
    bool timer_is_new = false;
    bool timer_found = false;
    size_t channel_idx;
    size_t i;
    esp_err_t err;

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
    if (hal_pwm_esp_find_channel(config->pin) != NULL) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }
    /* Input-only pads (for example GPIO 34..39 on the classic ESP32) have
     * no output driver and cannot produce a PWM waveform.  Reject them
     * before touching any timer/channel resource so a failed init never
     * needs a hardware rollback and the portable result reports the
     * unsupported configuration. */
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)config->pin)) {
        return HAL_ERR_NOT_SUPPORTED;
    }

    /* Allocate an exclusive LEDC channel. */
    for (channel_idx = 0U; channel_idx < HAL_PWM_ESP_CHANNEL_COUNT;
         ++channel_idx) {
        if (!s_channels[channel_idx].in_use) {
            break;
        }
    }
    if (channel_idx == HAL_PWM_ESP_CHANNEL_COUNT) {
        return HAL_ERR_NO_RESOURCE;
    }

    /* Select the LEDC timer for the channel.  A timer may be shared by
     * several channels, but only when the frequency (and the fixed duty
     * resolution/speed mode) match - reconfiguring a timer would change the
     * frequency of every pin already using it.  Prefer reusing a compatible
     * timer; otherwise configure a free one; if neither exists, report
     * resource exhaustion. */
    for (i = 0U; i < HAL_PWM_ESP_TIMER_COUNT; ++i) {
        if (s_timers[i].in_use) {
            if (s_timers[i].frequency_hz == config->frequency_hz) {
                timer_num = (ledc_timer_t)i;
                timer_is_new = false;
                timer_found = true;
                break;
            }
        } else if (free_timer < 0) {
            free_timer = (int)i;
        }
    }
    if (!timer_found) {
        if (free_timer >= 0) {
            timer_num = (ledc_timer_t)free_timer;
            timer_is_new = true;
            timer_found = true;
        } else {
            return HAL_ERR_NO_RESOURCE;
        }
    }

    /* Provisionally claim the channel record.  The HAL is not thread-safe
     * and init/deinit are serialized per application, so no other init can
     * observe this record until the hardware calls below have completed; on
     * failure the record is cleared again before returning. */
    ch = &s_channels[channel_idx];
    ch->in_use = true;
    ch->pin = config->pin;
    ch->polarity = config->polarity;
    ch->timer = timer_num;

    if (timer_is_new) {
        ledc_timer_config_t timer_conf;

        memset(&timer_conf, 0, sizeof(timer_conf));
        timer_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
        timer_conf.duty_resolution = HAL_PWM_ESP_DUTY_RESOLUTION;
        timer_conf.timer_num = timer_num;
        timer_conf.freq_hz = config->frequency_hz;
        timer_conf.clk_cfg = LEDC_AUTO_CLK;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        timer_conf.deconfigure = false;
#endif

        err = ledc_timer_config(&timer_conf);
        if (err != ESP_OK) {
            /* The timer was never committed.  On ESP-IDF 5.5 the driver
             * creates its speed-mode context before rejecting the frequency,
             * so ledc_timer_pause() can report success merely because that
             * context exists while the selected timer was never acquired or
             * configured (deconfiguration would then return
             * ESP_ERR_INVALID_STATE).  The HAL record is simply cleared: an
             * unsuccessfully configured timer must not consume or
             * permanently quarantine a timer, so repeated unsupported
             * requests cannot exhaust the pool and a later supported request
             * can still use it. */
            hal_pwm_esp_clear_channel_record(ch);
            return hal_pwm_esp_map_err(err);
        }

        /* The timer is really configured now; commit its record so a later
         * compatible or conflicting init can select it correctly. */
        s_timers[timer_num].in_use = true;
        s_timers[timer_num].frequency_hz = config->frequency_hz;
    }

    {
        ledc_channel_config_t channel_conf;

        memset(&channel_conf, 0, sizeof(channel_conf));
        channel_conf.gpio_num = (int)config->pin;
        channel_conf.speed_mode = HAL_PWM_ESP_SPEED_MODE;
        channel_conf.channel = (ledc_channel_t)channel_idx;
        channel_conf.intr_type = LEDC_INTR_DISABLE;
        channel_conf.timer_sel = timer_num;
        channel_conf.duty = 0U; /* the output starts at logical INACTIVE */
        channel_conf.hpoint = 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
        channel_conf.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
#endif
        /* Active polarity is applied through the pad output inversion: with
         * HAL_POLARITY_ACTIVE_LOW the duty portion of every period becomes
         * physical LOW, exactly as the portable contract defines. */
        channel_conf.flags.output_invert =
            (config->polarity == HAL_POLARITY_ACTIVE_LOW) ? 1U : 0U;

        err = ledc_channel_config(&channel_conf);
        if (err != ESP_OK) {
            /* ledc_channel_config() may have attached the channel and routed
             * the GPIO before reporting its error, so roll that work back
             * before releasing any record.  All rollback steps are
             * best-effort: on failure the allocator must still be left free
             * and consistent, because a failed call must never quarantine a
             * resource. */
            (void)ledc_stop(HAL_PWM_ESP_SPEED_MODE,
                            (ledc_channel_t)channel_idx,
                            hal_pwm_esp_inactive_idle_level(config->polarity));
            (void)gpio_reset_pin((gpio_num_t)config->pin);

            if (timer_is_new) {
                /* A freshly configured timer has no other user.  Release it
                 * (best effort) and clear its record: an unreferenced
                 * driver-side timer configuration is harmless and may stay
                 * configured for later reuse. */
                hal_pwm_esp_release_timer(timer_num);
                s_timers[timer_num].in_use = false;
                s_timers[timer_num].frequency_hz = 0U;
            }
            /* A shared (compatible) timer is left running and owned exactly
             * as it was before this call; an already-active instance is
             * never disturbed. */

            hal_pwm_esp_clear_channel_record(ch);
            return hal_pwm_esp_map_err(err);
        }
    }

    return HAL_OK;
}

hal_status_t hal_pwm_set_duty(hal_pin_t pin_id, float duty_percent)
{
    hal_pwm_esp_channel_t *ch;
    ledc_channel_t channel_num;
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
    channel_num = (ledc_channel_t)(ch - s_channels);

    if (duty_percent == HAL_PWM_DUTY_MAX_PERCENT) {
        /* The LEDC maximum-resolution endpoint is unsafe on the supported
         * SoCs: duty == 2**resolution can overflow or miscompute the
         * hardware counter.  Stop the channel and hold the pad at logical
         * ACTIVE instead; a stopped channel is re-enabled by the next
         * ledc_update_duty() call below. */
        err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, channel_num,
                        hal_pwm_esp_active_idle_level(ch->polarity));
        if (err != ESP_OK) {
            return hal_pwm_esp_map_err(err);
        }
        return HAL_OK;
    }

    err = ledc_set_duty(HAL_PWM_ESP_SPEED_MODE, channel_num,
                        hal_pwm_esp_duty_to_ticks(duty_percent));
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }
    /* ledc_update_duty() also re-enables a channel previously stopped by
     * hal_pwm_force_inactive() or the 100%-duty endpoint handling. */
    err = ledc_update_duty(HAL_PWM_ESP_SPEED_MODE, channel_num);
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }

    return HAL_OK;
}

hal_status_t hal_pwm_force_inactive(hal_pin_t pin_id)
{
    hal_pwm_esp_channel_t *ch;
    ledc_channel_t channel_num;
    esp_err_t err;

    if (!hal_pwm_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    ch = hal_pwm_esp_find_channel(pin_id);
    if (ch == NULL) {
        return HAL_ERR_NOT_INITIALIZED;
    }
    channel_num = (ledc_channel_t)(ch - s_channels);

    /* Halt the channel at the logical INACTIVE level (the idle level before
     * the output inversion).  The last duty value is retained by the
     * hardware, and hal_pwm_set_duty() re-enables the channel. */
    err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, channel_num,
                    hal_pwm_esp_inactive_idle_level(ch->polarity));
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }

    return HAL_OK;
}

hal_status_t hal_pwm_deinit(hal_pin_t pin_id)
{
    hal_pwm_esp_channel_t *ch;
    ledc_timer_t timer_num;
    ledc_channel_t channel_num;
    size_t channel_idx;
    esp_err_t err;

    if (!hal_pwm_esp_is_valid_pin(pin_id)) {
        return HAL_ERR_INVALID_PIN;
    }
    ch = hal_pwm_esp_find_channel(pin_id);
    if (ch == NULL) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    channel_idx = (size_t)(ch - s_channels);
    channel_num = (ledc_channel_t)channel_idx;
    timer_num = ch->timer;

    /* Stop the channel at the logical INACTIVE level.  On failure nothing is
     * released and the pin stays initialized, so a later hal_pwm_deinit()
     * retry can complete the teardown. */
    err = ledc_stop(HAL_PWM_ESP_SPEED_MODE, channel_num,
                    hal_pwm_esp_inactive_idle_level(ch->polarity));
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }

    /* Release the pad: this disconnects the LEDC signal and resets the GPIO
     * routing.  On failure the channel record stays intact so the caller can
     * retry deinitialization. */
    err = gpio_reset_pin((gpio_num_t)pin_id);
    if (err != ESP_OK) {
        return hal_pwm_esp_map_err(err);
    }

    /* If no other channel still depends on the timer, release it: clear its
     * record so a later init may reuse or reconfigure it.  An unreferenced
     * timer that the driver cannot delete stays configured in the driver and
     * is safely reused or reconfigured later; this is not a HAL resource
     * leak. */
    if (!hal_pwm_esp_timer_referenced(timer_num, channel_idx)) {
        hal_pwm_esp_release_timer(timer_num);
        s_timers[timer_num].in_use = false;
        s_timers[timer_num].frequency_hz = 0U;
    }

    hal_pwm_esp_clear_channel_record(ch);

    return HAL_OK;
}