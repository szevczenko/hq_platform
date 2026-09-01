/**
 * @file hal_types.h
 * @brief Portable HAL public types (TASK-004)
 *
 * Normative public API contract for the hq_platform hardware abstraction
 * layer (HAL).  The types defined here are shared by every HAL subsystem
 * (GPIO, PWM, ...) and are intentionally free of any ESP-IDF, POSIX, Linux
 * or other target-specific dependency.
 *
 * The header is self-contained C99 and exposes only portable values:
 *
 *   - #hal_status_t  - portable result/error type returned by every HAL call,
 *   - #hal_pin_t     - portable pin identifier,
 *   - #hal_polarity_t - portable active-polarity selector.
 *
 * Platform backends (TASK-005 through TASK-010) translate these portable
 * types onto their native resources (register banks, GPIO numbers, LEDC
 * timers and channels, sysfs entries, ...).  Nothing in this header
 * references or exposes such native concepts, so application code never
 * needs a target-specific header to use the HAL.
 */

#ifndef HAL_TYPES_H
#define HAL_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* hal_status_t - portable result/error type                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable HAL result/error type.
 *
 * Every public HAL function returns #hal_status_t.  #HAL_OK (0) signals
 * success; every other value is a portable error code.  Backends never
 * invent new codes: if a native operation fails for a reason not listed
 * here, the backend maps it onto the closest portable code, usually
 * #HAL_ERROR or #HAL_ERR_INTERNAL.
 *
 * The exact integer representation is an implementation choice of this
 * contract: values are stable, so applications and tests may compare
 * against the symbolic names and must not hard-code raw integers other
 * than the documented ones.
 */
typedef enum hal_status {
    HAL_OK                      = 0,   /**< Operation completed successfully. */
    HAL_ERROR                   = -1,  /**< Generic, backend-specific failure. */
    HAL_ERR_INVALID_ARGUMENT    = -2,  /**< Invalid argument (NULL pointer, out-of-range enum value, ...). */
    HAL_ERR_INVALID_PIN         = -3,  /**< Pin identifier is not valid on this platform or equals #HAL_PIN_NONE. */
    HAL_ERR_NOT_INITIALIZED     = -4,  /**< Operation on a pin that was not initialized or was already deinitialized. */
    HAL_ERR_ALREADY_INITIALIZED = -5,  /**< Initialization requested for a pin that is already initialized. */
    HAL_ERR_OUT_OF_RANGE        = -6,  /**< Numeric parameter outside its documented range (e.g. duty cycle). */
    HAL_ERR_NOT_SUPPORTED       = -7,  /**< Requested configuration is not supported by the backend (e.g. frequency/resolution). */
    HAL_ERR_NO_RESOURCE         = -8,  /**< No hardware resource available (timer, channel, pin, ...). */
    HAL_ERR_BUSY                = -9,  /**< Resource is temporarily busy; retry later. */
    HAL_ERR_PERMISSION          = -10, /**< Operation is not permitted in the current state. */
    HAL_ERR_INTERNAL            = -11  /**< Unexpected internal or backend error. */
} hal_status_t;

/* --------------------------------------------------------------------- */
/* hal_pin_t - portable pin identifier                                   */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable pin identifier.
 *
 * An unsigned integer that selects a hardware pin for GPIO or PWM use.
 * The mapping from #hal_pin_t to the physical pin (GPIO number, port and
 * pad, ...) is defined by the platform backend and is an implementation
 * detail.  Applications obtain pin identifiers from board configuration;
 * they must never interpret the numeric value themselves.
 *
 * #HAL_PIN_NONE is the reserved "no pin" sentinel and is never a valid
 * pin on any backend.
 */
typedef uint32_t hal_pin_t;

/** @brief Reserved sentinel meaning "no pin".  Never a valid pin value. */
#define HAL_PIN_NONE ((hal_pin_t)0xFFFFFFFFu)

/* --------------------------------------------------------------------- */
/* hal_polarity_t - active polarity                                      */
/* --------------------------------------------------------------------- */

/**
 * @brief Active polarity of a GPIO or PWM output.
 *
 * All HAL logical values are expressed relative to the configured
 * polarity:
 *
 *   - #HAL_POLARITY_ACTIVE_HIGH: logical ACTIVE  == physical HIGH,
 *                                logical INACTIVE == physical LOW.
 *   - #HAL_POLARITY_ACTIVE_LOW:  logical ACTIVE  == physical LOW,
 *                                logical INACTIVE == physical HIGH.
 *
 * See hal_gpio.h for the full logical/electrical truth table.
 */
typedef enum hal_polarity {
    HAL_POLARITY_ACTIVE_HIGH = 0, /**< Logical ACTIVE maps to physical HIGH. */
    HAL_POLARITY_ACTIVE_LOW  = 1  /**< Logical ACTIVE maps to physical LOW. */
} hal_polarity_t;

#ifdef __cplusplus
}
#endif

#endif /* HAL_TYPES_H */