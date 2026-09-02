/*
 * hal_demo — ESP-IDF example demonstrating the portable HAL API (TASK-010).
 *
 * This application drives a GPIO output and a PWM output exclusively through
 * the portable hq_platform HAL:
 *
 *   - hal_gpio_init()   configures an output pin,
 *   - hal_gpio_write()  drives logical ACTIVE and INACTIVE states,
 *   - hal_gpio_deinit() releases the GPIO pin,
 *   - hal_pwm_init()    configures a PWM output,
 *   - hal_pwm_set_duty() sets two different duty-cycle values,
 *   - hal_pwm_force_inactive() halts PWM generation at the logical INACTIVE
 *     level,
 *   - hal_pwm_deinit()  releases the PWM output.
 *
 * Together these demonstrate the complete HAL lifecycle:
 *
 *     init -> use -> force inactive -> deinit
 *
 * Pin selection: the GPIO and PWM pads are configurable at build time through
 * the example's Kconfig options CONFIG_HAL_DEMO_GPIO_PIN and
 * CONFIG_HAL_DEMO_PWM_PIN (see main/Kconfig.projbuild and
 * sdkconfig.defaults).  Keeping the pins in the application's project
 * configuration means the portable HAL API itself never assumes a particular
 * pin exists on any target: the application chooses the pad, and the HAL only
 * ever deals with opaque hal_pin_t values.
 *
 * main.c calls only the portable HAL API.  It does not call any ESP-IDF GPIO
 * (driver/gpio.h), LEDC (driver/ledc.h) or ESP-IDF-specific HAL
 * implementation function directly; all target-specific work stays inside the
 * hq_hal ESP-IDF component (src/hal/hq_hal).
 */

#include <stdio.h>

#include "hal_types.h"
#include "hal_gpio.h"
#include "hal_pwm.h"

/* Demo frequency for the PWM output.  The hq_hal ESP-IDF PWM backend accepts
 * any frequency the LEDC peripheral can produce at its fixed 13-bit duty
 * resolution (roughly 0.04 Hz to 9.7 kHz on an 80 MHz APB clock); 1 kHz is a
 * mid-range value that is achievable on the supported targets. */
#define HAL_DEMO_PWM_FREQUENCY_HZ 1000u

static int demo_gpio(void)
{
    hal_gpio_config_t config;
    hal_status_t status;

    config.pin = (hal_pin_t)CONFIG_HAL_DEMO_GPIO_PIN;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;
    config.pull = HAL_GPIO_PULL_NONE;
    config.output = true;

    status = hal_gpio_init(&config);
    if (status != HAL_OK) {
        printf("hal_demo: hal_gpio_init failed (%d)\n", (int)status);
        return -1;
    }
    printf("hal_demo: GPIO pin %lu initialized as output\n",
           (unsigned long)config.pin);

    /* Drive the logical ACTIVE state through the portable API. */
    status = hal_gpio_write(config.pin, true);
    if (status != HAL_OK) {
        printf("hal_demo: hal_gpio_write(ACTIVE) failed (%d)\n", (int)status);
        goto fail;
    }
    printf("hal_demo: GPIO pin %lu driven ACTIVE\n", (unsigned long)config.pin);

    /* Drive the logical INACTIVE state through the portable API. */
    status = hal_gpio_write(config.pin, false);
    if (status != HAL_OK) {
        printf("hal_demo: hal_gpio_write(INACTIVE) failed (%d)\n", (int)status);
        goto fail;
    }
    printf("hal_demo: GPIO pin %lu driven INACTIVE\n", (unsigned long)config.pin);

    status = hal_gpio_deinit(config.pin);
    if (status != HAL_OK) {
        printf("hal_demo: hal_gpio_deinit failed (%d)\n", (int)status);
        return -1;
    }
    printf("hal_demo: GPIO pin %lu deinitialized\n", (unsigned long)config.pin);

    return 0;

fail:
    hal_gpio_deinit(config.pin);
    return -1;
}

static int demo_pwm(void)
{
    hal_pwm_config_t config;
    hal_status_t status;

    config.pin = (hal_pin_t)CONFIG_HAL_DEMO_PWM_PIN;
    config.frequency_hz = HAL_DEMO_PWM_FREQUENCY_HZ;
    config.polarity = HAL_POLARITY_ACTIVE_HIGH;

    status = hal_pwm_init(&config);
    if (status != HAL_OK) {
        printf("hal_demo: hal_pwm_init failed (%d)\n", (int)status);
        return -1;
    }
    printf("hal_demo: PWM pin %lu initialized (%lu Hz)\n",
           (unsigned long)config.pin, (unsigned long)config.frequency_hz);

    /* First duty-cycle value. */
    status = hal_pwm_set_duty(config.pin, 50.0f);
    if (status != HAL_OK) {
        printf("hal_demo: hal_pwm_set_duty(50%%) failed (%d)\n", (int)status);
        goto fail;
    }
    printf("hal_demo: PWM pin %lu duty 50%%\n", (unsigned long)config.pin);

    /* Second, different duty-cycle value. */
    status = hal_pwm_set_duty(config.pin, 25.0f);
    if (status != HAL_OK) {
        printf("hal_demo: hal_pwm_set_duty(25%%) failed (%d)\n", (int)status);
        goto fail;
    }
    printf("hal_demo: PWM pin %lu duty 25%%\n", (unsigned long)config.pin);

    /* Force the output to its logical INACTIVE state. */
    status = hal_pwm_force_inactive(config.pin);
    if (status != HAL_OK) {
        printf("hal_demo: hal_pwm_force_inactive failed (%d)\n", (int)status);
        goto fail;
    }
    printf("hal_demo: PWM pin %lu forced inactive\n", (unsigned long)config.pin);

    status = hal_pwm_deinit(config.pin);
    if (status != HAL_OK) {
        printf("hal_demo: hal_pwm_deinit failed (%d)\n", (int)status);
        return -1;
    }
    printf("hal_demo: PWM pin %lu deinitialized\n", (unsigned long)config.pin);

    return 0;

fail:
    hal_pwm_deinit(config.pin);
    return -1;
}

void app_main(void)
{
    printf("hal_demo: starting portable HAL demo\n");

    if (demo_gpio() != 0) {
        printf("hal_demo: GPIO step failed\n");
        return;
    }
    if (demo_pwm() != 0) {
        printf("hal_demo: PWM step failed\n");
        return;
    }

    printf("hal_demo: portable HAL demo finished successfully\n");
}
