#include <stdbool.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

#include "hq_cmd.h"

static bool g_exit_requested = false;

static void cmd_exit_handler(EmbeddedCli *cli, char *args, void *context)
{
    (void)cli;
    (void)args;
    (void)context;

    g_exit_requested = true;
    hq_cmd_print("CLI will shutdown now...");
}

int main(void)
{
    struct termios original_stdin;
    struct termios raw_stdin;
    unsigned char c;

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

    if (hq_cmd_init() != 0)
    {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_stdin);
        printf("Failed to initialize command line\n");
        return 1;
    }

    hq_cmd_binding_t exit_binding = {
        .name = "exit",
        .help = "Stop CLI and exit",
        .tokenize_args = false,
        .context = NULL,
        .handler = cmd_exit_handler,
    };

    (void)hq_cmd_register(&exit_binding);

    printf("Cli is running. Press 'Esc' or type 'exit' to quit\r\n");
    printf("Type \"help\" for a list of commands\r\n");

    hq_cmd_process();

    while (!g_exit_requested)
    {
        if (read(STDIN_FILENO, &c, 1) > 0)
        {
            if (c == 0x1B)
            {
                g_exit_requested = true;
                break;
            }

            hq_cmd_receive_char((char)c);
            hq_cmd_process();
        }
    }

    hq_cmd_deinit();
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_stdin);
    return 0;
}
