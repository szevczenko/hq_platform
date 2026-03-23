#ifndef OSAL_LOG_H
#define OSAL_LOG_H

#include <stdarg.h>

typedef enum {
    OSAL_LOG_DEBUG,    /**< @brief Detailed debugging information */
    OSAL_LOG_INFO,     /**< @brief General informational messages */
    OSAL_LOG_WARNING,  /**< @brief Warning messages */
    OSAL_LOG_ERROR,     /**< @brief Error messages */
    OSAL_LOG_NONE       /**< @brief No logging */
} osal_log_level_t;

#ifndef OSAL_LOG_LEVEL
#define OSAL_LOG_LEVEL  OSAL_LOG_INFO
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_DEBUG
#define osal_log_debug(...)  osal_log_v("DEBUG", __VA_ARGS__)
#else
#define osal_log_debug(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_INFO
#define osal_log_info(...)  osal_log_v("INFO", __VA_ARGS__)
#else
#define osal_log_info(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_WARNING
#define osal_log_warning(...)  osal_log_v("WARNING", __VA_ARGS__)
#else
#define osal_log_warning(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_ERROR
#define osal_log_error(...)  osal_log_v("ERROR", __VA_ARGS__)
#else
#define osal_log_error(...)  ((void)0)
#endif

void osal_log_v(const char *level, const char *format, va_list args);

#endif /* OSAL_LOG_H */
