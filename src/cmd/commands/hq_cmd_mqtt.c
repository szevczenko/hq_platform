/**
 *******************************************************************************
 * @file    hq_cmd_mqtt.c
 * @author  Dmytro Shevchenko
 * @brief   CLI commands for MQTT management
 *******************************************************************************
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hq_cmd.h"
#include "mqtt_app.h"
#include "mqtt_config.h"

static bool g_mqtt_initialized = false;
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
      if (i < count)
        return hq_cmd_get_token(args, (uint16_t)(i + 1));
      return NULL;
    }
  }
  return NULL;
}

static bool has_option(const char *args, const char *option)
{
  return hq_cmd_find_token(args, option) != 0;
}

static bool read_cert_from_file(const char *path, char *buf, size_t buf_size)
{
  FILE *fp;
  size_t n;

  if (!path || !buf || buf_size < 2)
    return false;

  fp = fopen(path, "rb");
  if (!fp)
    return false;

  n = fread(buf, 1, buf_size - 1, fp);
  (void)fclose(fp);

  if (n == 0)
    return false;

  buf[n] = '\0';
  return true;
}

static void cmd_help(void)
{
  hq_cmd_print("MQTT commands:");
  hq_cmd_print("  mqtt --start                                  Init/start MQTT app");
  hq_cmd_print("  mqtt --stop                                   Deinit MQTT app");
  hq_cmd_print("  mqtt --status                                 Show connection status");
  hq_cmd_print("  mqtt --show                                   Show current config");
  hq_cmd_print("  mqtt --pub <topic> --msg <payload> [--qos n] Publish message");
  hq_cmd_print("  mqtt --set-address <url>                      Set broker url");
  hq_cmd_print("  mqtt --set-user <name>                        Set username");
  hq_cmd_print("  mqtt --set-pass <pass>                        Set password");
  hq_cmd_print("  mqtt --set-client-id <id>                     Set client id");
  hq_cmd_print("  mqtt --set-prefix <topic>                     Set prefix topic");
  hq_cmd_print("  mqtt --set-post-topic <topic>                 Set post topic");
  hq_cmd_print("  mqtt --set-ssl <0|1>                          Set SSL usage");
  hq_cmd_print("  mqtt --set-skip-verify <0|1>                  Skip TLS cert verification");
  hq_cmd_print("  mqtt --set-cert <filepath>                    Load CA cert from file");
  hq_cmd_print("  mqtt --set-client-cert <filepath>             Load client cert from file");
  hq_cmd_print("  mqtt --set-client-key <filepath>              Load client key from file");
  hq_cmd_print("  mqtt --save                                   Save mqtt.json + cert file");
}

static void cmd_show(void)
{
  bool ssl = false;
  bool skip_verify = false;

  (void)mqtt_config_get_bool(&ssl, MQTT_CONFIG_VALUE_SSL);
  (void)mqtt_config_get_bool(&skip_verify, MQTT_CONFIG_VALUE_SKIP_VERIFY);

  print_line("address:     %s", mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS));
  print_line("username:    %s", mqtt_config_get_string(MQTT_CONFIG_VALUE_USERNAME));
  print_line("password:    %s",
             strlen(mqtt_config_get_string(MQTT_CONFIG_VALUE_PASSWORD)) > 0
               ? "****" : "(empty)");
  print_line("client_id:   %s", mqtt_config_get_string(MQTT_CONFIG_VALUE_CLIENT_ID));
  print_line("prefix:      %s", mqtt_config_get_string(MQTT_CONFIG_VALUE_TOPIC_PREFIX));
  print_line("post topic:  %s", mqtt_config_get_string(MQTT_CONFIG_VALUE_POST_DATA_TOPIC));
  print_line("ssl:         %s", ssl ? "enabled" : "disabled");
  print_line("skip_verify: %s", skip_verify ? "yes" : "no");
  print_line("cert:        %s",
             strlen(mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT)) > 0
               ? "loaded" : "empty");
}

static void cmd_set_cert_from_file(const char *path, mqtt_config_value_t key,
                                   const char *label)
{
  static char cert_buf[MQTT_CERT_MAX_SIZE];

  if (!read_cert_from_file(path, cert_buf, sizeof(cert_buf))) {
    print_line("Failed to read file: %s", path);
    return;
  }
  (void)mqtt_config_set_cert(cert_buf, strlen(cert_buf), 0, key);
  print_line("%s loaded from %s", label, path);
}

static void cmd_set(const char *args)
{
  const char *value = NULL;

  if ((value = find_option_value(args, "--set-address"))) {
    (void)mqtt_config_set_string(value, MQTT_CONFIG_VALUE_ADDRESS);
    hq_cmd_print("MQTT address updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-user"))) {
    (void)mqtt_config_set_string(value, MQTT_CONFIG_VALUE_USERNAME);
    hq_cmd_print("MQTT user updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-pass"))) {
    (void)mqtt_config_set_string(value, MQTT_CONFIG_VALUE_PASSWORD);
    hq_cmd_print("MQTT password updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-client-id"))) {
    (void)mqtt_config_set_string(value, MQTT_CONFIG_VALUE_CLIENT_ID);
    hq_cmd_print("MQTT client id updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-prefix"))) {
    (void)mqtt_config_set_string(value, MQTT_CONFIG_VALUE_TOPIC_PREFIX);
    hq_cmd_print("MQTT prefix updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-post-topic"))) {
    (void)mqtt_config_set_string(value, MQTT_CONFIG_VALUE_POST_DATA_TOPIC);
    hq_cmd_print("MQTT post topic updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-ssl"))) {
    bool en = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
    (void)mqtt_config_set_bool(en, MQTT_CONFIG_VALUE_SSL);
    hq_cmd_print("MQTT SSL flag updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-skip-verify"))) {
    bool en = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
    (void)mqtt_config_set_bool(en, MQTT_CONFIG_VALUE_SKIP_VERIFY);
    hq_cmd_print("MQTT skip-verify flag updated.");
    return;
  }
  if ((value = find_option_value(args, "--set-cert"))) {
    cmd_set_cert_from_file(value, MQTT_CONFIG_VALUE_CERT, "CA cert");
    return;
  }
  if ((value = find_option_value(args, "--set-client-cert"))) {
    cmd_set_cert_from_file(value, MQTT_CONFIG_VALUE_CLIENT_CERT, "Client cert");
    return;
  }
  if ((value = find_option_value(args, "--set-client-key"))) {
    cmd_set_cert_from_file(value, MQTT_CONFIG_VALUE_CLIENT_KEY, "Client key");
    return;
  }

  hq_cmd_print("No MQTT set option found.");
}

static void cmd_publish(const char *args)
{
  const char *topic = find_option_value(args, "--pub");
  const char *msg = find_option_value(args, "--msg");
  const char *qos_str = find_option_value(args, "--qos");
  int qos = 0;

  if (topic && !msg) {
    uint16_t pub_pos = hq_cmd_find_token(args, "--pub");
    if (pub_pos != 0) {
      const char *shorthand_msg =
        hq_cmd_get_token(args, (uint16_t)(pub_pos + 2));
      if (shorthand_msg && strncmp(shorthand_msg, "--", 2) != 0)
        msg = shorthand_msg;
    }
  }

  if (!topic || !msg) {
    hq_cmd_print("Usage: mqtt --pub <topic> --msg <payload> [--qos n]");
    hq_cmd_print("   or: mqtt --pub <topic> <payload> [--qos n]");
    return;
  }

  if (qos_str) {
    qos = atoi(qos_str);
    if (qos < 0 || qos > 2) {
      hq_cmd_print("Invalid QoS. Use 0, 1, or 2.");
      return;
    }
  }

  if (!mqtt_app_post_data(topic, msg, qos)) {
    hq_cmd_print("Publish queue failed.");
    return;
  }

  hq_cmd_print("Message queued.");
}

static void hq_cmd_mqtt_handler(hq_cmd_cli_t *cli, char *args, void *context)
{
  (void)cli;
  (void)context;

  if (args == NULL || hq_cmd_get_token_count(args) == 0
      || has_option(args, "--help") || has_option(args, "help")) {
    cmd_help();
    return;
  }

  if (has_option(args, "--start")) {
    mqtt_config_init();
    mqtt_app_init();
    g_mqtt_initialized = true;
    hq_cmd_print("MQTT started.");
  } else if (has_option(args, "--stop")) {
    mqtt_app_deinit();
    g_mqtt_initialized = false;
    hq_cmd_print("MQTT stopped.");
  } else if (has_option(args, "--status")) {
    print_line("Initialized: %s", g_mqtt_initialized ? "yes" : "no");
    print_line("Connected:   %s", mqtt_app_is_connected() ? "yes" : "no");
  } else if (has_option(args, "--show")) {
    cmd_show();
  } else if (has_option(args, "--save")) {
    if (mqtt_config_save())
      hq_cmd_print("MQTT config saved.");
    else
      hq_cmd_print("MQTT config save failed.");
  } else if (has_option(args, "--pub")) {
    cmd_publish(args);
  } else if (has_option(args, "--set-address")
             || has_option(args, "--set-user")
             || has_option(args, "--set-pass")
             || has_option(args, "--set-client-id")
             || has_option(args, "--set-prefix")
             || has_option(args, "--set-post-topic")
             || has_option(args, "--set-ssl")
             || has_option(args, "--set-skip-verify")
             || has_option(args, "--set-cert")
             || has_option(args, "--set-client-cert")
             || has_option(args, "--set-client-key")) {
    cmd_set(args);
  } else {
    hq_cmd_print("Unknown option. Type 'mqtt' for help.");
  }
}

void hq_cmd_mqtt_register(void)
{
  hq_cmd_binding_t binding = {
    .name = "mqtt",
    .help = "MQTT management (type 'mqtt' for sub-commands)",
    .tokenize_args = true,
    .context = NULL,
    .handler = hq_cmd_mqtt_handler,
  };

  (void)hq_cmd_register(&binding);
}