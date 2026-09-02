/*
 * ESP PWM backend runtime tests against an LEDC driver test double (TASK-009)
 *
 * Compiles the real ESP-IDF PWM backend (src/hal/esp/hal_pwm_esp.c) on the
 * host against the mocked legacy LEDC/GPIO driver surface (tests/hal_esp/mock
 * + ledc_mock.c) so the portable contract can be exercised without ESP
 * hardware.  The build registers this same test at simulated ESP-IDF
 * versions 5.0, 5.1, 5.3 and 5.5, proving that the version guards around
 * ledc_timer_config_t.deconfigure (5.2+) and ledc_channel_config_t.sleep_mode
 * (5.4+) compile for every supported ESP-IDF release line.
 *
 * The scenarios mirror the TASK-009 validation list:
 *   1. successful init followed by 0% / 50% / 100% duty, forced inactive,
 *      resumed duty and deinit for both polarities,
 *   2. multiple simultaneous instances, including compatible timer sharing
 *      and incompatible frequencies,
 *   3. deinit followed by re-init of the released pin/channel,
 *   4. an unsupported frequency returning HAL_ERR_NOT_SUPPORTED,
 *   5. more consecutive unsupported-frequency attempts than the target has
 *      LEDC timers, followed by a supported initialization that still
 *      succeeds instead of reporting HAL_ERR_NO_RESOURCE,
 * plus failure-atomic initialization (channel/timer rollback, context
 * allocation failure) and deinit error mapping.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_idf_version.h"
#include "hal_pwm.h"
#include "ledc_mock.h"
#include "unity.h"

/* The backend uses a fixed 13-bit duty resolution. */
#define TEST_DUTY_PERIOD 8192U

/* -------------------------------------------------------------------- */
/* Helpers                                                               */
/* -------------------------------------------------------------------- */

static void init_pin(hal_pin_t pin, uint32_t freq_hz, hal_polarity_t polarity)
{
    hal_pwm_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.pin = pin;
    cfg.frequency_hz = freq_hz;
    cfg.polarity = polarity;
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_init(&cfg));
}

static void assert_channel(int gpio, uint32_t duty, bool sig_en,
                           uint32_t idle, bool invert)
{
    ledc_channel_t ch;

    ch = ledc_mock_find_channel_by_gpio(gpio);
    TEST_ASSERT_TRUE(ch != LEDC_CHANNEL_MAX);
    TEST_ASSERT_TRUE(ledc_mock_channel_attached(ch));
    TEST_ASSERT_EQUAL_INT(gpio, ledc_mock_channel_gpio(ch));
    TEST_ASSERT_EQUAL_UINT32(duty, ledc_mock_channel_duty(ch));
    TEST_ASSERT_EQUAL_INT(sig_en ? 1 : 0,
                          ledc_mock_channel_sig_en(ch) ? 1 : 0);
    TEST_ASSERT_EQUAL_UINT32(idle, ledc_mock_channel_idle(ch));
    TEST_ASSERT_EQUAL_INT(invert ? 1 : 0,
                          ledc_mock_channel_invert(ch) ? 1 : 0);
}

static void assert_not_attached(int gpio)
{
    TEST_ASSERT_TRUE(ledc_mock_find_channel_by_gpio(gpio) == LEDC_CHANNEL_MAX);
}

/* Reject a single frequency so tests can pick an "unsupported" request. */
static bool reject_12345(uint32_t freq_hz)
{
    return freq_hz != 12345U;
}

/* -------------------------------------------------------------------- */
/* Argument / state validation                                           */
/* -------------------------------------------------------------------- */

static void test_init_argument_validation(void)
{
    hal_pwm_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.pin = 2U;
    cfg.frequency_hz = 1000U;
    cfg.polarity = HAL_POLARITY_ACTIVE_HIGH;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, hal_pwm_init(NULL));

    cfg.frequency_hz = 0U;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, hal_pwm_init(&cfg));
    cfg.frequency_hz = 1000U;

    cfg.polarity = (hal_polarity_t)5;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, hal_pwm_init(&cfg));
    cfg.polarity = HAL_POLARITY_ACTIVE_HIGH;

    cfg.pin = HAL_PIN_NONE;
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, hal_pwm_init(&cfg));
    cfg.pin = 2U;

    cfg.pin = 100U; /* beyond GPIO_NUM_MAX */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, hal_pwm_init(&cfg));
    cfg.pin = 2U;

    cfg.pin = 34U; /* input-only pad on the modelled ESP32 pad map */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_SUPPORTED, hal_pwm_init(&cfg));
    cfg.pin = 2U;

    /* Nothing above may have consumed any resource. */
    TEST_ASSERT_FALSE(ledc_mock_ctx_exists());
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_0));
}

static void test_operation_validation(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, hal_pwm_set_duty(2U, -0.1f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, hal_pwm_set_duty(2U, 100.1f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, hal_pwm_set_duty(2U, NAN));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, hal_pwm_set_duty(2U, INFINITY));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_OUT_OF_RANGE, hal_pwm_set_duty(2U, -INFINITY));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, hal_pwm_set_duty(HAL_PIN_NONE, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, hal_pwm_set_duty(100U, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_set_duty(2U, 50.0f));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, hal_pwm_force_inactive(HAL_PIN_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_force_inactive(2U));

    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_PIN, hal_pwm_deinit(HAL_PIN_NONE));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_deinit(2U));

    TEST_ASSERT_FALSE(ledc_mock_ctx_exists());
}

static void test_init_already_initialized(void)
{
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);

    /* Re-initializing an initialized pin (same or different configuration)
     * must fail and leave the existing configuration unchanged. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_ALREADY_INITIALIZED,
                          hal_pwm_init(&(hal_pwm_config_t){ 2U, 1000U,
                                                            HAL_POLARITY_ACTIVE_HIGH }));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_ALREADY_INITIALIZED,
                          hal_pwm_init(&(hal_pwm_config_t){ 2U, 2000U,
                                                            HAL_POLARITY_ACTIVE_LOW }));

    /* The original configuration is unchanged. */
    assert_channel(2, 0U, true, 0U, false);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
    assert_not_attached(2);
}

/* -------------------------------------------------------------------- */
/* Duty lifecycle for both polarities                                    */
/* -------------------------------------------------------------------- */

static void test_duty_lifecycle_active_high(void)
{
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(2, 0U, true, 0U, false);

    /* 50% -> half of the 13-bit period. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(2U, 50.0f));
    assert_channel(2, TEST_DUTY_PERIOD / 2U, true, 0U, false);

    /* 0% -> permanently inactive (0 ticks), generation still enabled. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(2U, 0.0f));
    assert_channel(2, 0U, true, 0U, false);

    /* 100% -> constant logical ACTIVE: channel stopped at the active idle
     * level (1) instead of programming 2**resolution.  The previously set
     * duty tick value stays untouched. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(2U, 100.0f));
    assert_channel(2, 0U, false, 1U, false);

    /* force_inactive -> channel stopped at logical INACTIVE (idle 0). */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_force_inactive(2U));
    assert_channel(2, 0U, false, 0U, false);

    /* A later set_duty() resumes generation. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(2U, 25.0f));
    assert_channel(2, TEST_DUTY_PERIOD / 4U, true, 0U, false);

    /* set_duty(100) again (constant active), then deinit stops the output. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(2U, 100.0f));
    assert_channel(2, TEST_DUTY_PERIOD / 4U, false, 1U, false);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
    assert_not_attached(2);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_set_duty(2U, 50.0f));
}

static void test_duty_lifecycle_active_low(void)
{
    init_pin(3U, 1000U, HAL_POLARITY_ACTIVE_LOW);
    assert_channel(3, 0U, true, 0U, true);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(3U, 50.0f));
    assert_channel(3, TEST_DUTY_PERIOD / 2U, true, 0U, true);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(3U, 0.0f));
    assert_channel(3, 0U, true, 0U, true);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(3U, 100.0f));
    assert_channel(3, 0U, false, 1U, true);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_force_inactive(3U));
    assert_channel(3, 0U, false, 0U, true);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(3U, 75.0f));
    assert_channel(3, 3U * TEST_DUTY_PERIOD / 4U, true, 0U, true);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(3U));
    assert_not_attached(3);
}

/* -------------------------------------------------------------------- */
/* Multiple instances: timer sharing and incompatible frequencies        */
/* -------------------------------------------------------------------- */

static void test_timer_sharing_and_incompatible_frequencies(void)
{
    ledc_channel_t ch2;
    ledc_channel_t ch3;
    ledc_channel_t ch4;
    ledc_channel_t ch5;
    ledc_channel_t ch6;
    ledc_timer_t timer1000;

    /* Compatible frequencies share one timer. */
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    init_pin(3U, 1000U, HAL_POLARITY_ACTIVE_LOW);
    ch2 = ledc_mock_find_channel_by_gpio(2);
    ch3 = ledc_mock_find_channel_by_gpio(3);
    TEST_ASSERT_TRUE(ch2 != LEDC_CHANNEL_MAX && ch3 != LEDC_CHANNEL_MAX);
    /* Two active instances never claim the same exclusive channel. */
    TEST_ASSERT_NOT_EQUAL_INT((int)ch2, (int)ch3);
    /* But they share their timer. */
    timer1000 = ledc_mock_channel_timer(ch2);
    TEST_ASSERT_EQUAL_INT((int)timer1000, (int)ledc_mock_channel_timer(ch3));

    /* Incompatible frequencies get distinct timers. */
    init_pin(4U, 2000U, HAL_POLARITY_ACTIVE_HIGH);
    init_pin(5U, 3000U, HAL_POLARITY_ACTIVE_HIGH);
    init_pin(6U, 4000U, HAL_POLARITY_ACTIVE_HIGH);
    ch4 = ledc_mock_find_channel_by_gpio(4);
    ch5 = ledc_mock_find_channel_by_gpio(5);
    ch6 = ledc_mock_find_channel_by_gpio(6);
    TEST_ASSERT_NOT_EQUAL_INT((int)timer1000, (int)ledc_mock_channel_timer(ch4));
    TEST_ASSERT_NOT_EQUAL_INT((int)timer1000, (int)ledc_mock_channel_timer(ch5));
    TEST_ASSERT_NOT_EQUAL_INT((int)timer1000, (int)ledc_mock_channel_timer(ch6));
    TEST_ASSERT_NOT_EQUAL_INT((int)ledc_mock_channel_timer(ch4),
                              (int)ledc_mock_channel_timer(ch5));
    TEST_ASSERT_NOT_EQUAL_INT((int)ledc_mock_channel_timer(ch4),
                              (int)ledc_mock_channel_timer(ch6));
    TEST_ASSERT_NOT_EQUAL_INT((int)ledc_mock_channel_timer(ch5),
                              (int)ledc_mock_channel_timer(ch6));

    /* All four timers are in use: a fifth distinct frequency is exhausted. */
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NO_RESOURCE,
                          hal_pwm_init(&(hal_pwm_config_t){ 7U, 5000U,
                                                            HAL_POLARITY_ACTIVE_HIGH }));

    /* Releasing pin 4 frees its timer; a later init can use it again. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(4U));
    init_pin(7U, 5000U, HAL_POLARITY_ACTIVE_HIGH);

    /* Deinit of a shared timer keeps it running for the other user. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
    assert_channel(3, 0U, true, 0U, true);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    /* 5.2+ deconfigures an unreferenced timer; the timer is still configured
     * here because pin 3 keeps using it. */
    TEST_ASSERT_TRUE(ledc_mock_timer_configured(timer1000));
#endif

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(3U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(5U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(6U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(7U));

    /* Released timers/channels are reusable by new instances. */
    init_pin(8U, 2000U, HAL_POLARITY_ACTIVE_HIGH);
    init_pin(9U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(8, 0U, true, 0U, false);
    assert_channel(9, 0U, true, 0U, false);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(8U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(9U));
}

/* -------------------------------------------------------------------- */
/* Deinit then re-init                                                   */
/* -------------------------------------------------------------------- */

static void test_deinit_reinit(void)
{
    init_pin(11U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(11U, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(11U));
    assert_not_attached(11);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_set_duty(11U, 50.0f));
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_deinit(11U));

    /* The same pin may be initialized again, even with another frequency. */
    init_pin(11U, 1500U, HAL_POLARITY_ACTIVE_LOW);
    assert_channel(11, 0U, true, 0U, true);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(11U, 50.0f));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    TEST_ASSERT_TRUE(ledc_mock_timer_configured(ledc_mock_channel_timer(
        ledc_mock_find_channel_by_gpio(11))));
#endif
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(11U));
    assert_not_attached(11);
}

/* -------------------------------------------------------------------- */
/* Unsupported frequency handling                                        */
/* -------------------------------------------------------------------- */

static void test_unsupported_frequency(void)
{
    hal_pwm_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.pin = 2U;
    cfg.frequency_hz = 12345U;
    cfg.polarity = HAL_POLARITY_ACTIVE_HIGH;

    ledc_mock_set_freq_supported(reject_12345);

    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_SUPPORTED, hal_pwm_init(&cfg));

    /* The failed call leaves no HAL instance, no channel and no timer
     * configured - only the driver context may exist (ESP-IDF 5.5). */
    assert_not_attached(2);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_set_duty(2U, 50.0f));
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_0));

    /* A later supported initialization still succeeds. */
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(2, 0U, true, 0U, false);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
}

static void test_repeated_unsupported_frequency_no_exhaustion(void)
{
    hal_pwm_config_t cfg;
    int attempt;

    memset(&cfg, 0, sizeof(cfg));
    cfg.pin = 2U;
    cfg.frequency_hz = 12345U;
    cfg.polarity = HAL_POLARITY_ACTIVE_HIGH;

    ledc_mock_set_freq_supported(reject_12345);

    /* More consecutive unsupported attempts than the target has timers. */
    for (attempt = 0; attempt < 20; ++attempt) {
        TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_SUPPORTED, hal_pwm_init(&cfg));
        assert_not_attached(2);
    }

    /* No timer was ever acquired/configures by the failed attempts, so a
     * supported request still succeeds instead of reporting exhaustion. */
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_0));
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_1));
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_2));
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_3));

    cfg.frequency_hz = 1000U;
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_init(&cfg));
    assert_channel(2, 0U, true, 0U, false);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
}

/* -------------------------------------------------------------------- */
/* Failure-atomic initialization                                         */
/* -------------------------------------------------------------------- */

static void test_failed_init_channel_config_rollback(void)
{
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(2, 0U, true, 0U, false);

    /* A pin that fails after the driver attached its channel must be rolled
     * back (stop + GPIO reset + timer release) and freed. */
    ledc_mock_set_channel_config_fail_after_attach(ESP_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT,
                          hal_pwm_init(&(hal_pwm_config_t){ 4U, 1000U,
                                                            HAL_POLARITY_ACTIVE_HIGH }));
    assert_not_attached(4);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_set_duty(4U, 50.0f));

    /* The existing instance on pin 2 and its shared timer are unchanged. */
    assert_channel(2, 0U, true, 0U, false);

    /* The failed call left no reservation: the same pin can be initialized
     * immediately (even with the same frequency) once the driver recovers. */
    ledc_mock_set_channel_config_fail_after_attach(ESP_OK);
    init_pin(4U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(4, 0U, true, 0U, false);

    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(4U));
}

static void test_failed_init_fresh_timer_rollback(void)
{
    /* A fresh timer whose channel configuration fails is released again:
     * with 5.2+ deconfiguration it leaves the driver, and in every version
     * the HAL record disappears so the timer stays allocatable. */
    ledc_mock_set_channel_config_fail_after_attach(ESP_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT,
                          hal_pwm_init(&(hal_pwm_config_t){ 2U, 1000U,
                                                            HAL_POLARITY_ACTIVE_HIGH }));
    assert_not_attached(2);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    /* 5.2+ rolls the driver timer back completely. */
    TEST_ASSERT_FALSE(ledc_mock_timer_configured(LEDC_TIMER_0));
#endif

    ledc_mock_set_channel_config_fail_after_attach(ESP_OK);
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(2, 0U, true, 0U, false);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
}

static void test_failed_init_context_allocation(void)
{
    ledc_mock_set_ctx_alloc_fail(ESP_ERR_NO_MEM);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NO_RESOURCE,
                          hal_pwm_init(&(hal_pwm_config_t){ 2U, 1000U,
                                                            HAL_POLARITY_ACTIVE_HIGH }));
    assert_not_attached(2);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_set_duty(2U, 50.0f));

    /* Recovering the driver lets the same pin initialize. */
    ledc_mock_set_ctx_alloc_fail(ESP_OK);
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);
    assert_channel(2, 0U, true, 0U, false);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
}

/* -------------------------------------------------------------------- */
/* Deinit error handling                                                 */
/* -------------------------------------------------------------------- */

static void test_deinit_gpio_reset_failure(void)
{
    init_pin(2U, 1000U, HAL_POLARITY_ACTIVE_HIGH);

    /* A pad-reset failure is mapped and keeps the instance initialized so a
     * retry can complete the teardown. */
    ledc_mock_set_gpio_reset_fail(ESP_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_INVALID_ARGUMENT, hal_pwm_deinit(2U));
    assert_channel(2, 0U, false, 0U, false);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_set_duty(2U, 50.0f));

    ledc_mock_set_gpio_reset_fail(ESP_OK);
    TEST_ASSERT_EQUAL_INT(HAL_OK, hal_pwm_deinit(2U));
    assert_not_attached(2);
    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED, hal_pwm_deinit(2U));
}

/* -------------------------------------------------------------------- */
/* Runner                                                                */
/* -------------------------------------------------------------------- */

void setUp(void)
{
    ledc_mock_reset();
}

void tearDown(void)
{
}

static void hal_pwm_esp_tests_run(void)
{
    RUN_TEST(test_init_argument_validation);
    RUN_TEST(test_operation_validation);
    RUN_TEST(test_init_already_initialized);
    RUN_TEST(test_duty_lifecycle_active_high);
    RUN_TEST(test_duty_lifecycle_active_low);
    RUN_TEST(test_timer_sharing_and_incompatible_frequencies);
    RUN_TEST(test_deinit_reinit);
    RUN_TEST(test_unsupported_frequency);
    RUN_TEST(test_repeated_unsupported_frequency_no_exhaustion);
    RUN_TEST(test_failed_init_channel_config_rollback);
    RUN_TEST(test_failed_init_fresh_timer_rollback);
    RUN_TEST(test_failed_init_context_allocation);
    RUN_TEST(test_deinit_gpio_reset_failure);
}

int main(void)
{
    UNITY_BEGIN();
    hal_pwm_esp_tests_run();
    return UNITY_END();
}