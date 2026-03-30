#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hq_config.h"
#include "hq_cmd.h"
#include "hq_cmd_internal.h"

static EmbeddedCli *g_cli = NULL;

static void hq_cmd_on_unknown(EmbeddedCli *cli, CliCommand *command)
{
    (void)cli;

    if (command == NULL || command->name == NULL)
    {
        return;
    }

    hq_cmd_print("Unknown command. Use 'help' to list available commands.\r\n");
}

static void hq_cmd_write_char_adapter(EmbeddedCli *cli, char c)
{
    (void)cli;
    hq_cmd_platform_write_char(c);
}

int32_t hq_cmd_init(void)
{
    if (g_cli != NULL)
    {
        return 0;
    }

    EmbeddedCliConfig *cfg = embeddedCliDefaultConfig();
    cfg->rxBufferSize = (uint16_t)CONFIG_CMD_RX_BUFFER_SIZE;
    cfg->cmdBufferSize = (uint16_t)CONFIG_CMD_BUFFER_SIZE;
    cfg->historyBufferSize = (uint16_t)CONFIG_CMD_HISTORY_BUFFER_SIZE;
    cfg->maxBindingCount = (uint16_t)CONFIG_CMD_MAX_BINDING_COUNT;
    cfg->enableAutoComplete = (CONFIG_CMD_ENABLE_AUTOCOMPLETE != 0);
    cfg->invitation = CONFIG_CMD_INVITATION;

    g_cli = embeddedCliNew(cfg);
    if (g_cli == NULL)
    {
        return -1;
    }

    g_cli->writeChar = hq_cmd_write_char_adapter;
    g_cli->onCommand = hq_cmd_on_unknown;

    hq_cmd_register_builtin_commands();

    return 0;
}

void hq_cmd_deinit(void)
{
    if (g_cli == NULL)
    {
        return;
    }

    embeddedCliFree(g_cli);
    g_cli = NULL;
}

void hq_cmd_process(void)
{
    if (g_cli == NULL)
    {
        return;
    }

    embeddedCliProcess(g_cli);
}

void hq_cmd_receive_char(char c)
{
    if (g_cli == NULL)
    {
        return;
    }

    embeddedCliReceiveChar(g_cli, c);
}

void hq_cmd_print(const char *text)
{
    if (g_cli == NULL || text == NULL)
    {
        return;
    }

    embeddedCliPrint(g_cli, text);
}

int32_t hq_cmd_register_internal(const CliCommandBinding *binding)
{
    if (g_cli == NULL || binding == NULL)
    {
        return -1;
    }

    return embeddedCliAddBinding(g_cli, *binding) ? 0 : -1;
}

int32_t hq_cmd_register(const hq_cmd_binding_t *binding)
{
    if (binding == NULL)
    {
        return -1;
    }

    CliCommandBinding cli_binding = {
        .name = binding->name,
        .help = binding->help,
        .tokenizeArgs = binding->tokenize_args,
        .context = binding->context,
        .binding = binding->handler,
    };

    return hq_cmd_register_internal(&cli_binding);
}

EmbeddedCli *hq_cmd_get_cli(void)
{
    return g_cli;
}
