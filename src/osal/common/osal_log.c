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
    osal_printf("\r\n");
    fflush(stdout);
}

void osal_log_printf(const char *level, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    osal_log_v(level, format, args);
    va_end(args);
}
