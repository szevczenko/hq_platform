#include <stddef.h>

#include "hq_cmd_internal.h"

static void hq_cmd_hello_handler(EmbeddedCli *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    if (args == NULL || embeddedCliGetTokenCount(args) == 0)
    {
        hq_cmd_platform_write_char('H');
        hq_cmd_platform_write_char('e');
        hq_cmd_platform_write_char('l');
        hq_cmd_platform_write_char('l');
        hq_cmd_platform_write_char('o');
        hq_cmd_platform_write_char(' ');
        hq_cmd_platform_write_char('W');
        hq_cmd_platform_write_char('o');
        hq_cmd_platform_write_char('r');
        hq_cmd_platform_write_char('l');
        hq_cmd_platform_write_char('d');
        hq_cmd_platform_write_char('!');
        hq_cmd_platform_write_char('\r');
        hq_cmd_platform_write_char('\n');
        return;
    }

    const char *name = embeddedCliGetToken(args, 1);
    const char *prefix = "Hello ";
    while (*prefix != '\0')
    {
        hq_cmd_platform_write_char(*prefix++);
    }

    while (name != NULL && *name != '\0')
    {
        hq_cmd_platform_write_char(*name++);
    }

    hq_cmd_platform_write_char('!');
    hq_cmd_platform_write_char('\r');
    hq_cmd_platform_write_char('\n');
}

void hq_cmd_register_builtin_commands(void)
{
    CliCommandBinding hello_binding = {
        "hello",
        "Print hello world or hello <name>",
        true,
        NULL,
        hq_cmd_hello_handler
    };

    (void)hq_cmd_register_internal(&hello_binding);
}
