/*
 * Test double for ESP-IDF soc/soc_caps.h (TASK-009)
 *
 * Host-side mock describing the LEDC capabilities of a classic ESP32-like
 * target: 8 low-speed channels and 4 timers.  Only the capability used by
 * the ESP PWM backend (SOC_LEDC_SUPPORTED) is modeled; the backend is really
 * only compiled for targets that provide LEDC.
 */

#ifndef MOCK_SOC_CAPS_H
#define MOCK_SOC_CAPS_H

#define SOC_LEDC_SUPPORTED 1

#endif /* MOCK_SOC_CAPS_H */