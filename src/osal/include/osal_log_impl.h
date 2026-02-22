#ifndef OSAL_LOG_IMPL_H
#define OSAL_LOG_IMPL_H

#include <stdarg.h>

/**
 * @brief Platform-specific print function
 *
 * Must be implemented by each platform in osal_log_impl.c.
 * Called internally by osal_printf().
 *
 * @param[in] format  Printf-style format string
 * @param[in] args    va_list of format arguments
 * @return Number of characters written, or negative on error
 */
int osal_impl_printf(const char *format, va_list args);

#endif /* OSAL_LOG_IMPL_H */
