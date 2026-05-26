/**
 *******************************************************************************
 * @file    tb_tests.c
 * @brief   ThingsBoard client unit tests
 *******************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb_client.h"
#include "tb_telemetry.h"
#include "tb_attributes.h"
#include "tb_rpc.h"
#include "tb_provision.h"
#include "tb_claim.h"
#include "mqtt_app_mock.h"
#include "cJSON.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message)                          \
    do {                                                        \
        tests_run++;                                            \
        if (condition) {                                        \
            tests_passed++;                                     \
            printf("  [PASS] %s\n", message);                  \
        } else {                                                \
            tests_failed++;                                     \
            printf("  [FAIL] %s (line %d)\n", message, __LINE__); \
        }                                                       \
    } while (0)

#define TEST_START(name)                                             \
    printf("\n--------------------------------------------------\n"); \
    printf("TEST: %s\n", name);                                      \
    printf("--------------------------------------------------\n")

/* ============================================================
 * Helper: create and connect client
 * ============================================================ */
static tb_client_t *create_test_client(void)
{
    mqtt_app_mock_reset();

    tb_client_config_t cfg = {
        .server_url = "mqtt://localhost:1883",
        .access_token = "test_token",
        .client_id = NULL,
        .device_name = "test_device",
    };

    tb_client_t *client = NULL;
    int ret = tb_client_init(&client, &cfg);
    if (ret != 0 || client == NULL) {
        return NULL;
    }
    tb_client_connect(client);
    return client;
}

static void destroy_test_client(tb_client_t *client)
{
    if (client) {
        tb_client_deinit(client);
    }
}

/* ============================================================
 * Test: Client Init/Connect/Disconnect
 * ============================================================ */
static void test_client_lifecycle(void)
{
    TEST_START("Client Lifecycle");
    mqtt_app_mock_reset();

    tb_client_config_t cfg = {
        .server_url = "mqtt://tb.example.com:1883",
        .access_token = "my_device_token",
        .client_id = "my_client",
        .device_name = "sensor_1",
    };

    tb_client_t *client = NULL;
    int ret = tb_client_init(&client, &cfg);
    TEST_ASSERT(ret == 0, "tb_client_init succeeds");
    TEST_ASSERT(client != NULL, "client handle is not NULL");

    ret = tb_client_connect(client);
    TEST_ASSERT(ret == 0, "tb_client_connect succeeds");
    TEST_ASSERT(tb_client_is_connected(client) == true, "client is connected");

    tb_client_disconnect(client);
    TEST_ASSERT(tb_client_is_connected(client) == false, "client is disconnected");

    tb_client_deinit(client);
}

static void test_client_init_null_params(void)
{
    TEST_START("Client Init NULL params");

    int ret = tb_client_init(NULL, NULL);
    TEST_ASSERT(ret != 0, "init with NULL client ptr fails");

    tb_client_t *client = NULL;
    tb_client_config_t cfg = { .server_url = NULL, .access_token = NULL };
    ret = tb_client_init(&client, &cfg);
    TEST_ASSERT(ret != 0, "init with NULL access_token fails");
}

static void test_client_request_id(void)
{
    TEST_START("Client Request ID");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    uint32_t id1 = tb_client_get_next_request_id(client);
    uint32_t id2 = tb_client_get_next_request_id(client);
    uint32_t id3 = tb_client_get_next_request_id(client);
    TEST_ASSERT(id1 == 1, "first request ID is 1");
    TEST_ASSERT(id2 == 2, "second request ID is 2");
    TEST_ASSERT(id3 == 3, "third request ID is 3");

    destroy_test_client(client);
}

/* ============================================================
 * Test: Telemetry
 * ============================================================ */
static void test_telemetry_send_int(void)
{
    TEST_START("Telemetry Send Int");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_telemetry_send_int(client, "temperature", 25);
    TEST_ASSERT(ret == 0, "send int telemetry succeeds");
    TEST_ASSERT(mock_publish_count == 1, "one message published");
    TEST_ASSERT(strcmp(mock_publishes[0].topic, "v1/devices/me/telemetry") == 0,
                "published to telemetry topic");

    /* Verify JSON content */
    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "published JSON is valid");
    if (root) {
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
        TEST_ASSERT(temp != NULL && cJSON_IsNumber(temp), "temperature key exists and is number");
        TEST_ASSERT(temp != NULL && temp->valueint == 25, "temperature value is 25");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

static void test_telemetry_send_double(void)
{
    TEST_START("Telemetry Send Double");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_telemetry_send_double(client, "humidity", 65.5);
    TEST_ASSERT(ret == 0, "send double telemetry succeeds");

    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "published JSON is valid");
    if (root) {
        cJSON *hum = cJSON_GetObjectItemCaseSensitive(root, "humidity");
        TEST_ASSERT(hum != NULL && cJSON_IsNumber(hum), "humidity key exists");
        TEST_ASSERT(hum != NULL && hum->valuedouble > 65.4 && hum->valuedouble < 65.6,
                    "humidity value is ~65.5");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

static void test_telemetry_send_bool(void)
{
    TEST_START("Telemetry Send Bool");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_telemetry_send_bool(client, "active", true);
    TEST_ASSERT(ret == 0, "send bool telemetry succeeds");

    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "published JSON is valid");
    if (root) {
        cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active");
        TEST_ASSERT(active != NULL && cJSON_IsTrue(active), "active is true");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

static void test_telemetry_send_string(void)
{
    TEST_START("Telemetry Send String");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_telemetry_send_string(client, "status", "running");
    TEST_ASSERT(ret == 0, "send string telemetry succeeds");

    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "published JSON is valid");
    if (root) {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        TEST_ASSERT(status != NULL && cJSON_IsString(status), "status is string");
        TEST_ASSERT(status != NULL && strcmp(status->valuestring, "running") == 0,
                    "status value is 'running'");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

static void test_telemetry_send_json(void)
{
    TEST_START("Telemetry Send Raw JSON");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    const char *json = "{\"temp\":22,\"hum\":55}";
    int ret = tb_telemetry_send_json(client, json);
    TEST_ASSERT(ret == 0, "send json telemetry succeeds");
    TEST_ASSERT(strcmp(mock_publishes[0].message, json) == 0,
                "raw JSON passed through unchanged");

    destroy_test_client(client);
}

static void test_telemetry_null_params(void)
{
    TEST_START("Telemetry NULL params");

    int ret = tb_telemetry_send_int(NULL, "key", 1);
    TEST_ASSERT(ret != 0, "send with NULL client fails");

    tb_client_t *client = create_test_client();
    ret = tb_telemetry_send_int(client, NULL, 1);
    TEST_ASSERT(ret != 0, "send with NULL key fails");

    destroy_test_client(client);
}

/* ============================================================
 * Test: Attributes
 * ============================================================ */
static void test_attributes_send(void)
{
    TEST_START("Attributes Send");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_attributes_send_string(client, "firmware_version", "1.2.3");
    TEST_ASSERT(ret == 0, "send attribute succeeds");
    TEST_ASSERT(strcmp(mock_publishes[0].topic, "v1/devices/me/attributes") == 0,
                "published to attribute topic");

    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "attribute JSON valid");
    if (root) {
        cJSON *fw = cJSON_GetObjectItemCaseSensitive(root, "firmware_version");
        TEST_ASSERT(fw != NULL && strcmp(fw->valuestring, "1.2.3") == 0,
                    "firmware_version value correct");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

static bool s_attr_response_received = false;
static char s_attr_response_buf[512] = {0};

static void attr_response_cb(const char *json_response, void *user_data)
{
    s_attr_response_received = true;
    if (json_response) {
        strncpy(s_attr_response_buf, json_response, sizeof(s_attr_response_buf) - 1);
    }
    (void)user_data;
}

static void test_attributes_request(void)
{
    TEST_START("Attributes Request & Response");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    s_attr_response_received = false;
    memset(s_attr_response_buf, 0, sizeof(s_attr_response_buf));

    const char *keys[] = {"firmware_version", "serial_number"};
    int ret = tb_attributes_request_client(client, keys, 2,
                                           attr_response_cb, NULL, 5000);
    TEST_ASSERT(ret == 0, "attribute request succeeds");
    TEST_ASSERT(mock_publish_count == 1, "request message published");

    /* Verify request topic format */
    TEST_ASSERT(strstr(mock_publishes[0].topic, "v1/devices/me/attributes/request/") != NULL,
                "request published to correct topic");

    /* Verify request JSON has clientKeys */
    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "request JSON valid");
    if (root) {
        cJSON *ck = cJSON_GetObjectItemCaseSensitive(root, "clientKeys");
        TEST_ASSERT(ck != NULL && cJSON_IsString(ck), "clientKeys present");
        TEST_ASSERT(ck != NULL && strstr(ck->valuestring, "firmware_version") != NULL,
                    "clientKeys contains firmware_version");
        cJSON_Delete(root);
    }

    /* Simulate server response */
    const char *response = "{\"client\":{\"firmware_version\":\"2.0\"}}";
    mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
                                  response, strlen(response));
    TEST_ASSERT(s_attr_response_received == true, "attribute response callback called");
    TEST_ASSERT(strstr(s_attr_response_buf, "firmware_version") != NULL,
                "response contains firmware_version");

    destroy_test_client(client);
}

static bool s_shared_attr_received = false;
static char s_shared_attr_buf[512] = {0};

static void shared_attr_cb(const char *json_payload, void *user_data)
{
    s_shared_attr_received = true;
    if (json_payload) {
        strncpy(s_shared_attr_buf, json_payload, sizeof(s_shared_attr_buf) - 1);
    }
    (void)user_data;
}

static void test_attributes_subscribe_shared(void)
{
    TEST_START("Shared Attribute Subscribe");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    s_shared_attr_received = false;

    int ret = tb_attributes_subscribe(client, shared_attr_cb, NULL);
    TEST_ASSERT(ret == 0, "subscribe shared attributes succeeds");

    /* Simulate shared attribute update from server */
    const char *update = "{\"threshold\":42}";
    mqtt_app_mock_deliver_message("v1/devices/me/attributes",
                                  update, strlen(update));
    TEST_ASSERT(s_shared_attr_received == true, "shared attr callback called");
    TEST_ASSERT(strstr(s_shared_attr_buf, "threshold") != NULL,
                "shared attr payload contains key");

    ret = tb_attributes_unsubscribe(client);
    TEST_ASSERT(ret == 0, "unsubscribe shared attributes succeeds");

    destroy_test_client(client);
}

/* ============================================================
 * Test: Server-Side RPC
 * ============================================================ */
static bool s_server_rpc_received = false;
static char s_rpc_method[64] = {0};
static char s_rpc_params[256] = {0};
static uint32_t s_rpc_request_id = 0;

static void server_rpc_cb(const char *method, const char *params_json,
                          uint32_t request_id, void *user_data)
{
    s_server_rpc_received = true;
    if (method) strncpy(s_rpc_method, method, sizeof(s_rpc_method) - 1);
    if (params_json) strncpy(s_rpc_params, params_json, sizeof(s_rpc_params) - 1);
    s_rpc_request_id = request_id;
    (void)user_data;
}

static void test_server_side_rpc(void)
{
    TEST_START("Server-Side RPC");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    s_server_rpc_received = false;
    memset(s_rpc_method, 0, sizeof(s_rpc_method));
    memset(s_rpc_params, 0, sizeof(s_rpc_params));

    int ret = tb_rpc_subscribe_server(client, server_rpc_cb, NULL);
    TEST_ASSERT(ret == 0, "subscribe server RPC succeeds");

    /* Simulate server sending RPC request */
    const char *rpc_msg = "{\"method\":\"setLed\",\"params\":{\"pin\":4,\"value\":1}}";
    mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/42",
                                  rpc_msg, strlen(rpc_msg));

    TEST_ASSERT(s_server_rpc_received == true, "server RPC callback called");
    TEST_ASSERT(strcmp(s_rpc_method, "setLed") == 0, "RPC method is 'setLed'");
    TEST_ASSERT(s_rpc_request_id == 42, "RPC request_id is 42");
    TEST_ASSERT(strstr(s_rpc_params, "pin") != NULL, "RPC params contain 'pin'");

    /* Send response */
    mock_publish_count = 0;
    ret = tb_rpc_respond(client, 42, "{\"result\":\"ok\"}");
    TEST_ASSERT(ret == 0, "RPC respond succeeds");
    TEST_ASSERT(strstr(mock_publishes[0].topic, "v1/devices/me/rpc/response/42") != NULL,
                "response published to correct topic");

    ret = tb_rpc_unsubscribe_server(client);
    TEST_ASSERT(ret == 0, "unsubscribe server RPC succeeds");

    destroy_test_client(client);
}

/* ============================================================
 * Test: Client-Side RPC
 * ============================================================ */
static bool s_client_rpc_received = false;
static char s_client_rpc_response[512] = {0};

static void client_rpc_cb(const char *response_json, void *user_data)
{
    s_client_rpc_received = true;
    if (response_json) {
        strncpy(s_client_rpc_response, response_json,
                sizeof(s_client_rpc_response) - 1);
    }
    (void)user_data;
}

static void test_client_side_rpc(void)
{
    TEST_START("Client-Side RPC");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    s_client_rpc_received = false;
    memset(s_client_rpc_response, 0, sizeof(s_client_rpc_response));

    int ret = tb_rpc_request(client, "getTime", NULL,
                             client_rpc_cb, NULL, 5000);
    TEST_ASSERT(ret == 0, "client RPC request succeeds");
    TEST_ASSERT(mock_publish_count == 1, "RPC request published");

    /* Verify request format */
    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "RPC request JSON valid");
    if (root) {
        cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
        TEST_ASSERT(method != NULL && strcmp(method->valuestring, "getTime") == 0,
                    "method is 'getTime'");
        cJSON_Delete(root);
    }

    /* Extract request ID from topic */
    const char *id_start = strrchr(mock_publishes[0].topic, '/');
    TEST_ASSERT(id_start != NULL, "topic has request ID");
    uint32_t req_id = (uint32_t)strtoul(id_start + 1, NULL, 10);

    /* Simulate server response */
    char resp_topic[128];
    snprintf(resp_topic, sizeof(resp_topic),
             "v1/devices/me/rpc/response/%u", req_id);
    const char *resp = "{\"time\":1700000000}";
    mqtt_app_mock_deliver_message(resp_topic, resp, strlen(resp));

    TEST_ASSERT(s_client_rpc_received == true, "client RPC response received");
    TEST_ASSERT(strstr(s_client_rpc_response, "1700000000") != NULL,
                "response contains time value");

    destroy_test_client(client);
}

/* ============================================================
 * Test: Provisioning
 * ============================================================ */
static bool s_provision_received = false;
static char s_provision_response[512] = {0};

static void provision_cb(const char *response_json, void *user_data)
{
    s_provision_received = true;
    if (response_json) {
        strncpy(s_provision_response, response_json,
                sizeof(s_provision_response) - 1);
    }
    (void)user_data;
}

static void test_provisioning(void)
{
    TEST_START("Device Provisioning");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    s_provision_received = false;

    tb_provision_request_t req = {
        .device_name = "new_device",
        .provision_device_key = "my_provision_key",
        .provision_device_secret = "my_provision_secret",
        .credentials_type = NULL,
    };

    int ret = tb_provision_request(client, &req, provision_cb, NULL, 10000);
    TEST_ASSERT(ret == 0, "provision request succeeds");
    TEST_ASSERT(mock_publish_count == 1, "provision message published");
    TEST_ASSERT(strcmp(mock_publishes[0].topic, "/provision/request") == 0,
                "published to provision topic");

    /* Verify provision JSON */
    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "provision JSON valid");
    if (root) {
        cJSON *dk = cJSON_GetObjectItemCaseSensitive(root, "provisionDeviceKey");
        TEST_ASSERT(dk != NULL && strcmp(dk->valuestring, "my_provision_key") == 0,
                    "provisionDeviceKey correct");
        cJSON *dn = cJSON_GetObjectItemCaseSensitive(root, "deviceName");
        TEST_ASSERT(dn != NULL && strcmp(dn->valuestring, "new_device") == 0,
                    "deviceName correct");
        cJSON_Delete(root);
    }

    /* Simulate provision response */
    const char *resp = "{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
    mqtt_app_mock_deliver_message("/provision/response", resp, strlen(resp));
    TEST_ASSERT(s_provision_received == true, "provision callback called");
    TEST_ASSERT(strstr(s_provision_response, "abc123") != NULL,
                "provision response contains token");

    destroy_test_client(client);
}

/* ============================================================
 * Test: Device Claiming
 * ============================================================ */
static void test_claim_device(void)
{
    TEST_START("Device Claiming");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_claim_device(client, "my_secret", 60000);
    TEST_ASSERT(ret == 0, "claim device succeeds");
    TEST_ASSERT(strcmp(mock_publishes[0].topic, "v1/devices/me/claim") == 0,
                "published to claim topic");

    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "claim JSON valid");
    if (root) {
        cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "secretKey");
        TEST_ASSERT(sk != NULL && strcmp(sk->valuestring, "my_secret") == 0,
                    "secretKey correct");
        cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "durationMs");
        TEST_ASSERT(dur != NULL && dur->valueint == 60000,
                    "durationMs correct");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

static void test_claim_device_no_secret(void)
{
    TEST_START("Device Claiming (no secret)");
    tb_client_t *client = create_test_client();
    TEST_ASSERT(client != NULL, "client created");

    int ret = tb_claim_device(client, NULL, 30000);
    TEST_ASSERT(ret == 0, "claim without secret succeeds");

    cJSON *root = cJSON_Parse(mock_publishes[0].message);
    TEST_ASSERT(root != NULL, "claim JSON valid");
    if (root) {
        cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "secretKey");
        TEST_ASSERT(sk == NULL, "no secretKey when NULL passed");
        cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "durationMs");
        TEST_ASSERT(dur != NULL && dur->valueint == 30000, "durationMs correct");
        cJSON_Delete(root);
    }

    destroy_test_client(client);
}

/* ============================================================
 * Main
 * ============================================================ */
#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    printf("\n==================================================\n");
    printf("        ThingsBoard Client Unit Tests            \n");
    printf("==================================================\n");

    /* Client lifecycle */
    test_client_lifecycle();
    test_client_init_null_params();
    test_client_request_id();

    /* Telemetry */
    test_telemetry_send_int();
    test_telemetry_send_double();
    test_telemetry_send_bool();
    test_telemetry_send_string();
    test_telemetry_send_json();
    test_telemetry_null_params();

    /* Attributes */
    test_attributes_send();
    test_attributes_request();
    test_attributes_subscribe_shared();

    /* RPC */
    test_server_side_rpc();
    test_client_side_rpc();

    /* Provisioning */
    test_provisioning();

    /* Claiming */
    test_claim_device();
    test_claim_device_no_secret();

    printf("\n==================================================\n");
    printf("              TEST SUMMARY                       \n");
    printf("==================================================\n");
    printf("  Run:    %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("==================================================\n");

#ifndef ESP_PLATFORM
    return (tests_failed == 0) ? 0 : 1;
#endif
}
