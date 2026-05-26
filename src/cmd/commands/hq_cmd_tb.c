/**
 *******************************************************************************
 * @file    hq_cmd_tb.c
 * @brief   CLI commands for ThingsBoard client management
 *******************************************************************************
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hq_cmd.h"
#include "tb_client.h"
#include "tb_telemetry.h"
#include "tb_attributes.h"
#include "tb_rpc.h"
#include "tb_provision.h"
#include "tb_claim.h"

static tb_client_t *s_client = NULL;
static char g_out[512];

static void print_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_out, sizeof(g_out), fmt, ap);
    va_end(ap);
    hq_cmd_print(g_out);
}

static const char *find_option_value(const char *args, const char *option)
{
    uint16_t count = hq_cmd_get_token_count(args);
    for (uint16_t i = 1; i <= count; ++i) {
        const char *tok = hq_cmd_get_token(args, i);
        if (tok && strcmp(tok, option) == 0) {
            if (i < count) {
                return hq_cmd_get_token(args, (uint16_t)(i + 1));
            }
            return NULL;
        }
    }
    return NULL;
}

static bool has_option(const char *args, const char *option)
{
    return hq_cmd_find_token(args, option) != 0;
}

/* ── Help ────────────────────────────────────────────────────────────── */

static void cmd_help(void)
{
    hq_cmd_print("ThingsBoard commands:");
    hq_cmd_print("  tb --connect --url <url> --token <token>       Connect to TB");
    hq_cmd_print("  tb --disconnect                                Disconnect");
    hq_cmd_print("  tb --status                                    Show status");
    hq_cmd_print("");
    hq_cmd_print("  tb --telemetry <key> <value> [--type int|double|bool|str]");
    hq_cmd_print("                                                 Send telemetry");
    hq_cmd_print("  tb --telemetry-json <json>                     Send raw JSON telemetry");
    hq_cmd_print("");
    hq_cmd_print("  tb --attr <key> <value> [--type int|double|bool|str]");
    hq_cmd_print("                                                 Send client attribute");
    hq_cmd_print("  tb --attr-json <json>                          Send raw JSON attributes");
    hq_cmd_print("  tb --attr-request --keys <k1,k2,...> [--shared]");
    hq_cmd_print("                                                 Request attribute values");
    hq_cmd_print("");
    hq_cmd_print("  tb --rpc-subscribe                             Subscribe to server RPC");
    hq_cmd_print("  tb --rpc-unsubscribe                           Unsubscribe server RPC");
    hq_cmd_print("  tb --rpc-call <method> [--params <json>]       Client-side RPC");
    hq_cmd_print("");
    hq_cmd_print("  tb --provision --key <dk> --secret <ds> [--name <n>]");
    hq_cmd_print("                                                 Provision device");
    hq_cmd_print("  tb --claim [--secret <key>] --duration <ms>    Claim device");
}

/* ── Connect / Disconnect ────────────────────────────────────────────── */

static void cmd_connect(const char *args)
{
    const char *url = find_option_value(args, "--url");
    const char *token = find_option_value(args, "--token");

    if (!url || !token) {
        hq_cmd_print("Usage: tb --connect --url <url> --token <token>");
        return;
    }

    if (s_client != NULL) {
        hq_cmd_print("Already connected. Disconnect first.");
        return;
    }

    tb_client_config_t cfg = {
        .server_url = url,
        .access_token = token,
        .client_id = NULL,
        .device_name = "cli_device",
    };

    if (tb_client_init(&s_client, &cfg) != 0) {
        hq_cmd_print("Failed to initialize TB client.");
        return;
    }

    if (tb_client_connect(s_client) != 0) {
        hq_cmd_print("Failed to connect.");
        tb_client_deinit(s_client);
        s_client = NULL;
        return;
    }

    hq_cmd_print("ThingsBoard client connected.");
}

static void cmd_disconnect(void)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected.");
        return;
    }

    tb_client_deinit(s_client);
    s_client = NULL;
    hq_cmd_print("ThingsBoard client disconnected.");
}

static void cmd_status(void)
{
    if (s_client == NULL) {
        hq_cmd_print("ThingsBoard client: not initialized");
        return;
    }
    print_line("ThingsBoard client: %s",
               tb_client_is_connected(s_client) ? "connected" : "disconnected");
}

/* ── Type detection / parsing helpers ────────────────────────────────── */

typedef enum {
    VAL_TYPE_STR,
    VAL_TYPE_INT,
    VAL_TYPE_DOUBLE,
    VAL_TYPE_BOOL,
} val_type_t;

static val_type_t detect_type(const char *value)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        return VAL_TYPE_BOOL;
    }

    /* Check if it's a number */
    char *end = NULL;
    (void)strtol(value, &end, 10);
    if (*end == '\0' && end != value) {
        return VAL_TYPE_INT;
    }
    (void)strtod(value, &end);
    if (*end == '\0' && end != value) {
        return VAL_TYPE_DOUBLE;
    }

    return VAL_TYPE_STR;
}

static val_type_t parse_type_option(const char *args)
{
    const char *type_str = find_option_value(args, "--type");
    if (type_str == NULL) {
        return VAL_TYPE_STR; /* marker for "auto-detect" */
    }
    if (strcmp(type_str, "int") == 0) return VAL_TYPE_INT;
    if (strcmp(type_str, "double") == 0) return VAL_TYPE_DOUBLE;
    if (strcmp(type_str, "bool") == 0) return VAL_TYPE_BOOL;
    return VAL_TYPE_STR;
}

/* ── Telemetry ───────────────────────────────────────────────────────── */

static void cmd_telemetry(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    /* tb --telemetry <key> <value> [--type ...] */
    uint16_t pos = hq_cmd_find_token(args, "--telemetry");
    if (pos == 0) return;

    const char *key = hq_cmd_get_token(args, (uint16_t)(pos + 1));
    const char *value = hq_cmd_get_token(args, (uint16_t)(pos + 2));

    if (!key || !value || strncmp(key, "--", 2) == 0) {
        hq_cmd_print("Usage: tb --telemetry <key> <value> [--type int|double|bool|str]");
        return;
    }

    val_type_t type = parse_type_option(args);
    /* If no explicit type given, auto-detect */
    if (!find_option_value(args, "--type")) {
        type = detect_type(value);
    }

    int ret = -1;
    switch (type) {
    case VAL_TYPE_INT:
        ret = tb_telemetry_send_int(s_client, key, (int64_t)strtoll(value, NULL, 10));
        break;
    case VAL_TYPE_DOUBLE:
        ret = tb_telemetry_send_double(s_client, key, strtod(value, NULL));
        break;
    case VAL_TYPE_BOOL:
        ret = tb_telemetry_send_bool(s_client, key, strcmp(value, "true") == 0);
        break;
    case VAL_TYPE_STR:
    default:
        ret = tb_telemetry_send_string(s_client, key, value);
        break;
    }

    if (ret == 0) {
        print_line("Telemetry sent: %s = %s", key, value);
    } else {
        hq_cmd_print("Failed to send telemetry.");
    }
}

static void cmd_telemetry_json(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    const char *json = find_option_value(args, "--telemetry-json");
    if (!json) {
        hq_cmd_print("Usage: tb --telemetry-json <json>");
        return;
    }

    int ret = tb_telemetry_send_json(s_client, json);
    if (ret == 0) {
        hq_cmd_print("Raw JSON telemetry sent.");
    } else {
        hq_cmd_print("Failed to send telemetry.");
    }
}

/* ── Attributes ──────────────────────────────────────────────────────── */

static void cmd_attr(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    uint16_t pos = hq_cmd_find_token(args, "--attr");
    if (pos == 0) return;

    const char *key = hq_cmd_get_token(args, (uint16_t)(pos + 1));
    const char *value = hq_cmd_get_token(args, (uint16_t)(pos + 2));

    if (!key || !value || strncmp(key, "--", 2) == 0) {
        hq_cmd_print("Usage: tb --attr <key> <value> [--type int|double|bool|str]");
        return;
    }

    val_type_t type = parse_type_option(args);
    if (!find_option_value(args, "--type")) {
        type = detect_type(value);
    }

    int ret = -1;
    switch (type) {
    case VAL_TYPE_INT:
        ret = tb_attributes_send_int(s_client, key, (int64_t)strtoll(value, NULL, 10));
        break;
    case VAL_TYPE_DOUBLE:
        ret = tb_attributes_send_double(s_client, key, strtod(value, NULL));
        break;
    case VAL_TYPE_BOOL:
        ret = tb_attributes_send_bool(s_client, key, strcmp(value, "true") == 0);
        break;
    case VAL_TYPE_STR:
    default:
        ret = tb_attributes_send_string(s_client, key, value);
        break;
    }

    if (ret == 0) {
        print_line("Attribute sent: %s = %s", key, value);
    } else {
        hq_cmd_print("Failed to send attribute.");
    }
}

static void cmd_attr_json(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    const char *json = find_option_value(args, "--attr-json");
    if (!json) {
        hq_cmd_print("Usage: tb --attr-json <json>");
        return;
    }

    int ret = tb_attributes_send_json(s_client, json);
    if (ret == 0) {
        hq_cmd_print("Raw JSON attributes sent.");
    } else {
        hq_cmd_print("Failed to send attributes.");
    }
}

static void attr_response_print(const char *json_response, void *user_data)
{
    (void)user_data;
    print_line("Attribute response: %s", json_response ? json_response : "(null)");
}

static void cmd_attr_request(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    const char *keys_str = find_option_value(args, "--keys");
    if (!keys_str) {
        hq_cmd_print("Usage: tb --attr-request --keys <k1,k2,...> [--shared]");
        return;
    }

    /* Split comma-separated keys */
    char keys_buf[256];
    strncpy(keys_buf, keys_str, sizeof(keys_buf) - 1);
    keys_buf[sizeof(keys_buf) - 1] = '\0';

    const char *key_ptrs[16];
    size_t num_keys = 0;
    char *tok = strtok(keys_buf, ",");
    while (tok && num_keys < 16) {
        key_ptrs[num_keys++] = tok;
        tok = strtok(NULL, ",");
    }

    if (num_keys == 0) {
        hq_cmd_print("No keys specified.");
        return;
    }

    int ret;
    if (has_option(args, "--shared")) {
        ret = tb_attributes_request_shared(s_client, key_ptrs, num_keys,
                                           attr_response_print, NULL, 5000);
    } else {
        ret = tb_attributes_request_client(s_client, key_ptrs, num_keys,
                                           attr_response_print, NULL, 5000);
    }

    if (ret == 0) {
        hq_cmd_print("Attribute request sent.");
    } else {
        hq_cmd_print("Failed to send attribute request.");
    }
}

/* ── RPC ─────────────────────────────────────────────────────────────── */

static void server_rpc_print(const char *method, const char *params_json,
                             uint32_t request_id, void *user_data)
{
    (void)user_data;
    print_line("RPC request [id=%u]: method=%s params=%s",
               request_id, method, params_json);
    /* Auto-respond with empty success */
    if (s_client) {
        tb_rpc_respond(s_client, request_id, "{}");
        print_line("RPC response sent [id=%u]: {}", request_id);
    }
}

static void cmd_rpc_subscribe(void)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    int ret = tb_rpc_subscribe_server(s_client, server_rpc_print, NULL);
    if (ret == 0) {
        hq_cmd_print("Subscribed to server-side RPC.");
    } else {
        hq_cmd_print("Failed to subscribe to RPC.");
    }
}

static void cmd_rpc_unsubscribe(void)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    int ret = tb_rpc_unsubscribe_server(s_client);
    if (ret == 0) {
        hq_cmd_print("Unsubscribed from server-side RPC.");
    } else {
        hq_cmd_print("Failed to unsubscribe from RPC.");
    }
}

static void client_rpc_print(const char *response_json, void *user_data)
{
    (void)user_data;
    print_line("RPC response: %s", response_json ? response_json : "(null)");
}

static void cmd_rpc_call(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    const char *method = find_option_value(args, "--rpc-call");
    if (!method) {
        hq_cmd_print("Usage: tb --rpc-call <method> [--params <json>]");
        return;
    }

    const char *params = find_option_value(args, "--params");

    int ret = tb_rpc_request(s_client, method, params, client_rpc_print, NULL, 5000);
    if (ret == 0) {
        print_line("RPC request sent: %s", method);
    } else {
        hq_cmd_print("Failed to send RPC request.");
    }
}

/* ── Provisioning ────────────────────────────────────────────────────── */

static void provision_response_print(const char *response_json, void *user_data)
{
    (void)user_data;
    print_line("Provision response: %s", response_json ? response_json : "(null)");
}

static void cmd_provision(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect --url <url> --token provision");
        return;
    }

    const char *key = find_option_value(args, "--key");
    const char *secret = find_option_value(args, "--secret");
    const char *name = find_option_value(args, "--name");

    if (!key || !secret) {
        hq_cmd_print("Usage: tb --provision --key <dk> --secret <ds> [--name <n>]");
        return;
    }

    tb_provision_request_t req = {
        .device_name = name,
        .provision_device_key = key,
        .provision_device_secret = secret,
        .credentials_type = NULL,
        .token = NULL,
        .username = NULL,
        .password = NULL,
        .client_id = NULL,
        .certificate_hash = NULL,
    };

    int ret = tb_provision_request(s_client, &req, provision_response_print, NULL, 10000);
    if (ret == 0) {
        hq_cmd_print("Provision request sent.");
    } else {
        hq_cmd_print("Failed to send provision request.");
    }
}

/* ── Claiming ────────────────────────────────────────────────────────── */

static void cmd_claim(const char *args)
{
    if (s_client == NULL) {
        hq_cmd_print("Not connected. Use: tb --connect ...");
        return;
    }

    const char *duration_str = find_option_value(args, "--duration");
    if (!duration_str) {
        hq_cmd_print("Usage: tb --claim [--secret <key>] --duration <ms>");
        return;
    }

    const char *secret = find_option_value(args, "--secret");
    uint32_t duration_ms = (uint32_t)strtoul(duration_str, NULL, 10);

    int ret = tb_claim_device(s_client, secret, duration_ms);
    if (ret == 0) {
        print_line("Claim request sent (duration=%u ms).", duration_ms);
    } else {
        hq_cmd_print("Failed to send claim request.");
    }
}

/* ── Main handler ────────────────────────────────────────────────────── */

static void hq_cmd_tb_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
    (void)cli;
    (void)context;

    if (args == NULL || hq_cmd_get_token_count(args) == 0
        || has_option(args, "--help") || has_option(args, "help")) {
        cmd_help();
        return;
    }

    if (has_option(args, "--connect")) {
        cmd_connect(args);
    } else if (has_option(args, "--disconnect")) {
        cmd_disconnect();
    } else if (has_option(args, "--status")) {
        cmd_status();
    } else if (has_option(args, "--telemetry-json")) {
        cmd_telemetry_json(args);
    } else if (has_option(args, "--telemetry")) {
        cmd_telemetry(args);
    } else if (has_option(args, "--attr-json")) {
        cmd_attr_json(args);
    } else if (has_option(args, "--attr-request")) {
        cmd_attr_request(args);
    } else if (has_option(args, "--attr")) {
        cmd_attr(args);
    } else if (has_option(args, "--rpc-subscribe")) {
        cmd_rpc_subscribe();
    } else if (has_option(args, "--rpc-unsubscribe")) {
        cmd_rpc_unsubscribe();
    } else if (has_option(args, "--rpc-call")) {
        cmd_rpc_call(args);
    } else if (has_option(args, "--provision")) {
        cmd_provision(args);
    } else if (has_option(args, "--claim")) {
        cmd_claim(args);
    } else {
        hq_cmd_print("Unknown option. Type 'tb' for help.");
    }
}

void hq_cmd_tb_register(void)
{
    hq_cmd_binding_t binding = {
        .name = "tb",
        .help = "ThingsBoard client (type 'tb' for sub-commands)",
        .tokenize_args = true,
        .context = NULL,
        .handler = hq_cmd_tb_handler,
    };

    (void)hq_cmd_register(&binding);
}
