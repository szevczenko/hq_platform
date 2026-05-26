/**
 *******************************************************************************
 * @file    tb_telemetry.c
 * @brief   ThingsBoard client – telemetry implementation
 *******************************************************************************
 */

#include "tb_telemetry.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "osal_log.h"

#define TB_TELEMETRY_TOPIC "v1/devices/me/telemetry"

int tb_telemetry_send_int(tb_client_t *client, const char *key, int64_t value)
{
    if (client == NULL || key == NULL) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    /* cJSON uses double internally for numbers; for large int64 this is lossy,
       but matches ThingsBoard's JSON number handling */
    cJSON_AddNumberToObject(root, key, (double)value);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -1;
    }

    int ret = tb_client_publish(client, TB_TELEMETRY_TOPIC, json);
    cJSON_free(json);
    return ret;
}

int tb_telemetry_send_double(tb_client_t *client, const char *key, double value)
{
    if (client == NULL || key == NULL) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    cJSON_AddNumberToObject(root, key, value);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -1;
    }

    int ret = tb_client_publish(client, TB_TELEMETRY_TOPIC, json);
    cJSON_free(json);
    return ret;
}

int tb_telemetry_send_bool(tb_client_t *client, const char *key, bool value)
{
    if (client == NULL || key == NULL) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    cJSON_AddBoolToObject(root, key, value);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -1;
    }

    int ret = tb_client_publish(client, TB_TELEMETRY_TOPIC, json);
    cJSON_free(json);
    return ret;
}

int tb_telemetry_send_string(tb_client_t *client, const char *key,
                             const char *value)
{
    if (client == NULL || key == NULL || value == NULL) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    cJSON_AddStringToObject(root, key, value);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -1;
    }

    int ret = tb_client_publish(client, TB_TELEMETRY_TOPIC, json);
    cJSON_free(json);
    return ret;
}

int tb_telemetry_send_json(tb_client_t *client, const char *json)
{
    if (client == NULL || json == NULL) {
        return -1;
    }
    return tb_client_publish(client, TB_TELEMETRY_TOPIC, json);
}
