/*
 * Test double for ESP-IDF driver/gpio.h (TASK-009)
 *
 * Host-side mock of the GPIO API surface used by the ESP PWM backend:
 *
 *   - gpio_num_t and the numbered-pad bound GPIO_NUM_MAX,
 *   - the GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_GPIO pad checks
 *     (modelled after the classic ESP32: pads 0..39 with 34..39 being
 *     input-only),
 *   - gpio_reset_pin(), whose driver behavior is modelled in ledc_mock.c.
 */

#ifndef MOCK_DRIVER_GPIO_H
#define MOCK_DRIVER_GPIO_H

#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_MAX 40

/* Modelled after the classic ESP32 pad map: all numbered pads minus the
 * reserved holes (20, 24, 28..31) are valid GPIOs. */
#define GPIO_IS_VALID_GPIO(gpio_num) \
    ((gpio_num) >= 0 && (gpio_num) < GPIO_NUM_MAX && \
     (gpio_num) != 20 && (gpio_num) != 24 && \
     !((gpio_num) >= 28 && (gpio_num) <= 31))

/* Input-only pads 34..39 have no output driver. */
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio_num) \
    (GPIO_IS_VALID_GPIO(gpio_num) && (gpio_num) < 34)

esp_err_t gpio_reset_pin(gpio_num_t gpio_num);

#endif /* MOCK_DRIVER_GPIO_H */