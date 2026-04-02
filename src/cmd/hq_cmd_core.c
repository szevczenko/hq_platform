#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hq_config.h"
#include "hq_cmd.h"
#include "hq_cmd_internal.h"
#include "osal_task.h"
#include "osal_bin_sem.h"
#include "osal_log.h"

static osal_task_id_t    g_input_task_id;
static osal_bin_sem_id_t g_stop_done_sem;
static osal_bin_sem_id_t g_stop_request_sem;

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

static void hq_cmd_input_task(void *arg)
{
    (void)arg;

    while (true)
    {
        if (osal_bin_sem_timed_wait(g_stop_request_sem, 0) == OSAL_SUCCESS)
        {
            break;
        }

        int c = hq_cmd_platform_read_char();
        if (c >= 0)
        {
            hq_cmd_receive_char((char)c);
            hq_cmd_process();
        }
        else
        {
            osal_task_delay_ms(10);
        }
    }

    (void)osal_bin_sem_give(g_stop_done_sem);
    (void)osal_task_delete(g_input_task_id);
}

int32_t hq_cmd_init(void)
{
    osal_status_t status;
    if (g_cli != NULL)
    {
        return 0;
    }

    hq_cmd_platform_output_init();

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
        osal_log_error("Failed to create CLI instance");
        return -1;
    }

    g_cli->writeChar = hq_cmd_write_char_adapter;
    g_cli->onCommand = hq_cmd_on_unknown;

    hq_cmd_register_builtin_commands();

    if ((status = osal_bin_sem_create(&g_stop_done_sem, "cmd_stop_done", OSAL_SEM_EMPTY)) != OSAL_SUCCESS)
    {
        osal_log_error("Failed to create stop done semaphore %s", osal_get_status_name(status));
        embeddedCliFree(g_cli);
        g_cli = NULL;
        return -1;
    }

    if ((status = osal_bin_sem_create(&g_stop_request_sem, "cmd_stop_req", OSAL_SEM_EMPTY)) != OSAL_SUCCESS)
    {
        osal_log_error("Failed to create stop request semaphore %s", osal_get_status_name(status));
        (void)osal_bin_sem_delete(g_stop_done_sem);
        embeddedCliFree(g_cli);
        g_cli = NULL;
        return -1;
    }

    osal_task_attr_t attr;
    (void)osal_task_attributes_init(&attr);

    if ((status = osal_task_create(&g_input_task_id, "cmd_input",
                         hq_cmd_input_task, NULL, NULL,
                         (size_t)CONFIG_CMD_INPUT_TASK_STACK_SIZE,
                         (osal_priority_t)CONFIG_CMD_INPUT_TASK_PRIORITY,
                         &attr)) != OSAL_SUCCESS)
    {
        osal_log_error("Failed to create input task %s", osal_get_status_name(status));
        (void)osal_bin_sem_delete(g_stop_request_sem);
        (void)osal_bin_sem_delete(g_stop_done_sem);
        embeddedCliFree(g_cli);
        g_cli = NULL;
        return -1;
    }

    return 0;
}

void hq_cmd_deinit(void)
{
    if (g_cli == NULL)
    {
        return;
    }

    hq_cmd_stop();
    (void)osal_bin_sem_take(g_stop_done_sem);
    (void)osal_bin_sem_delete(g_stop_request_sem);
    (void)osal_bin_sem_delete(g_stop_done_sem);

    embeddedCliFree(g_cli);
    g_cli = NULL;
}

void hq_cmd_stop(void)
{
    (void)osal_bin_sem_give(g_stop_request_sem);
}

void hq_cmd_wait(void)
{
    if (g_cli == NULL)
    {
        return;
    }

    /* Block until the input task signals it has stopped. */
    (void)osal_bin_sem_take(g_stop_done_sem);

    /* Re-give so deinit can also take it. */
    (void)osal_bin_sem_give(g_stop_done_sem);
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

hq_cmd_cli_t *hq_cmd_get_cli(void)
{
    return g_cli;
}

/* ── Argument helper wrappers ──────────────────────────────────────── */

void hq_cmd_tokenize_args(char *args)
{
    embeddedCliTokenizeArgs(args);
}

uint16_t hq_cmd_get_token_count(const char *args)
{
    return embeddedCliGetTokenCount(args);
}

const char *hq_cmd_get_token(const char *args, uint16_t pos)
{
    return embeddedCliGetToken(args, pos);
}

char *hq_cmd_get_token_variable(char *args, uint16_t pos)
{
    return embeddedCliGetTokenVariable(args, pos);
}

uint16_t hq_cmd_find_token(const char *args, const char *token)
{
    return embeddedCliFindToken(args, token);
}
