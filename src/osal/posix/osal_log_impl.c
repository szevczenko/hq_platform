#include <stdio.h>

#include "osal_log_impl.h"

int osal_impl_printf(const char *format, va_list args)
{
    return vprintf(format, args);
}
