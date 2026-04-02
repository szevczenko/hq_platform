#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "hq_cmd_internal.h"

void hq_cmd_platform_output_init(void)
{
    /* Make stdout unbuffered so character echo is immediate. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Set stdin non-blocking so read_char returns immediately when idle. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

void hq_cmd_platform_write_char(char c)
{
    (void)putchar((int)c);
}

int hq_cmd_platform_read_char(void)
{
    uint8_t byte;
    if (read(STDIN_FILENO, &byte, 1) == 1)
    {
        return (int)byte;
    }
    return -1;
}
