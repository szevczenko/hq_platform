#include <stdio.h>

#if CONFIG_HQ_PLATFORM_POSIX
#include <termios.h>
#include <unistd.h>
#endif

#include "hq_cmd.h"
#include "hq_config.h"

static void cmd_exit_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)args;
    (void)context;

    hq_cmd_print("CLI will shutdown now...\r\n");
    hq_cmd_stop();
}

static void cmd_status_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)args;
    (void)context;

    hq_cmd_print("cmd_demo status: hq backend active\r\n");
}

static void cmd_echo_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    hq_cmd_print("echo: ");
    if (args != NULL && args[0] != '\0')
    {
        hq_cmd_print(args);
    }
    else
    {
        hq_cmd_print("<empty>");
    }
    hq_cmd_print("\r\n");
}

static int register_demo_commands(void)
{
    hq_cmd_binding_t status_binding = {
        .name = "status",
        .help = "Print demo status",
        .tokenize_args = false,
        .context = NULL,
        .handler = cmd_status_handler,
    };

    hq_cmd_binding_t echo_binding = {
        .name = "echo",
        .help = "Echo provided arguments",
        .tokenize_args = false,
        .context = NULL,
        .handler = cmd_echo_handler,
    };

    hq_cmd_binding_t exit_binding = {
        .name = "exit",
        .help = "Stop CLI and exit",
        .tokenize_args = false,
        .context = NULL,
        .handler = cmd_exit_handler,
    };

    if (hq_cmd_register(&status_binding) != 0)
    {
        return -1;
    }
    if (hq_cmd_register(&echo_binding) != 0)
    {
        return -1;
    }
    if (hq_cmd_register(&exit_binding) != 0)
    {
        return -1;
    }

    return 0;
}

static int main_function(void)
{
#if CONFIG_HQ_PLATFORM_POSIX
    struct termios original_stdin, raw_stdin;
    if (tcgetattr(STDIN_FILENO, &original_stdin) != 0)
    {
        perror("tcgetattr");
        return 1;
    }

    raw_stdin = original_stdin;
    cfmakeraw(&raw_stdin);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_stdin) != 0)
    {
        perror("tcsetattr");
        return 1;
    }
#endif
    printf("\n=============================================\n");
    printf("            CMD Demo Application             \n");
    printf("=============================================\n");

    if (hq_cmd_init() != 0)
    {
#if CONFIG_HQ_PLATFORM_POSIX
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_stdin);
#endif
        printf("Failed to initialize command line\n");
        return 1;
    }

    if (register_demo_commands() != 0)
    {
        printf("Failed to register demo commands\n");
        hq_cmd_deinit();
#if CONFIG_HQ_PLATFORM_POSIX
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_stdin);
#endif
        return 1;
    }

    printf("CLI is running. Type 'help' to list commands, 'exit' to quit.\r\n");
    hq_cmd_process();

    hq_cmd_wait();
    hq_cmd_deinit();
#if CONFIG_HQ_PLATFORM_POSIX
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_stdin);
#endif
    printf("CLI stopped. Exiting.\n");
    return 0;
}

#ifdef CONFIG_HQ_PLATFORM_ESP
void app_main(void)
{
    (void)main_function();
}
#else
int main(void)
{
    return main_function();
}
#endif