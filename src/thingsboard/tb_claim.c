/**
 *******************************************************************************
 * @file    tb_claim.c
 * @brief   ThingsBoard client – device claiming implementation
 *******************************************************************************
 */

#include "tb_claim.h"

#include <string.h>

#include "cJSON.h"
#include "osal_log.h"

#define TB_CLAIM_TOPIC "v1/devices/me/claim"

int tb_claim_device(tb_client_t *client, const char *secret_key,
                    uint32_t duration_ms)
{
    if (client == NULL) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    if (secret_key != NULL && secret_key[0] != '\0') {
        cJSON_AddStringToObject(root, "secretKey", secret_key);
    }
    cJSON_AddNumberToObject(root, "durationMs", (double)duration_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -1;
    }

    int ret = tb_client_publish(client, TB_CLAIM_TOPIC, json);
    cJSON_free(json);
    return ret;
}
