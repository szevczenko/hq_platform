#ifndef OSAL_TIME_H
#define OSAL_TIME_H

#include <stdint.h>

/**
 * @brief Time representation similar to POSIX struct timespec
 *
 * Provides a portable time structure with seconds and nanoseconds
 * resolution, analogous to struct timespec.
 */
typedef struct
{
    int64_t tv_sec;  /**< Seconds */
    int64_t tv_nsec; /**< Nanoseconds (0 to 999999999) */
} osal_time_t;

/**
 * @brief Get the total number of seconds from an osal_time_t value
 *
 * @param[in] t  Time value
 *
 * @return Total seconds as an integer
 */
static inline int64_t osal_time_get_total_seconds(osal_time_t t)
{
    return t.tv_sec;
}

#endif /* OSAL_TIME_H */
