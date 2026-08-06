/**
 *******************************************************************************
 * @file    tb_reconnect_integration_test.c
 * @brief   Broker-level reconnect integration coverage for ThingsBoard modules
 *******************************************************************************
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mongoose_process.h"
#include "mqtt_config.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_client.h"
#include "tb_rpc.h"

#define TEST_WAIT_STEP_MS 100U
#define TEST_WAIT_SHORT_MS 3000U
#define TEST_WAIT_CONNECT_MS 45000U
#define TEST_WAIT_CALLBACK_MS 8000U

#define TEST_ASSERT(condition, message)                                            \
	do {                                                                         \
		tests_run++;                                                           \
		if (condition) {                                                       \
			tests_passed++;                                                  \
			printf("  [PASS] %s\n", message);                               \
		} else {                                                               \
			tests_failed++;                                                  \
			printf("  [FAIL] %s (line %d)\n", message, __LINE__);          \
		}                                                                      \
	} while (0)

#define TEST_START(name)                                                           \
	printf("\n--------------------------------------------------\n");        \
	printf("TEST: %s\n", name);                                               \
	printf("--------------------------------------------------\n")

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static tb_client_t *g_client = NULL;
static volatile int g_connect_count = 0;
static volatile int g_disconnect_count = 0;

static volatile int g_attr_null_count = 0;
static volatile int g_attr_data_count = 0;
static char g_attr_last_payload[512] = { 0 };
static volatile uint32_t g_attr_last_req_id = 0;

static volatile int g_shared_count = 0;

static volatile int g_server_rpc_count = 0;
static volatile uint32_t g_last_server_rpc_id = 0;

static volatile int g_client_rpc_null_count = 0;
static volatile int g_client_rpc_data_count = 0;
static char g_client_rpc_last_payload[512] = { 0 };

static const char *g_broker_stop_cmd = NULL;
static const char *g_broker_start_cmd = NULL;

static void on_connect(tb_client_t *client, void *user_data)
{
	(void)client;
	(void)user_data;
	g_connect_count++;
}

static void on_disconnect(tb_client_t *client,
				 tb_client_disconnect_reason_t reason,
				 void *user_data)
{
	(void)client;
	(void)reason;
	(void)user_data;
	g_disconnect_count++;
}

static void on_attr_response(const char *json_response, void *user_data)
{
	(void)user_data;
	if (json_response == NULL) {
		g_attr_null_count++;
		return;
	}
	g_attr_data_count++;
	strncpy(g_attr_last_payload, json_response, sizeof(g_attr_last_payload) - 1);
	g_attr_last_payload[sizeof(g_attr_last_payload) - 1] = '\0';
}

static void on_shared_attr(const char *json_payload, void *user_data)
{
	(void)user_data;
	if (json_payload != NULL) {
		g_shared_count++;
	}
}

static void on_server_rpc(const char *method, const char *params_json,
			  uint32_t request_id, void *user_data)
{
	(void)method;
	(void)params_json;
	(void)user_data;
	g_server_rpc_count++;
	g_last_server_rpc_id = request_id;
	(void)tb_rpc_respond(g_client, request_id, "{\"ok\":true}");
}

static void on_client_rpc(const char *response_json, void *user_data)
{
	(void)user_data;
	if (response_json == NULL) {
		g_client_rpc_null_count++;
		return;
	}
	g_client_rpc_data_count++;
	strncpy(g_client_rpc_last_payload, response_json,
		sizeof(g_client_rpc_last_payload) - 1);
	g_client_rpc_last_payload[sizeof(g_client_rpc_last_payload) - 1] = '\0';
}

static bool wait_until(bool (*predicate)(void), uint32_t timeout_ms)
{
	uint32_t waited = 0;
	while (waited < timeout_ms) {
		if (predicate()) {
			return true;
		}
		osal_task_delay_ms(TEST_WAIT_STEP_MS);
		waited += TEST_WAIT_STEP_MS;
	}
	return false;
}

static bool predicate_connected(void)
{
	return tb_client_is_connected(g_client);
}

static bool predicate_disconnected(void)
{
	return !tb_client_is_connected(g_client);
}

static bool predicate_attr_data_seen(void);
static bool predicate_attr_request_seen(void);

static bool bounce_broker(void)
{
	int rc;

	if (g_broker_stop_cmd == NULL || g_broker_start_cmd == NULL ||
	    g_broker_stop_cmd[0] == '\0' || g_broker_start_cmd[0] == '\0') {
		return false;
	}

	rc = system(g_broker_stop_cmd);
	if (rc != 0) {
		return false;
	}

	/* Give the socket close path a moment before restart. */
	osal_task_delay_ms(800);

	rc = system(g_broker_start_cmd);
	if (rc != 0) {
		return false;
	}

	return true;
}

static void observe_attr_request_topic(const char *topic, const char *payload,
				       size_t payload_len)
{
	(void)payload;
	(void)payload_len;
	const char *id_start = strrchr(topic, '/');
	if (id_start != NULL) {
		g_attr_last_req_id = (uint32_t)strtoul(id_start + 1, NULL, 10);
	}
}

static void test_attribute_request_reconnect(void)
{
	static const char *keys[] = { "firmware_version" };
	char response_topic[128];

	TEST_START("Attribute Request Reconnect");

	g_attr_null_count = 0;
	g_attr_data_count = 0;
	g_attr_last_req_id = 0;
	memset(g_attr_last_payload, 0, sizeof(g_attr_last_payload));

	TEST_ASSERT(tb_client_subscribe(g_client,
				"v1/devices/me/attributes/request/+",
				observe_attr_request_topic,
				TEST_WAIT_SHORT_MS) == 0,
		    "observer subscribed to attribute request topic");

	TEST_ASSERT(tb_attributes_request_client(g_client, keys, 1,
					 on_attr_response, NULL,
					 TEST_WAIT_SHORT_MS) == 0,
		    "attribute request succeeds before broker drop");
	TEST_ASSERT(wait_until(predicate_attr_request_seen, TEST_WAIT_CALLBACK_MS),
		    "attribute request id captured from broker traffic");

	TEST_ASSERT(bounce_broker(), "broker bounced successfully");
	TEST_ASSERT(wait_until(predicate_disconnected, TEST_WAIT_CONNECT_MS),
		    "client observed disconnect after broker stop");
	TEST_ASSERT(wait_until(predicate_connected, TEST_WAIT_CONNECT_MS),
		    "client reconnected after broker restart");
	TEST_ASSERT(g_attr_null_count >= 1,
		    "pending attribute callback failed on disconnect");

	g_attr_last_req_id = 0;
	TEST_ASSERT(tb_attributes_request_client(g_client, keys, 1,
					 on_attr_response, NULL,
					 TEST_WAIT_SHORT_MS) == 0,
		    "attribute request succeeds after reconnect");
	TEST_ASSERT(wait_until(predicate_attr_request_seen, TEST_WAIT_CALLBACK_MS),
		    "attribute request id captured after reconnect");
	TEST_ASSERT(g_attr_last_req_id > 0,
		    "attribute request id captured after reconnect");

	snprintf(response_topic, sizeof(response_topic),
		 "v1/devices/me/attributes/response/%u", g_attr_last_req_id);
	TEST_ASSERT(tb_client_publish(g_client, response_topic,
			      "{\"client\":{\"firmware_version\":\"9.9.9\"}}") == 0,
		    "attribute response published via broker");

	TEST_ASSERT(wait_until(predicate_attr_data_seen, TEST_WAIT_CALLBACK_MS),
		    "attribute callback received data after reconnect");
	TEST_ASSERT(strstr(g_attr_last_payload, "firmware_version") != NULL,
		    "attribute callback payload contains expected key");
}

static bool predicate_attr_data_seen(void)
{
	return g_attr_data_count >= 1;
}

static bool predicate_shared_seen(void)
{
	return g_shared_count > 0;
}

static bool predicate_server_rpc_seen(void)
{
	return g_server_rpc_count > 0;
}

static bool predicate_client_rpc_data_seen(void)
{
	return g_client_rpc_data_count > 0;
}

static bool predicate_attr_request_seen(void)
{
	return g_attr_last_req_id > 0;
}

static void test_shared_attr_rpc_and_fw_reconnect(void)
{
	char client_rpc_resp_topic[128];
	uint32_t rpc_probe_req_id;
	uint32_t expected_rpc_req_id;

	TEST_START("Shared Attributes, RPC, and Firmware Reconnect");

	g_shared_count = 0;
	g_server_rpc_count = 0;
	g_last_server_rpc_id = 0;
	g_client_rpc_null_count = 0;
	g_client_rpc_data_count = 0;
	memset(g_client_rpc_last_payload, 0, sizeof(g_client_rpc_last_payload));

	TEST_ASSERT(tb_attributes_subscribe(g_client, on_shared_attr, NULL) == 0,
		    "shared attribute subscription succeeds");
	TEST_ASSERT(tb_rpc_subscribe_server(g_client, on_server_rpc, NULL) == 0,
		    "server RPC subscription succeeds");

	TEST_ASSERT(bounce_broker(), "broker bounced during active subscriptions");
	TEST_ASSERT(wait_until(predicate_disconnected, TEST_WAIT_CONNECT_MS),
		    "disconnect observed after second broker stop");
	TEST_ASSERT(wait_until(predicate_connected, TEST_WAIT_CONNECT_MS),
		    "reconnect observed after second broker restart");

	TEST_ASSERT(tb_client_publish(g_client, "v1/devices/me/attributes",
			      "{\"threshold\":42}") == 0,
		    "shared attribute update published");
	TEST_ASSERT(wait_until(predicate_shared_seen, TEST_WAIT_CALLBACK_MS),
		    "shared attribute callback restored after reconnect");
	TEST_ASSERT(g_shared_count == 1,
		    "shared attribute callback invoked exactly once");

	TEST_ASSERT(tb_client_publish(g_client, "v1/devices/me/rpc/request/77",
			      "{\"method\":\"setLed\",\"params\":{\"value\":1}}") == 0,
		    "server RPC request published");
	TEST_ASSERT(wait_until(predicate_server_rpc_seen, TEST_WAIT_CALLBACK_MS),
		    "server RPC callback restored after reconnect");
	TEST_ASSERT(g_last_server_rpc_id == 77,
		    "server RPC request id propagated");

	TEST_ASSERT(tb_rpc_request(g_client, "getTime", NULL, on_client_rpc,
			   NULL, TEST_WAIT_SHORT_MS) == 0,
		    "client RPC request succeeds after reconnect");
	TEST_ASSERT(g_client_rpc_null_count == 0,
		    "no spurious client RPC timeout on healthy reconnect path");

	rpc_probe_req_id = tb_client_get_next_request_id(g_client);
	expected_rpc_req_id = rpc_probe_req_id - 1;

	snprintf(client_rpc_resp_topic, sizeof(client_rpc_resp_topic),
		 "v1/devices/me/rpc/response/%u", expected_rpc_req_id);
	TEST_ASSERT(tb_client_publish(g_client, client_rpc_resp_topic,
			      "{\"time\":1700000099}") == 0,
		    "client RPC response published");
	TEST_ASSERT(wait_until(predicate_client_rpc_data_seen, TEST_WAIT_CALLBACK_MS),
		    "client RPC response callback restored after reconnect");
	TEST_ASSERT(strstr(g_client_rpc_last_payload, "1700000099") != NULL,
		    "client RPC payload propagated");
}

int main(void)
{
	const char *mqtt_url = getenv("TB_IT_MQTT_URL");
	tb_client_config_t cfg;
	int rc;

	if (mqtt_url == NULL || mqtt_url[0] == '\0') {
		mqtt_url = "mqtt://127.0.0.1:1884";
	}
	g_broker_stop_cmd = getenv("TB_IT_BROKER_STOP_CMD");
	g_broker_start_cmd = getenv("TB_IT_BROKER_START_CMD");

	printf("\n==================================================\n");
	printf("   ThingsBoard Reconnect Broker Integration Test\n");
	printf("==================================================\n");
	printf("Broker URL: %s\n", mqtt_url);

	MongooseProcess_Init();

	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.server_url, mqtt_url, sizeof(cfg.server_url) - 1);
	strncpy(cfg.access_token, "tb_it_user", sizeof(cfg.access_token) - 1);
	strncpy(cfg.client_id, "tb_it_client", sizeof(cfg.client_id) - 1);
	strncpy(cfg.device_name, "tb_integration", sizeof(cfg.device_name) - 1);
	cfg.on_connect = on_connect;
	cfg.on_disconnect = on_disconnect;

	rc = tb_client_init(&g_client, &cfg);
	if (rc != 0 || g_client == NULL) {
		printf("[FAIL] tb_client_init failed: %d\n", rc);
		MongooseProcess_Deinit();
		return 1;
	}

	mqtt_config_set_string("", MQTT_CONFIG_VALUE_PASSWORD);

	rc = tb_client_connect(g_client);
	if (rc != 0) {
		printf("[FAIL] tb_client_connect failed: %d\n", rc);
		tb_client_deinit(g_client);
		MongooseProcess_Deinit();
		return 1;
	}

	if (!wait_until(predicate_connected, TEST_WAIT_CONNECT_MS)) {
		printf("[FAIL] connect timeout\n");
		tb_client_deinit(g_client);
		MongooseProcess_Deinit();
		return 1;
	}

	test_attribute_request_reconnect();
	test_shared_attr_rpc_and_fw_reconnect();

	printf("\n==================================================\n");
	printf("                  TEST SUMMARY                    \n");
	printf("==================================================\n");
	printf("  Run:    %d\n", tests_run);
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("==================================================\n");

	tb_client_deinit(g_client);
	MongooseProcess_Deinit();

	return (tests_failed == 0) ? 0 : 1;
}
