#ifndef OSAL_LOG_H
#define OSAL_LOG_H

#include <stdarg.h>

typedef enum {
    OSAL_LOG_DEBUG,    /**< @brief Detailed debugging information */
    OSAL_LOG_INFO,     /**< @brief General informational messages */
    OSAL_LOG_WARNING,  /**< @brief Warning messages */
    OSAL_LOG_ERROR     /**< @brief Error messages */
} osal_log_level_t;

#ifndef OSAL_LOG_LEVEL
#define OSAL_LOG_LEVEL  OSAL_LOG_INFO
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_DEBUG
void osal_log_debug(const char *format, ...);
#else
#define osal_log_debug(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_INFO
void osal_log_info(const char *format, ...);
#else
#define osal_log_info(...)  ((void)0)
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_WARNING
void osal_log_warning(const char *format, ...);
#else
#define osal_log_warning(...)  ((void)0)
#endif

void osal_log_error(const char *format, ...);

#endif /* OSAL_LOG_H */
