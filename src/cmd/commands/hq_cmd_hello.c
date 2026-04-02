#include <stddef.h>

#include "hq_cmd.h"

static void hq_cmd_hello_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    if (args == NULL || hq_cmd_get_token_count(args) == 0)
    {
        hq_cmd_print("Hello World!");
        return;
    }

    const char *name = hq_cmd_get_token(args, 1);
    if (name != NULL)
    {
        hq_cmd_print("Hello ");
        hq_cmd_print(name);
        hq_cmd_print("!");
    }
}

void hq_cmd_register_builtin_commands(void)
{
    hq_cmd_binding_t hello_binding = {
        .name = "hello",
        .help = "Print hello world or hello <name>",
        .tokenize_args = true,
        .context = NULL,
        .handler = hq_cmd_hello_handler,
    };

    (void)hq_cmd_register(&hello_binding);
}
