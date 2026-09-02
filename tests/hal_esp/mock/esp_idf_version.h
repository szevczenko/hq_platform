/*
 * Test double for ESP-IDF esp_idf_version.h (TASK-009)
 *
 * Host-side mock of the ESP-IDF version header used to compile the ESP PWM
 * backend (src/hal/esp/hal_pwm_esp.c) against a LEDC driver test double.
 * The simulated ESP-IDF version is selected at compile time through the
 * HAL_PWM_MOCK_IDF_MAJOR / HAL_PWM_MOCK_IDF_MINOR / HAL_PWM_MOCK_IDF_PATCH
 * definitions (default 5.5.5), mirroring the macros of the real header so
 * the backend's version guards are exercised for every supported IDF
 * release line.
 */

#ifndef MOCK_ESP_IDF_VERSION_H
#define MOCK_ESP_IDF_VERSION_H

#ifndef HAL_PWM_MOCK_IDF_MAJOR
#define HAL_PWM_MOCK_IDF_MAJOR 5
#endif
#ifndef HAL_PWM_MOCK_IDF_MINOR
#define HAL_PWM_MOCK_IDF_MINOR 5
#endif
#ifndef HAL_PWM_MOCK_IDF_PATCH
#define HAL_PWM_MOCK_IDF_PATCH 5
#endif

#define ESP_IDF_VERSION_MAJOR HAL_PWM_MOCK_IDF_MAJOR
#define ESP_IDF_VERSION_MINOR HAL_PWM_MOCK_IDF_MINOR
#define ESP_IDF_VERSION_PATCH HAL_PWM_MOCK_IDF_PATCH

#define ESP_IDF_VERSION_VAL(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))

#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, \
                                            ESP_IDF_VERSION_MINOR, \
                                            ESP_IDF_VERSION_PATCH)

#endif /* MOCK_ESP_IDF_VERSION_H */