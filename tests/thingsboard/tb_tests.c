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
#include "tb_firmware_update.h"
#include "osal_ota.h"
#include "osal_ota_state.h"
#include "mqtt_app_mock.h"
#include "cJSON.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static int s_conn_event_order[16];
static int s_conn_event_count = 0;
static int s_connect_cb_count = 0;
static int s_disconnect_cb_count = 0;
static int s_connect_failure_cb_count = 0;
static tb_client_disconnect_reason_t s_last_disconnect_reason =
	TB_CLIENT_DISCONNECT_REASON_EXPLICIT;
static tb_client_connect_failure_reason_t s_last_connect_failure_reason =
	TB_CLIENT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR;
static void *s_last_connection_user_data = NULL;
static bool s_connect_cb_connected_state = false;
static bool s_disconnect_cb_connected_state = true;
static bool s_connect_failure_cb_connected_state = true;

enum {
	CONN_EVENT_CONNECT = 1,
	CONN_EVENT_DISCONNECT,
	CONN_EVENT_CONNECT_FAILURE,
};

static void reset_connection_callback_state(void)
{
	s_conn_event_count = 0;
	s_connect_cb_count = 0;
	s_disconnect_cb_count = 0;
	s_connect_failure_cb_count = 0;
	s_last_disconnect_reason = TB_CLIENT_DISCONNECT_REASON_EXPLICIT;
	s_last_connect_failure_reason =
		TB_CLIENT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR;
	s_last_connection_user_data = NULL;
	s_connect_cb_connected_state = false;
	s_disconnect_cb_connected_state = true;
	s_connect_failure_cb_connected_state = true;
	memset(s_conn_event_order, 0, sizeof(s_conn_event_order));
}

static void record_connection_event(int event_id)
{
	if (s_conn_event_count < (int)(sizeof(s_conn_event_order) /
					   sizeof(s_conn_event_order[0]))) {
		s_conn_event_order[s_conn_event_count++] = event_id;
	}
}

static void on_client_connect(tb_client_t *client, void *user_data)
{
	s_connect_cb_count++;
	s_last_connection_user_data = user_data;
	s_connect_cb_connected_state = tb_client_is_connected(client);
	record_connection_event(CONN_EVENT_CONNECT);
}

static void on_client_disconnect(tb_client_t *client,
				 tb_client_disconnect_reason_t reason,
				 void *user_data)
{
	s_disconnect_cb_count++;
	s_last_disconnect_reason = reason;
	s_last_connection_user_data = user_data;
	s_disconnect_cb_connected_state = tb_client_is_connected(client);
	record_connection_event(CONN_EVENT_DISCONNECT);
}

static void on_client_connect_failure(
	tb_client_t *client, tb_client_connect_failure_reason_t reason,
	void *user_data)
{
	s_connect_failure_cb_count++;
	s_last_connect_failure_reason = reason;
	s_last_connection_user_data = user_data;
	s_connect_failure_cb_connected_state = tb_client_is_connected(client);
	record_connection_event(CONN_EVENT_CONNECT_FAILURE);
}

#define FW_SHA256_ABCDEFGH "9ac2197d9258257b1ae8463e4214e4cd0a578bc1517f2415928b91be4283fc48"

#define TEST_ASSERT(condition, message)                                       \
	do {                                                                  \
		tests_run++;                                                  \
		if (condition) {                                              \
			tests_passed++;                                       \
			printf("  [PASS] %s\n", message);                     \
		} else {                                                      \
			tests_failed++;                                       \
			printf("  [FAIL] %s (line %d)\n", message, __LINE__); \
		}                                                             \
	} while (0)

#define TEST_START(name)                                                  \
	printf("\n--------------------------------------------------\n"); \
	printf("TEST: %s\n", name);                                       \
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
	TEST_ASSERT(tb_client_is_connected(client) == true,
		    "client is connected");

	tb_client_disconnect(client);
	TEST_ASSERT(tb_client_is_connected(client) == false,
		    "client is disconnected");

	tb_client_deinit(client);
}

static void test_client_init_null_params(void)
{
	TEST_START("Client Init NULL params");

	int ret = tb_client_init(NULL, NULL);
	TEST_ASSERT(ret != 0, "init with NULL client ptr fails");

	tb_client_t *client = NULL;
	tb_client_config_t cfg = { 0 };
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

static void test_client_connection_callbacks(void)
{
	TEST_START("Client Connection Callbacks");
	mqtt_app_mock_reset();
	reset_connection_callback_state();

	int callback_cookie = 1337;
	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.client_id = "my_client",
		.device_name = "sensor_1",
		.on_connect = on_client_connect,
		.on_disconnect = on_client_disconnect,
		.on_connect_failure = on_client_connect_failure,
		.connection_user_data = &callback_cookie,
	};

	tb_client_t *client = NULL;
	TEST_ASSERT(tb_client_init(&client, &cfg) == 0,
		    "tb_client_init succeeds with connection callbacks");
	TEST_ASSERT(client != NULL, "client handle is not NULL");

	TEST_ASSERT(tb_client_connect(client) == 0, "tb_client_connect succeeds");
	TEST_ASSERT(s_connect_cb_count == 1,
		    "connect callback fires on initial connect");
	TEST_ASSERT(s_conn_event_count >= 1 &&
			    s_conn_event_order[0] == CONN_EVENT_CONNECT,
		    "connect callback is first lifecycle event");
	TEST_ASSERT(s_last_connection_user_data == &callback_cookie,
		    "connect callback receives configured user data");
	TEST_ASSERT(s_connect_cb_connected_state == true,
		    "client is connected when connect callback runs");

	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT(s_disconnect_cb_count == 1,
		    "disconnect callback fires for remote close");
	TEST_ASSERT(s_last_disconnect_reason ==
			    TB_CLIENT_DISCONNECT_REASON_REMOTE_CLOSE,
		    "disconnect callback reason maps to remote close");
	TEST_ASSERT(s_disconnect_cb_connected_state == false,
		    "client is disconnected when disconnect callback runs");

	mqtt_app_mock_simulate_connect();
	TEST_ASSERT(s_connect_cb_count == 2,
		    "connect callback fires again after reconnect");
	TEST_ASSERT(s_conn_event_count >= 3 &&
			    s_conn_event_order[1] == CONN_EVENT_DISCONNECT &&
			    s_conn_event_order[2] == CONN_EVENT_CONNECT,
		    "remote close/reconnect callback order is preserved");

	tb_client_disconnect(client);
	TEST_ASSERT(s_disconnect_cb_count == 2,
		    "disconnect callback fires for explicit disconnect");
	TEST_ASSERT(s_last_disconnect_reason ==
			    TB_CLIENT_DISCONNECT_REASON_EXPLICIT,
		    "disconnect callback reason maps to explicit disconnect");

	tb_client_deinit(client);
}

static void test_client_connection_failure_callback(void)
{
	TEST_START("Client Connection Failure Callback");
	mqtt_app_mock_reset();
	reset_connection_callback_state();

	int callback_cookie = 42;
	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.on_connect = on_client_connect,
		.on_disconnect = on_client_disconnect,
		.on_connect_failure = on_client_connect_failure,
		.connection_user_data = &callback_cookie,
	};

	tb_client_t *client = NULL;
	TEST_ASSERT(tb_client_init(&client, &cfg) == 0,
		    "tb_client_init succeeds for connection failure test");
	TEST_ASSERT(client != NULL, "client handle is not NULL");

	mqtt_app_mock_simulate_connect_failure(
		MQTT_CONNECT_FAILURE_REASON_CONNACK_REJECTED);
	TEST_ASSERT(s_connect_failure_cb_count == 1,
		    "connection failure callback fires for connack reject");
	TEST_ASSERT(s_last_connect_failure_reason ==
			    TB_CLIENT_CONNECT_FAILURE_REASON_CONNACK_REJECTED,
		    "connection failure reason maps to connack rejected");
	TEST_ASSERT(s_last_connection_user_data == &callback_cookie,
		    "connection failure callback receives configured user data");
	TEST_ASSERT(s_connect_failure_cb_connected_state == false,
		    "client is disconnected when failure callback runs");

	mqtt_app_mock_simulate_connect_failure(
		MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED);
	TEST_ASSERT(s_connect_failure_cb_count == 2,
		    "connection failure callback fires for connect creation failure");
	TEST_ASSERT(s_last_connect_failure_reason ==
			    TB_CLIENT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED,
		    "connection failure reason maps to connect creation failure");
	TEST_ASSERT(s_connect_failure_cb_connected_state == false,
		    "client remains disconnected during repeated failures");

	tb_client_deinit(client);
}

static void test_client_singleton_init_rejected(void)
{
	TEST_START("Client Singleton Init Rejected");
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.device_name = "sensor_1",
	};

	tb_client_t *client1 = NULL;
	tb_client_t *client2 = NULL;

	TEST_ASSERT(tb_client_init(&client1, &cfg) == 0,
		    "first tb_client_init succeeds");
	TEST_ASSERT(client1 != NULL, "first client handle is not NULL");
	TEST_ASSERT(tb_client_init(&client2, &cfg) != 0,
		    "second tb_client_init fails while first client is active");
	TEST_ASSERT(client2 == NULL,
		    "second client handle remains NULL on singleton rejection");

	tb_client_deinit(client1);
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
	TEST_ASSERT(strcmp(mock_publishes[0].topic,
			   "v1/devices/me/telemetry") == 0,
		    "published to telemetry topic");

	/* Verify JSON content */
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "published JSON is valid");
	if (root) {
		cJSON *temp =
			cJSON_GetObjectItemCaseSensitive(root, "temperature");
		TEST_ASSERT(temp != NULL && cJSON_IsNumber(temp),
			    "temperature key exists and is number");
		TEST_ASSERT(temp != NULL && temp->valueint == 25,
			    "temperature value is 25");
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
		TEST_ASSERT(hum != NULL && cJSON_IsNumber(hum),
			    "humidity key exists");
		TEST_ASSERT(hum != NULL && hum->valuedouble > 65.4 &&
				    hum->valuedouble < 65.6,
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
		cJSON *active =
			cJSON_GetObjectItemCaseSensitive(root, "active");
		TEST_ASSERT(active != NULL && cJSON_IsTrue(active),
			    "active is true");
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
		cJSON *status =
			cJSON_GetObjectItemCaseSensitive(root, "status");
		TEST_ASSERT(status != NULL && cJSON_IsString(status),
			    "status is string");
		TEST_ASSERT(status != NULL &&
				    strcmp(status->valuestring, "running") == 0,
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

	int ret =
		tb_attributes_send_string(client, "firmware_version", "1.2.3");
	TEST_ASSERT(ret == 0, "send attribute succeeds");
	TEST_ASSERT(strcmp(mock_publishes[0].topic,
			   "v1/devices/me/attributes") == 0,
		    "published to attribute topic");

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "attribute JSON valid");
	if (root) {
		cJSON *fw = cJSON_GetObjectItemCaseSensitive(
			root, "firmware_version");
		TEST_ASSERT(fw != NULL && strcmp(fw->valuestring, "1.2.3") == 0,
			    "firmware_version value correct");
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static bool s_attr_response_received = false;
static char s_attr_response_buf[512] = { 0 };

static void attr_response_cb(const char *json_response, void *user_data)
{
	s_attr_response_received = true;
	if (json_response) {
		strncpy(s_attr_response_buf, json_response,
			sizeof(s_attr_response_buf) - 1);
		s_attr_response_buf[sizeof(s_attr_response_buf) - 1] = '\0';
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

	const char *keys[] = { "firmware_version", "serial_number" };
	int ret = tb_attributes_request_client(client, keys, 2,
					       attr_response_cb, NULL, 5000);
	TEST_ASSERT(ret == 0, "attribute request succeeds");
	TEST_ASSERT(mock_publish_count == 1, "request message published");

	/* Verify request topic format */
	TEST_ASSERT(strstr(mock_publishes[0].topic,
			   "v1/devices/me/attributes/request/") != NULL,
		    "request published to correct topic");

	/* Verify request JSON has clientKeys */
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "request JSON valid");
	if (root) {
		cJSON *ck =
			cJSON_GetObjectItemCaseSensitive(root, "clientKeys");
		TEST_ASSERT(ck != NULL && cJSON_IsString(ck),
			    "clientKeys present");
		TEST_ASSERT(ck != NULL && strstr(ck->valuestring,
						 "firmware_version") != NULL,
			    "clientKeys contains firmware_version");
		cJSON_Delete(root);
	}

	/* Simulate server response */
	const char *response = "{\"client\":{\"firmware_version\":\"2.0\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      response, strlen(response));
	TEST_ASSERT(s_attr_response_received == true,
		    "attribute response callback called");
	TEST_ASSERT(strstr(s_attr_response_buf, "firmware_version") != NULL,
		    "response contains firmware_version");

	destroy_test_client(client);
}

static bool s_shared_attr_received = false;
static char s_shared_attr_buf[512] = { 0 };

static void shared_attr_cb(const char *json_payload, void *user_data)
{
	s_shared_attr_received = true;
	if (json_payload) {
		strncpy(s_shared_attr_buf, json_payload,
			sizeof(s_shared_attr_buf) - 1);
		s_shared_attr_buf[sizeof(s_shared_attr_buf) - 1] = '\0';
	}
	(void)user_data;
}

static void test_attributes_subscribe_shared(void)
{
	TEST_START("Shared Attribute Subscribe");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	s_shared_attr_received = false;
	memset(s_shared_attr_buf, 0, sizeof(s_shared_attr_buf));

	int ret = tb_attributes_subscribe(client, shared_attr_cb, NULL);
	TEST_ASSERT(ret == 0, "subscribe shared attributes succeeds");

	/* Simulate shared attribute update from server */
	const char *update = "{\"threshold\":42}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", update,
				      strlen(update));
	TEST_ASSERT(s_shared_attr_received == true,
		    "shared attr callback called");
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
static char s_rpc_method[64] = { 0 };
static char s_rpc_params[256] = { 0 };
static uint32_t s_rpc_request_id = 0;

static void server_rpc_cb(const char *method, const char *params_json,
			  uint32_t request_id, void *user_data)
{
	s_server_rpc_received = true;
	if (method)
		strncpy(s_rpc_method, method, sizeof(s_rpc_method) - 1);
	if (params_json)
		strncpy(s_rpc_params, params_json, sizeof(s_rpc_params) - 1);
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
	const char *rpc_msg =
		"{\"method\":\"setLed\",\"params\":{\"pin\":4,\"value\":1}}";
	mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/42", rpc_msg,
				      strlen(rpc_msg));

	TEST_ASSERT(s_server_rpc_received == true,
		    "server RPC callback called");
	TEST_ASSERT(strcmp(s_rpc_method, "setLed") == 0,
		    "RPC method is 'setLed'");
	TEST_ASSERT(s_rpc_request_id == 42, "RPC request_id is 42");
	TEST_ASSERT(strstr(s_rpc_params, "pin") != NULL,
		    "RPC params contain 'pin'");

	/* Send response */
	mock_publish_count = 0;
	ret = tb_rpc_respond(client, 42, "{\"result\":\"ok\"}");
	TEST_ASSERT(ret == 0, "RPC respond succeeds");
	TEST_ASSERT(strstr(mock_publishes[0].topic,
			   "v1/devices/me/rpc/response/42") != NULL,
		    "response published to correct topic");

	ret = tb_rpc_unsubscribe_server(client);
	TEST_ASSERT(ret == 0, "unsubscribe server RPC succeeds");

	destroy_test_client(client);
}

/* ============================================================
 * Test: Client-Side RPC
 * ============================================================ */
static bool s_client_rpc_received = false;
static char s_client_rpc_response[512] = { 0 };

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

	int ret = tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
				 5000);
	TEST_ASSERT(ret == 0, "client RPC request succeeds");
	TEST_ASSERT(mock_publish_count == 1, "RPC request published");

	/* Verify request format */
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "RPC request JSON valid");
	if (root) {
		cJSON *method =
			cJSON_GetObjectItemCaseSensitive(root, "method");
		TEST_ASSERT(method != NULL &&
				    strcmp(method->valuestring, "getTime") == 0,
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

	TEST_ASSERT(s_client_rpc_received == true,
		    "client RPC response received");
	TEST_ASSERT(strstr(s_client_rpc_response, "1700000000") != NULL,
		    "response contains time value");

	destroy_test_client(client);
}

/* ============================================================
 * Test: Provisioning
 * ============================================================ */
static bool s_provision_received = false;
static char s_provision_response[512] = { 0 };

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
		cJSON *dk = cJSON_GetObjectItemCaseSensitive(
			root, "provisionDeviceKey");
		TEST_ASSERT(dk != NULL && strcmp(dk->valuestring,
						 "my_provision_key") == 0,
			    "provisionDeviceKey correct");
		cJSON *dn =
			cJSON_GetObjectItemCaseSensitive(root, "deviceName");
		TEST_ASSERT(dn != NULL &&
				    strcmp(dn->valuestring, "new_device") == 0,
			    "deviceName correct");
		cJSON_Delete(root);
	}

	/* Simulate provision response */
	const char *resp =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
	mqtt_app_mock_deliver_message("/provision/response", resp,
				      strlen(resp));
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
		TEST_ASSERT(sk != NULL &&
				    strcmp(sk->valuestring, "my_secret") == 0,
			    "secretKey correct");
		cJSON *dur =
			cJSON_GetObjectItemCaseSensitive(root, "durationMs");
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
		cJSON *dur =
			cJSON_GetObjectItemCaseSensitive(root, "durationMs");
		TEST_ASSERT(dur != NULL && dur->valueint == 30000,
			    "durationMs correct");
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

/* ============================================================
 * Test: Firmware Update
 * ============================================================ */
static bool s_fw_applied_called = false;
static char s_fw_applied_title[128] = { 0 };
static char s_fw_applied_version[128] = { 0 };

static void fw_applied_cb(const char *new_title, const char *new_version,
			  void *user_data)
{
	s_fw_applied_called = true;
	if (new_title != NULL) {
		strncpy(s_fw_applied_title, new_title,
			sizeof(s_fw_applied_title) - 1);
	}
	if (new_version != NULL) {
		strncpy(s_fw_applied_version, new_version,
			sizeof(s_fw_applied_version) - 1);
	}
	(void)user_data;
}

static int find_last_publish_with_prefix(const char *prefix)
{
	for (int i = mock_publish_count - 1; i >= 0; i--) {
		if (strncmp(mock_publishes[i].topic, prefix, strlen(prefix)) ==
		    0) {
			return i;
		}
	}
	return -1;
}

static int find_last_publish_on_topic(const char *topic)
{
	for (int i = mock_publish_count - 1; i >= 0; i--) {
		if (strcmp(mock_publishes[i].topic, topic) == 0) {
			return i;
		}
	}
	return -1;
}

static bool last_fw_telemetry_matches(const char *expected_state,
					const char *expected_error)
{
	int idx = find_last_publish_on_topic("v1/devices/me/telemetry");
	bool matches = false;
	cJSON *root = NULL;
	cJSON *state = NULL;
	cJSON *error = NULL;

	if (idx < 0) {
		return false;
	}

	root = cJSON_Parse(mock_publishes[idx].message);
	if (root == NULL) {
		return false;
	}

	state = cJSON_GetObjectItemCaseSensitive(root, "fw_state");
	error = cJSON_GetObjectItemCaseSensitive(root, "fw_error");
	matches = cJSON_IsString(state) &&
		  strcmp(state->valuestring, expected_state) == 0;
	if (matches && expected_error != NULL) {
		matches = cJSON_IsString(error) &&
			strcmp(error->valuestring, expected_error) == 0;
	}

	cJSON_Delete(root);
	return matches;
}

static uint32_t parse_fw_request_id(int publish_idx)
{
	unsigned int req_id = 0;

	if (publish_idx < 0) {
		return 0;
	}
	if (sscanf(mock_publishes[publish_idx].topic,
		   "v2/fw/request/%u/chunk/0", &req_id) != 1) {
		return 0;
	}

	return (uint32_t)req_id;
}

static void test_osal_ota_checksum_validation(void)
{
	TEST_START("OSAL OTA SHA256 Validation");
	osal_ota_descriptor_t desc = {
		.title = "hq_platform.bin",
		.version = "1.1.0",
		.checksum = FW_SHA256_ABCDEFGH,
		.checksum_algorithm = "SHA256",
		.total_size = 8,
	};

	osal_ota_abort();
	TEST_ASSERT(osal_ota_begin(&desc) == OSAL_SUCCESS,
		    "ota begin with SHA256 metadata succeeds");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"ABCD", 4) == OSAL_SUCCESS,
		    "first chunk write succeeds");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"EFGH", 4) == OSAL_SUCCESS,
		    "second chunk write succeeds");
	TEST_ASSERT(osal_ota_verify() == OSAL_SUCCESS,
		    "checksum verification succeeds");
	TEST_ASSERT(osal_ota_finish(false) == OSAL_SUCCESS,
		    "ota finish without apply succeeds after verify");
}

static void test_osal_ota_checksum_failures(void)
{
	TEST_START("OSAL OTA Checksum Failures");
	osal_ota_descriptor_t desc = {
		.title = "hq_platform.bin",
		.version = "1.1.0",
		.checksum = FW_SHA256_ABCDEFGH,
		.checksum_algorithm = "SHA256",
		.total_size = 8,
	};

	osal_ota_abort();
	desc.checksum = "0000000000000000000000000000000000000000000000000000000000000000";
	TEST_ASSERT(osal_ota_begin(&desc) == OSAL_SUCCESS,
		    "ota begin succeeds with checksum mismatch test vector");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"ABCD", 4) == OSAL_SUCCESS,
		    "mismatch test first chunk succeeds");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"EFGH", 4) == OSAL_SUCCESS,
		    "mismatch test second chunk succeeds");
	TEST_ASSERT(osal_ota_verify() != OSAL_SUCCESS,
		    "checksum mismatch is rejected");
	TEST_ASSERT(osal_ota_finish(false) != OSAL_SUCCESS,
		    "finish fails when image is not verified");
	osal_ota_abort();

	desc.checksum = "not-a-valid-checksum";
	TEST_ASSERT(osal_ota_begin(&desc) != OSAL_SUCCESS,
		    "malformed checksum encoding is rejected");

	desc.checksum = FW_SHA256_ABCDEFGH;
	desc.checksum_algorithm = "MD5";
	TEST_ASSERT(osal_ota_begin(&desc) != OSAL_SUCCESS,
		    "unsupported checksum algorithm is rejected");

	desc.checksum_algorithm = "SHA256";
	TEST_ASSERT(osal_ota_begin(&desc) == OSAL_SUCCESS,
		    "ota begin succeeds for short stream test");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"ABCD", 4) == OSAL_SUCCESS,
		    "short stream partial write succeeds");
	TEST_ASSERT(osal_ota_verify() != OSAL_SUCCESS,
		    "short stream verify fails before completion");
	TEST_ASSERT(osal_ota_finish(false) != OSAL_SUCCESS,
		    "short stream finish fails before completion");
	osal_ota_abort();

	TEST_ASSERT(osal_ota_begin(&desc) == OSAL_SUCCESS,
		    "ota begin succeeds for oversized stream test");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"ABCDE", 5) == OSAL_SUCCESS,
		    "oversized stream first partial write succeeds");
	TEST_ASSERT(osal_ota_write((const uint8_t *)"FGHI", 4) != OSAL_SUCCESS,
		    "oversized stream is rejected");
	osal_ota_abort();
}

static void test_osal_ota_health_confirmation_api(void)
{
	TEST_START("OSAL OTA Health Confirmation API");
	TEST_ASSERT(osal_ota_init() == OSAL_SUCCESS,
		    "osal_ota_init succeeds on POSIX");
	TEST_ASSERT(osal_ota_needs_confirmation() == false,
		    "POSIX running image does not require confirmation");
	TEST_ASSERT(osal_ota_confirm_running_image() == OSAL_SUCCESS,
		    "confirm running image is a no-op on POSIX");
}

static void test_osal_ota_state_persistence(void)
{
	TEST_START("OSAL OTA State Persistence");
	osal_ota_state_t saved_state = { 0 };
	osal_ota_state_t loaded_state = { 0 };

	strncpy(saved_state.title, "hq_platform.bin",
		sizeof(saved_state.title) - 1);
	strncpy(saved_state.version, "1.2.3",
		sizeof(saved_state.version) - 1);
	strncpy(saved_state.checksum, FW_SHA256_ABCDEFGH,
		sizeof(saved_state.checksum) - 1);
	saved_state.download_state = OSAL_OTA_DOWNLOAD_STATE_DOWNLOADING;
	strncpy(saved_state.last_error, "chunk timeout",
		sizeof(saved_state.last_error) - 1);

	TEST_ASSERT(osal_ota_state_save(&saved_state) == OSAL_SUCCESS,
		    "OTA state save succeeds");
	TEST_ASSERT(osal_ota_state_load(&loaded_state) == OSAL_SUCCESS,
		    "OTA state load succeeds");
	TEST_ASSERT(strcmp(loaded_state.title, saved_state.title) == 0,
		    "loaded OTA title matches");
	TEST_ASSERT(strcmp(loaded_state.version, saved_state.version) == 0,
		    "loaded OTA version matches");
	TEST_ASSERT(strcmp(loaded_state.checksum, saved_state.checksum) == 0,
		    "loaded OTA checksum matches");
	TEST_ASSERT(loaded_state.download_state == saved_state.download_state,
		    "loaded OTA state matches");
	TEST_ASSERT(strcmp(loaded_state.last_error, saved_state.last_error) == 0,
		    "loaded OTA last error matches");
	TEST_ASSERT(osal_ota_state_clear() == OSAL_SUCCESS,
		    "OTA state clear succeeds");
	TEST_ASSERT(osal_ota_state_load(&loaded_state) == OSAL_ERR_EMPTY_SET,
		    "cleared OTA state is no longer present");
}

static void test_firmware_update_flow(void)
{
	TEST_START("Firmware Update Flow");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	s_fw_applied_called = false;
	memset(s_fw_applied_title, 0, sizeof(s_fw_applied_title));
	memset(s_fw_applied_version, 0, sizeof(s_fw_applied_version));

	tb_firmware_update_config_t cfg = {
		.current_title = "hq_platform.bin",
		.current_version = "1.0.0",
		.chunk_size = 4,
		.on_applied = fw_applied_cb,
		.user_data = NULL,
	};

	int ret = tb_firmware_update_init(client, &cfg);
	TEST_ASSERT(ret == 0, "firmware update init succeeds");

	mock_publish_count = 0;

	ret = tb_firmware_update_request_check(client);
	TEST_ASSERT(ret == 0, "firmware attribute check request succeeds");
	TEST_ASSERT(mock_subscribe_count == 2,
		    "firmware metadata request reuses the response subscription");

	/* Request ID from tb_client starts at 1 in this test setup. */
	const char *fw_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
			      "\"fw_version\":\"1.1.0\","
			      "\"fw_checksum\":\"" FW_SHA256_ABCDEFGH "\","
			      "\"fw_checksum_algorithm\":\"SHA256\","
			      "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      fw_meta, strlen(fw_meta));

	int req_idx = find_last_publish_with_prefix("v2/fw/request/");
	TEST_ASSERT(req_idx >= 0, "firmware chunk request was published");

	uint32_t req_id = parse_fw_request_id(req_idx);
	TEST_ASSERT(req_id > 0, "firmware request id parsed");

	char chunk_topic[128];
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/0",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "ABCD", 4);

	int req_idx_chunk1 = find_last_publish_with_prefix("v2/fw/request/");
	TEST_ASSERT(req_idx_chunk1 >= 0, "next chunk request published");
	TEST_ASSERT(strstr(mock_publishes[req_idx_chunk1].topic, "/chunk/1") !=
			    NULL,
		    "requested second firmware chunk");

	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/1",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "EFGH", 4);

	TEST_ASSERT(tb_firmware_update_is_in_progress() == false,
		    "firmware update completed");
	TEST_ASSERT(s_fw_applied_called == true,
		    "firmware applied callback called");
	TEST_ASSERT(strcmp(s_fw_applied_title, "hq_platform.bin") == 0,
		    "applied title updated");
	TEST_ASSERT(strcmp(s_fw_applied_version, "1.1.0") == 0,
		    "applied version updated");

	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

static void test_firmware_update_checksum_mismatch(void)
{
	TEST_START("Firmware Update Checksum Mismatch");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	s_fw_applied_called = false;
	memset(s_fw_applied_title, 0, sizeof(s_fw_applied_title));
	memset(s_fw_applied_version, 0, sizeof(s_fw_applied_version));

	tb_firmware_update_config_t cfg = {
		.current_title = "hq_platform.bin",
		.current_version = "1.0.0",
		.chunk_size = 4,
		.on_applied = fw_applied_cb,
		.user_data = NULL,
	};

	TEST_ASSERT(tb_firmware_update_init(client, &cfg) == 0,
		    "firmware update init succeeds");
	mock_publish_count = 0;
	TEST_ASSERT(tb_firmware_update_request_check(client) == 0,
		    "firmware attribute request succeeds");

	const char *fw_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
			      "\"fw_version\":\"1.1.0\","
			      "\"fw_checksum\":\"0000000000000000000000000000000000000000000000000000000000000000\","
			      "\"fw_checksum_algorithm\":\"SHA256\","
			      "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      fw_meta, strlen(fw_meta));

	uint32_t req_id = parse_fw_request_id(
		find_last_publish_with_prefix("v2/fw/request/"));
	TEST_ASSERT(req_id > 0, "firmware request id parsed");

	char chunk_topic[128];
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/0",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "ABCD", 4);
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/1",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "EFGH", 4);

	TEST_ASSERT(tb_firmware_update_is_in_progress() == false,
		    "firmware update stops after checksum mismatch");
	TEST_ASSERT(s_fw_applied_called == false,
		    "firmware apply callback is not called on checksum mismatch");
	TEST_ASSERT(last_fw_telemetry_matches("FAILED",
				      "firmware checksum mismatch"),
		    "firmware update reports failed checksum mismatch");

	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

static void test_firmware_update_invalid_checksum_metadata(void)
{
	TEST_START("Firmware Update Invalid Checksum Metadata");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	s_fw_applied_called = false;
	tb_firmware_update_config_t cfg = {
		.current_title = "hq_platform.bin",
		.current_version = "1.0.0",
		.chunk_size = 4,
		.on_applied = fw_applied_cb,
		.user_data = NULL,
	};

	TEST_ASSERT(tb_firmware_update_init(client, &cfg) == 0,
		    "firmware update init succeeds");
	mock_publish_count = 0;
	TEST_ASSERT(tb_firmware_update_request_check(client) == 0,
		    "firmware attribute request succeeds");

	const char *bad_checksum_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
				       "\"fw_version\":\"1.1.0\","
				       "\"fw_checksum\":\"not-a-valid-checksum\","
				       "\"fw_checksum_algorithm\":\"SHA256\","
				       "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      bad_checksum_meta,
				      strlen(bad_checksum_meta));
	TEST_ASSERT(find_last_publish_with_prefix("v2/fw/request/") < 0,
		    "no firmware chunk request is published for invalid checksum metadata");
	TEST_ASSERT(last_fw_telemetry_matches("FAILED", "invalid firmware checksum"),
		    "invalid checksum metadata reports a specific failure");

	mock_publish_count = 0;
	TEST_ASSERT(tb_firmware_update_request_check(client) == 0,
		    "second firmware attribute request succeeds");

	const char *bad_algorithm_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
				        "\"fw_version\":\"1.1.0\","
				        "\"fw_checksum\":\"" FW_SHA256_ABCDEFGH "\","
				        "\"fw_checksum_algorithm\":\"MD5\","
				        "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/2",
				      bad_algorithm_meta,
				      strlen(bad_algorithm_meta));
	TEST_ASSERT(find_last_publish_with_prefix("v2/fw/request/") < 0,
		    "no firmware chunk request is published for unsupported algorithm metadata");
	TEST_ASSERT(last_fw_telemetry_matches(
			    "FAILED", "unsupported firmware checksum algorithm"),
		    "unsupported checksum algorithm reports a specific failure");

	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

static void test_firmware_update_oversized_chunk_stream(void)
{
	TEST_START("Firmware Update Oversized Chunk Stream");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	s_fw_applied_called = false;
	tb_firmware_update_config_t cfg = {
		.current_title = "hq_platform.bin",
		.current_version = "1.0.0",
		.chunk_size = 4,
		.on_applied = fw_applied_cb,
		.user_data = NULL,
	};

	TEST_ASSERT(tb_firmware_update_init(client, &cfg) == 0,
		    "firmware update init succeeds");
	mock_publish_count = 0;
	TEST_ASSERT(tb_firmware_update_request_check(client) == 0,
		    "firmware attribute request succeeds");

	const char *fw_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
			      "\"fw_version\":\"1.1.0\","
			      "\"fw_checksum\":\"" FW_SHA256_ABCDEFGH "\","
			      "\"fw_checksum_algorithm\":\"SHA256\","
			      "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      fw_meta, strlen(fw_meta));

	uint32_t req_id = parse_fw_request_id(
		find_last_publish_with_prefix("v2/fw/request/"));
	TEST_ASSERT(req_id > 0, "firmware request id parsed");

	char chunk_topic[128];
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/0",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "ABCDE", 5);
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/1",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "FGHI", 4);

	TEST_ASSERT(tb_firmware_update_is_in_progress() == false,
		    "firmware update stops after oversized stream");
	TEST_ASSERT(s_fw_applied_called == false,
		    "firmware apply callback is not called on oversized stream");
	TEST_ASSERT(last_fw_telemetry_matches("FAILED", "ota write failed"),
		    "oversized chunk stream reports OTA write failure");

	tb_firmware_update_deinit(client);
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
	test_client_connection_callbacks();
	test_client_connection_failure_callback();
	test_client_singleton_init_rejected();

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

	/* Firmware update */
	test_osal_ota_checksum_validation();
	test_osal_ota_checksum_failures();
	test_osal_ota_health_confirmation_api();
	test_osal_ota_state_persistence();
	test_firmware_update_flow();
	test_firmware_update_checksum_mismatch();
	test_firmware_update_invalid_checksum_metadata();
	test_firmware_update_oversized_chunk_stream();

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
