#include <stdio.h>

#include "hq_cmd_internal.h"

void hq_cmd_platform_write_char(char c)
{
    (void)putchar((int)c);
}
