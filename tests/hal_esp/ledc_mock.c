/*
 * LEDC driver test double - implementation (TASK-009)
 *
 * Implements the mocked LEDC/GPIO driver surface declared in mock/.
 * See ledc_mock.h for the API contract and the modelled ESP-IDF 5.x
 * behavior.
 */

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "ledc_mock.h"

/* --------------------------------------------------------------------- */
/* Mocked driver state                                                   */
/* --------------------------------------------------------------------- */

typedef struct ledc_mock_timer {
    bool configured; /**< Timer was acquired/configured by ledc_timer_config(). */
    bool paused;     /**< Timer was paused with ledc_timer_pause(). */
    uint32_t freq_hz;
} ledc_mock_timer_t;

typedef struct ledc_mock_channel {
    bool attached;            /**< Channel is attached to a GPIO. */
    int gpio;
    ledc_timer_t timer;
    uint32_t duty;
    uint32_t idle_level;
    bool sig_out_en;
    bool output_invert;
} ledc_mock_channel_t;

static bool s_ctx_exists;
static bool (*s_freq_supported)(uint32_t freq_hz);
static esp_err_t s_ctx_alloc_err;
static esp_err_t s_channel_config_after_attach_err;
static esp_err_t s_gpio_reset_err;
static ledc_mock_timer_t s_timers[LEDC_TIMER_MAX];
static ledc_mock_channel_t s_channels[LEDC_CHANNEL_MAX];

/* --------------------------------------------------------------------- */
/* Internal helpers                                                       */
/* --------------------------------------------------------------------- */

static void ledc_mock_create_ctx(void)
{
    s_ctx_exists = true;
}

static bool ledc_mock_alloc_ctx(void)
{
    if (s_ctx_exists) {
        return true;
    }
    if (s_ctx_alloc_err != ESP_OK) {
        return false;
    }
    ledc_mock_create_ctx();
    return true;
}

static bool ledc_mock_freq_achievable(uint32_t freq_hz)
{
    if (s_freq_supported == NULL) {
        return true;
    }
    return s_freq_supported(freq_hz);
}

/* --------------------------------------------------------------------- */
/* Public control API                                                     */
/* --------------------------------------------------------------------- */

void ledc_mock_reset(void)
{
    size_t i;

    s_ctx_exists = false;
    s_freq_supported = NULL;
    s_ctx_alloc_err = ESP_OK;
    s_channel_config_after_attach_err = ESP_OK;
    s_gpio_reset_err = ESP_OK;

    for (i = 0U; i < LEDC_TIMER_MAX; ++i) {
        s_timers[i].configured = false;
        s_timers[i].paused = false;
        s_timers[i].freq_hz = 0U;
    }
    for (i = 0U; i < LEDC_CHANNEL_MAX; ++i) {
        s_channels[i].attached = false;
        s_channels[i].gpio = -1;
        s_channels[i].timer = LEDC_TIMER_0;
        s_channels[i].duty = 0U;
        s_channels[i].idle_level = 0U;
        s_channels[i].sig_out_en = false;
        s_channels[i].output_invert = false;
    }
}

void ledc_mock_set_freq_supported(bool (*is_supported)(uint32_t freq_hz))
{
    s_freq_supported = is_supported;
}

void ledc_mock_set_ctx_alloc_fail(esp_err_t err)
{
    s_ctx_alloc_err = err;
}

void ledc_mock_set_channel_config_fail_after_attach(esp_err_t err)
{
    s_channel_config_after_attach_err = err;
}

void ledc_mock_set_gpio_reset_fail(esp_err_t err)
{
    s_gpio_reset_err = err;
}

/* --------------------------------------------------------------------- */
/* State inspection                                                       */
/* --------------------------------------------------------------------- */

bool ledc_mock_ctx_exists(void)
{
    return s_ctx_exists;
}

bool ledc_mock_timer_configured(ledc_timer_t timer)
{
    return s_timers[timer].configured;
}

bool ledc_mock_timer_paused(ledc_timer_t timer)
{
    return s_timers[timer].paused;
}

uint32_t ledc_mock_timer_freq(ledc_timer_t timer)
{
    return s_timers[timer].freq_hz;
}

bool ledc_mock_channel_attached(ledc_channel_t channel)
{
    return s_channels[channel].attached;
}

int ledc_mock_channel_gpio(ledc_channel_t channel)
{
    return s_channels[channel].gpio;
}

ledc_timer_t ledc_mock_channel_timer(ledc_channel_t channel)
{
    return s_channels[channel].timer;
}

uint32_t ledc_mock_channel_duty(ledc_channel_t channel)
{
    return s_channels[channel].duty;
}

bool ledc_mock_channel_sig_en(ledc_channel_t channel)
{
    return s_channels[channel].sig_out_en;
}

uint32_t ledc_mock_channel_idle(ledc_channel_t channel)
{
    return s_channels[channel].idle_level;
}

bool ledc_mock_channel_invert(ledc_channel_t channel)
{
    return s_channels[channel].output_invert;
}

ledc_channel_t ledc_mock_find_channel_by_gpio(int gpio)
{
    ledc_channel_t ch;

    for (ch = LEDC_CHANNEL_0; ch < LEDC_CHANNEL_MAX; ++ch) {
        if (s_channels[ch].attached && s_channels[ch].gpio == gpio) {
            return ch;
        }
    }
    return LEDC_CHANNEL_MAX;
}

/* --------------------------------------------------------------------- */
/* Modelled driver API                                                    */
/* --------------------------------------------------------------------- */

esp_err_t ledc_timer_config(const ledc_timer_config_t *timer_conf)
{
    uint32_t timer_num;
    uint32_t speed_mode;

    if (timer_conf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    speed_mode = (uint32_t)timer_conf->speed_mode;
    timer_num = (uint32_t)timer_conf->timer_num;
    if (speed_mode >= LEDC_SPEED_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer_num >= LEDC_TIMER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

#if MOCK_LEDC_IDF_VERSION_GE(5, 2)
    if (timer_conf->deconfigure) {
        /* Mirror ESP-IDF 5.5 ledc_timer_del(): the pause succeeds whenever
         * the driver context exists, but the deletion itself requires the
         * timer to have been configured and paused. */
        if (!s_ctx_exists) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_timers[timer_num].configured) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_timers[timer_num].paused) {
            return ESP_ERR_INVALID_STATE;
        }
        s_timers[timer_num].configured = false;
        s_timers[timer_num].paused = false;
        s_timers[timer_num].freq_hz = 0U;
        return ESP_OK;
    }
#else
    (void)0;
#endif

    if (timer_conf->freq_hz == 0U ||
        (uint32_t)timer_conf->duty_resolution == 0U ||
        (uint32_t)timer_conf->duty_resolution >= LEDC_TIMER_BIT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The driver creates its speed-mode context before validating the
     * divisor (ESP-IDF 5.5 behavior): an unsupported frequency returns
     * ESP_FAIL and leaves a context but no configured timer. */
    if (!ledc_mock_alloc_ctx()) {
        return ESP_ERR_NO_MEM;
    }
    if (!ledc_mock_freq_achievable(timer_conf->freq_hz)) {
        return ESP_FAIL;
    }

    s_timers[timer_num].configured = true;
    s_timers[timer_num].paused = false;
    s_timers[timer_num].freq_hz = timer_conf->freq_hz;
    return ESP_OK;
}

esp_err_t ledc_timer_pause(ledc_mode_t speed_mode, ledc_timer_t timer_sel)
{
    if (speed_mode >= LEDC_SPEED_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer_sel >= LEDC_TIMER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Mirrors the real driver: pausing only requires the driver context to
     * exist, not that the timer was ever configured. */
    if (!s_ctx_exists) {
        return ESP_ERR_INVALID_STATE;
    }
    s_timers[timer_sel].paused = true;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *ledc_conf)
{
    uint32_t speed_mode;
    uint32_t channel_num;
    uint32_t timer_sel;

    if (ledc_conf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    speed_mode = (uint32_t)ledc_conf->speed_mode;
    channel_num = (uint32_t)ledc_conf->channel;
    timer_sel = (uint32_t)ledc_conf->timer_sel;
    if (channel_num >= LEDC_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (speed_mode >= LEDC_SPEED_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(ledc_conf->gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer_sel >= LEDC_TIMER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ledc_conf->intr_type >= LEDC_INTR_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
#if MOCK_LEDC_IDF_VERSION_GE(5, 4)
    if (ledc_conf->sleep_mode >= LEDC_SLEEP_MODE_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
#endif

    if (!ledc_mock_alloc_ctx()) {
        return ESP_ERR_NO_MEM;
    }

    s_channels[channel_num].attached = true;
    s_channels[channel_num].gpio = ledc_conf->gpio_num;
    s_channels[channel_num].timer = (ledc_timer_t)timer_sel;
    s_channels[channel_num].duty = ledc_conf->duty;
    s_channels[channel_num].idle_level = 0U;
    s_channels[channel_num].sig_out_en = true;
    s_channels[channel_num].output_invert =
        ledc_conf->flags.output_invert != 0U;

    if (s_channel_config_after_attach_err != ESP_OK) {
        return s_channel_config_after_attach_err;
    }

    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel,
                        uint32_t duty)
{
    if (speed_mode >= LEDC_SPEED_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= LEDC_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx_exists) {
        return ESP_ERR_INVALID_STATE;
    }
    s_channels[channel].duty = duty;
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    if (speed_mode >= LEDC_SPEED_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= LEDC_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx_exists) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Re-enables a channel previously stopped by ledc_stop(). */
    s_channels[channel].sig_out_en = true;
    return ESP_OK;
}

esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel,
                    uint32_t idle_level)
{
    if (speed_mode >= LEDC_SPEED_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= LEDC_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx_exists) {
        return ESP_ERR_INVALID_STATE;
    }
    s_channels[channel].sig_out_en = false;
    s_channels[channel].idle_level = idle_level & 0x1U;
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    ledc_channel_t ch;

    if (!GPIO_IS_VALID_GPIO(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gpio_reset_err != ESP_OK) {
        return s_gpio_reset_err;
    }
    for (ch = LEDC_CHANNEL_0; ch < LEDC_CHANNEL_MAX; ++ch) {
        if (s_channels[ch].attached && s_channels[ch].gpio == (int)gpio_num) {
            s_channels[ch].attached = false;
            s_channels[ch].gpio = -1;
            s_channels[ch].sig_out_en = false;
        }
    }
    return ESP_OK;
}