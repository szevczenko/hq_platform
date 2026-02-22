#include "osal_log.h"
#include "osal_log_impl.h"
#include <stdio.h>

static int osal_printf(const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = osal_impl_printf(format, args);
    va_end(args);

    return result;
}

static void osal_log_v(const char *level, const char *format, va_list args)
{
    osal_printf("[%s]: ", level);
    osal_impl_printf(format, args);
    osal_printf("\n");
    fflush(stdout);
}

#if OSAL_LOG_LEVEL <= OSAL_LOG_DEBUG
void osal_log_debug(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    osal_log_v("DEBUG", format, args);
    va_end(args);
}
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_INFO
void osal_log_info(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    osal_log_v("INFO", format, args);
    va_end(args);
}
#endif

#if OSAL_LOG_LEVEL <= OSAL_LOG_WARNING
void osal_log_warning(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    osal_log_v("WARNING", format, args);
    va_end(args);
}
#endif

void osal_log_error(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    osal_log_v("ERROR", format, args);
    va_end(args);
}
