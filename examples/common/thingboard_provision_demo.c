/*
 * ThingsBoard Provision Demo
 *
 * Requests credentials using provisioning key/secret, validates response,
 * stores credentials using OSAL file APIs, and reconnects using persisted
 * credentials when supported by current client config surface.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "mongoose_process.h"
#include "mqtt_config.h"
#include "osal_file.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_client.h"
#include "tb_provision.h"
#include "tb_telemetry.h"

#ifndef CONFIG_PROV_DEMO_BOOTSTRAP_CLIENT_ID
#define CONFIG_PROV_DEMO_BOOTSTRAP_CLIENT_ID "tb_provision_demo_bootstrap"
#endif
#ifndef CONFIG_PROV_DEMO_BOOTSTRAP_USERNAME
#define CONFIG_PROV_DEMO_BOOTSTRAP_USERNAME "provision"
#endif
#ifndef CONFIG_PROV_DEMO_BOOTSTRAP_PASSWORD
#define CONFIG_PROV_DEMO_BOOTSTRAP_PASSWORD ""
#endif
#ifndef CONFIG_PROV_DEMO_BOOTSTRAP_DEVICE_NAME
#define CONFIG_PROV_DEMO_BOOTSTRAP_DEVICE_NAME "Provision Bootstrap"
#endif
#ifndef CONFIG_PROV_DEMO_MQTT_URL
#define CONFIG_PROV_DEMO_MQTT_URL "mqtt://localhost:1883"
#endif
#ifndef CONFIG_PROV_DEMO_PROVISION_KEY
#define CONFIG_PROV_DEMO_PROVISION_KEY "PUT_PROVISION_KEY_HERE"
#endif
#ifndef CONFIG_PROV_DEMO_PROVISION_SECRET
#define CONFIG_PROV_DEMO_PROVISION_SECRET "PUT_PROVISION_SECRET_HERE"
#endif
#ifndef CONFIG_PROV_DEMO_DEVICE_NAME
#define CONFIG_PROV_DEMO_DEVICE_NAME "Provisioned Device Demo"
#endif
#ifndef CONFIG_PROV_DEMO_STORAGE_PATH
#define CONFIG_PROV_DEMO_STORAGE_PATH "tb_provisioned_credentials.json"
#endif
#ifndef CONFIG_PROV_DEMO_REQUEST_TIMEOUT_MS
#define CONFIG_PROV_DEMO_REQUEST_TIMEOUT_MS 10000
#endif
#ifndef CONFIG_PROV_DEMO_RECONNECT_DELAY_MS
#define CONFIG_PROV_DEMO_RECONNECT_DELAY_MS 3000
#endif

#define PROV_BOOTSTRAP_CLIENT_ID CONFIG_PROV_DEMO_BOOTSTRAP_CLIENT_ID
#define PROV_BOOTSTRAP_USERNAME CONFIG_PROV_DEMO_BOOTSTRAP_USERNAME
#define PROV_BOOTSTRAP_PASSWORD CONFIG_PROV_DEMO_BOOTSTRAP_PASSWORD
#define PROV_BOOTSTRAP_DEVICE_NAME CONFIG_PROV_DEMO_BOOTSTRAP_DEVICE_NAME
#define PROV_MQTT_URL CONFIG_PROV_DEMO_MQTT_URL
#define PROV_KEY CONFIG_PROV_DEMO_PROVISION_KEY
#define PROV_SECRET CONFIG_PROV_DEMO_PROVISION_SECRET
#define PROV_DEVICE_NAME CONFIG_PROV_DEMO_DEVICE_NAME
#define PROV_STORAGE_PATH CONFIG_PROV_DEMO_STORAGE_PATH
#define PROV_REQ_TIMEOUT_MS CONFIG_PROV_DEMO_REQUEST_TIMEOUT_MS
#define PROV_RECONNECT_DELAY_MS CONFIG_PROV_DEMO_RECONNECT_DELAY_MS

#define PROV_CRED_TYPE_ACCESS_TOKEN "ACCESS_TOKEN"
#define PROV_CRED_TYPE_MQTT_BASIC "MQTT_BASIC"
#define PROV_CRED_TYPE_X509 "X509_CERTIFICATE"

typedef struct {
    char credentials_type[32];
    char access_token[128];
    char username[128];
    char password[128];
    char client_id[128];
    char certificate_hash[256];
} provisioned_credentials_t;

static tb_client_t *g_tb_client;
static bool g_provision_response_ready = false;
static bool g_provision_response_valid = false;
static provisioned_credentials_t g_provisioned = { 0 };

static int connect_and_wait(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[PROV_DEMO] connect request failed: %d\n", rc);
        return rc;
    }

    for (int i = 0; i < 20; i++) {
        if (tb_client_is_connected(g_tb_client)) {
            return 0;
        }
        osal_task_delay_ms(250);
    }

    tb_client_disconnect(g_tb_client);
    return -1;
}

static bool parse_provision_response(const char *response_json,
                                     provisioned_credentials_t *out)
{
    if (response_json == NULL || out == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        return false;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "credentialsType");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return false;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->credentials_type, type->valuestring,
            sizeof(out->credentials_type) - 1);

    if (strcmp(out->credentials_type, PROV_CRED_TYPE_ACCESS_TOKEN) == 0) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "credentialsValue");
        if (!cJSON_IsString(value) || value->valuestring == NULL ||
            value->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return false;
        }
        strncpy(out->access_token, value->valuestring,
                sizeof(out->access_token) - 1);
    } else if (strcmp(out->credentials_type, PROV_CRED_TYPE_MQTT_BASIC) == 0) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "credentialsValue");
        if (!cJSON_IsString(value) || value->valuestring == NULL) {
            cJSON_Delete(root);
            return false;
        }

        cJSON *basic = cJSON_Parse(value->valuestring);
        if (basic == NULL) {
            cJSON_Delete(root);
            return false;
        }

        cJSON *username = cJSON_GetObjectItemCaseSensitive(basic, "username");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(basic, "password");
        cJSON *client_id = cJSON_GetObjectItemCaseSensitive(basic, "clientId");

        if (!cJSON_IsString(username) || !cJSON_IsString(password) ||
            !cJSON_IsString(client_id) || username->valuestring == NULL ||
            password->valuestring == NULL || client_id->valuestring == NULL) {
            cJSON_Delete(basic);
            cJSON_Delete(root);
            return false;
        }

        strncpy(out->username, username->valuestring,
                sizeof(out->username) - 1);
        strncpy(out->password, password->valuestring,
                sizeof(out->password) - 1);
        strncpy(out->client_id, client_id->valuestring,
                sizeof(out->client_id) - 1);

        cJSON_Delete(basic);
    } else if (strcmp(out->credentials_type, PROV_CRED_TYPE_X509) == 0) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "credentialsValue");
        if (!cJSON_IsString(value) || value->valuestring == NULL ||
            value->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return false;
        }

        strncpy(out->certificate_hash, value->valuestring,
                sizeof(out->certificate_hash) - 1);
    } else {
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

static int save_credentials(const char *path, const provisioned_credentials_t *creds)
{
    osal_file_id_t fd;
    int ret = -1;
    cJSON *root = NULL;
    char *payload = NULL;

    if (path == NULL || creds == NULL) {
        return -1;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    if (!cJSON_AddStringToObject(root, "credentialsType", creds->credentials_type) ||
        !cJSON_AddStringToObject(root, "accessToken", creds->access_token) ||
        !cJSON_AddStringToObject(root, "username", creds->username) ||
        !cJSON_AddStringToObject(root, "password", creds->password) ||
        !cJSON_AddStringToObject(root, "clientId", creds->client_id) ||
        !cJSON_AddStringToObject(root, "certificateHash", creds->certificate_hash)) {
        cJSON_Delete(root);
        return -1;
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return -1;
    }

    fd = osal_open_create(path,
                          OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
                          OSAL_WRITE_ONLY);
    if (fd < 0) {
        cJSON_free(payload);
        return -1;
    }

    ret = osal_write(fd, payload, strlen(payload));
    (void)osal_close(fd);
    cJSON_free(payload);
    return (ret >= 0) ? 0 : -1;
}

static int load_credentials(const char *path, provisioned_credentials_t *creds)
{
    osal_file_id_t fd;
    char buf[768];
    int32_t rd;

    if (path == NULL || creds == NULL) {
        return -1;
    }

    fd = osal_open_create(path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    if (fd < 0) {
        return -1;
    }

    rd = osal_read(fd, buf, sizeof(buf) - 1);
    (void)osal_close(fd);
    if (rd <= 0) {
        return -1;
    }

    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return -1;
    }

    memset(creds, 0, sizeof(*creds));

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "credentialsType");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
    cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *client_id = cJSON_GetObjectItemCaseSensitive(root, "clientId");
    cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "certificateHash");

    if (cJSON_IsString(type) && type->valuestring) {
        strncpy(creds->credentials_type, type->valuestring,
                sizeof(creds->credentials_type) - 1);
    }
    if (cJSON_IsString(token) && token->valuestring) {
        strncpy(creds->access_token, token->valuestring,
                sizeof(creds->access_token) - 1);
    }
    if (cJSON_IsString(username) && username->valuestring) {
        strncpy(creds->username, username->valuestring,
                sizeof(creds->username) - 1);
    }
    if (cJSON_IsString(password) && password->valuestring) {
        strncpy(creds->password, password->valuestring,
                sizeof(creds->password) - 1);
    }
    if (cJSON_IsString(client_id) && client_id->valuestring) {
        strncpy(creds->client_id, client_id->valuestring,
                sizeof(creds->client_id) - 1);
    }
    if (cJSON_IsString(hash) && hash->valuestring) {
        strncpy(creds->certificate_hash, hash->valuestring,
                sizeof(creds->certificate_hash) - 1);
    }

    cJSON_Delete(root);
    return 0;
}

static void on_provision_response(const char *response_json, void *user_data)
{
    (void)user_data;

    g_provision_response_ready = true;
    g_provision_response_valid = parse_provision_response(response_json, &g_provisioned);
}

static int provision_once(void)
{
    tb_provision_request_t req = {
        .device_name = PROV_DEVICE_NAME,
        .provision_device_key = PROV_KEY,
        .provision_device_secret = PROV_SECRET,
        .credentials_type = NULL,
    };

    g_provision_response_ready = false;
    g_provision_response_valid = false;
    memset(&g_provisioned, 0, sizeof(g_provisioned));

    int rc = tb_provision_request(g_tb_client, &req,
                                  on_provision_response, NULL,
                                  PROV_REQ_TIMEOUT_MS);
    if (rc != 0) {
        return rc;
    }

    uint32_t start = osal_task_get_time_ms();
    while (!g_provision_response_ready) {
        if ((uint32_t)(osal_task_get_time_ms() - start) >= PROV_REQ_TIMEOUT_MS) {
            return -1;
        }
        osal_task_delay_ms(100);
    }

    if (!g_provision_response_valid) {
        return -1;
    }

    if (save_credentials(PROV_STORAGE_PATH, &g_provisioned) != 0) {
        return -1;
    }

    return 0;
}

static int reconnect_with_persisted_credentials(void)
{
    provisioned_credentials_t creds;

    if (load_credentials(PROV_STORAGE_PATH, &creds) != 0) {
        return -1;
    }

    tb_client_disconnect(g_tb_client);
    tb_client_deinit(g_tb_client);
    g_tb_client = NULL;

    tb_client_config_t cfg = {
        .server_url = PROV_MQTT_URL,
        .device_name = PROV_DEVICE_NAME,
    };

    if (strcmp(creds.credentials_type, PROV_CRED_TYPE_ACCESS_TOKEN) == 0) {
        strncpy(cfg.access_token, creds.access_token, sizeof(cfg.access_token) - 1);
        strncpy(cfg.client_id, PROV_BOOTSTRAP_CLIENT_ID, sizeof(cfg.client_id) - 1);
    } else if (strcmp(creds.credentials_type, PROV_CRED_TYPE_MQTT_BASIC) == 0) {
        strncpy(cfg.access_token, creds.username, sizeof(cfg.access_token) - 1);
        strncpy(cfg.client_id, creds.client_id, sizeof(cfg.client_id) - 1);
    } else {
        return -1;
    }

    if (tb_client_init(&g_tb_client, &cfg) != 0) {
        return -1;
    }

    if (strcmp(creds.credentials_type, PROV_CRED_TYPE_MQTT_BASIC) == 0) {
        mqtt_config_set_string(creds.password, MQTT_CONFIG_VALUE_PASSWORD);
    }

    return connect_and_wait();
}

int main_function(void)
{
    int rc;

    printf("\n=============================================\n");
    printf("   ThingsBoard Provision Demo\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", PROV_BOOTSTRAP_CLIENT_ID);
    printf("  Broker    : %s\n", PROV_MQTT_URL);
    printf("=============================================\n\n");

    MongooseProcess_Init();

    tb_client_config_t bootstrap_cfg = {
        .server_url = PROV_MQTT_URL,
        .access_token = PROV_BOOTSTRAP_USERNAME,
        .client_id = PROV_BOOTSTRAP_CLIENT_ID,
        .device_name = PROV_BOOTSTRAP_DEVICE_NAME,
    };

    rc = tb_client_init(&g_tb_client, &bootstrap_cfg);
    if (rc != 0) {
        printf("[PROV_DEMO] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    mqtt_config_set_string(PROV_BOOTSTRAP_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[PROV_DEMO] Connecting bootstrap client...\n");
            rc = connect_and_wait();
            if (rc != 0) {
                printf("[PROV_DEMO] Reconnect in %d ms\n", PROV_RECONNECT_DELAY_MS);
                osal_task_delay_ms(PROV_RECONNECT_DELAY_MS);
                continue;
            }
            break;
        }
    }

    rc = provision_once();
    if (rc != 0) {
        printf("[PROV_DEMO] Provision request failed or response invalid\n");
        return 1;
    }

    printf("[PROV_DEMO] Provisioning response accepted and stored\n");

    rc = reconnect_with_persisted_credentials();
    if (rc != 0) {
        printf("[PROV_DEMO] Reconnect with persisted credentials failed\n");
        return 1;
    }

    printf("[PROV_DEMO] Reconnected using persisted credentials\n");

    (void)tb_telemetry_send_string(g_tb_client, "provision_state", "reconnected");
    (void)tb_attributes_send_string(g_tb_client, "provision_status", "ok");

    while (1) {
        osal_task_delay_ms(1000);
    }
}

#ifndef CONFIG_HQ_PLATFORM_ESP
int main(void)
{
    return main_function();
}
#endif
