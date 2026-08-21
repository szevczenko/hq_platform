#include "tb_test_common.h"

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

static void noop_topic_cb(const char *topic, const char *payload,
			  size_t payload_len)
{
	(void)topic;
	(void)payload;
	(void)payload_len;
}

static bool s_delayed_received = false;
static char s_delayed_payload[64] = { 0 };

static void delayed_transport_cb(const char *topic, const char *payload,
				 size_t payload_len)
{
	(void)topic;
	size_t copy_len = payload_len < sizeof(s_delayed_payload) - 1 ?
		payload_len : sizeof(s_delayed_payload) - 1;
	s_delayed_received = true;
	memcpy(s_delayed_payload, payload, copy_len);
	s_delayed_payload[copy_len] = '\0';
}

static void test_client_lifecycle(void)
{
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.client_id = "my_client",
		.device_name = "sensor_1",
	};

	tb_client_t *client = NULL;
	int ret = tb_client_init(&client, &cfg);
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_NOT_NULL(client);

	ret = tb_client_connect(client);
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_TRUE(tb_client_is_connected(client));

	tb_client_disconnect(client);
	TEST_ASSERT_FALSE(tb_client_is_connected(client));

	tb_client_deinit(client);
}

static void test_tls_adapter_configuration(void)
{
	mqtt_app_mock_reset();
	mqtt_config_init();

	TEST_ASSERT_TRUE(mqtt_config_set_string("mqtts://localhost:8883",
						 MQTT_CONFIG_VALUE_ADDRESS));
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL));
	TEST_ASSERT_TRUE(mqtt_config_set_bool(false,
					     MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						    "ca.crt",
						    MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						    "client.crt",
						    MQTT_CONFIG_VALUE_CLIENT_CERT));
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						    "client.key",
						    MQTT_CONFIG_VALUE_CLIENT_KEY));

	bool ssl = false;
	bool skip_verify = true;
	mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
	const char *source_value = NULL;

	TEST_ASSERT_TRUE(mqtt_config_get_bool(&ssl, MQTT_CONFIG_VALUE_SSL));
	TEST_ASSERT_TRUE(ssl);
	TEST_ASSERT_TRUE(mqtt_config_get_bool(&skip_verify,
					     MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_FALSE(skip_verify);

	TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &source_value,
						    MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_EQUAL(MQTT_CERT_SOURCE_FILE_PATH, source);
	TEST_ASSERT_NOT_NULL(source_value);
	TEST_ASSERT_EQUAL_STRING("ca.crt", source_value);

	TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &source_value,
						    MQTT_CONFIG_VALUE_CLIENT_CERT));
	TEST_ASSERT_EQUAL(MQTT_CERT_SOURCE_FILE_PATH, source);
	TEST_ASSERT_NOT_NULL(source_value);
	TEST_ASSERT_EQUAL_STRING("client.crt", source_value);

	TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &source_value,
						    MQTT_CONFIG_VALUE_CLIENT_KEY));
	TEST_ASSERT_EQUAL(MQTT_CERT_SOURCE_FILE_PATH, source);
	TEST_ASSERT_NOT_NULL(source_value);
	TEST_ASSERT_EQUAL_STRING("client.key", source_value);
}

static void test_client_init_null_params(void)
{
	int ret = tb_client_init(NULL, NULL);
	TEST_ASSERT_TRUE(ret != 0);

	tb_client_t *client = NULL;
	tb_client_config_t cfg = { 0 };
	ret = tb_client_init(&client, &cfg);
	TEST_ASSERT_TRUE(ret != 0);
}

static void test_client_request_id(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	uint32_t id1 = tb_client_get_next_request_id(client);
	uint32_t id2 = tb_client_get_next_request_id(client);
	uint32_t id3 = tb_client_get_next_request_id(client);
	TEST_ASSERT_EQUAL(1, id1);
	TEST_ASSERT_EQUAL(2, id2);
	TEST_ASSERT_EQUAL(3, id3);

	destroy_test_client(client);
}

static void test_client_connection_callbacks(void)
{
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
	TEST_ASSERT_TRUE(tb_client_init(&client, &cfg) == 0);
	TEST_ASSERT_NOT_NULL(client);

	TEST_ASSERT_TRUE(tb_client_connect(client) == 0);
	TEST_ASSERT_EQUAL(1, s_connect_cb_count);
	TEST_ASSERT_TRUE(s_conn_event_count >= 1 &&
			 s_conn_event_order[0] == CONN_EVENT_CONNECT);
	TEST_ASSERT_TRUE(s_last_connection_user_data == &callback_cookie);
	TEST_ASSERT_TRUE(s_connect_cb_connected_state);

	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT_EQUAL(1, s_disconnect_cb_count);
	TEST_ASSERT_EQUAL(TB_CLIENT_DISCONNECT_REASON_REMOTE_CLOSE,
			  s_last_disconnect_reason);
	TEST_ASSERT_FALSE(s_disconnect_cb_connected_state);

	mqtt_app_mock_simulate_connect();
	TEST_ASSERT_EQUAL(2, s_connect_cb_count);
	TEST_ASSERT_TRUE(s_conn_event_count >= 3 &&
			 s_conn_event_order[1] == CONN_EVENT_DISCONNECT &&
			 s_conn_event_order[2] == CONN_EVENT_CONNECT);

	tb_client_disconnect(client);
	TEST_ASSERT_EQUAL(2, s_disconnect_cb_count);
	TEST_ASSERT_EQUAL(TB_CLIENT_DISCONNECT_REASON_EXPLICIT,
			  s_last_disconnect_reason);

	tb_client_deinit(client);
}

static void test_client_connection_failure_callback(void)
{
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
	TEST_ASSERT_TRUE(tb_client_init(&client, &cfg) == 0);
	TEST_ASSERT_NOT_NULL(client);

	mqtt_app_mock_simulate_connect_failure(
		MQTT_CONNECT_FAILURE_REASON_CONNACK_REJECTED);
	TEST_ASSERT_EQUAL(1, s_connect_failure_cb_count);
	TEST_ASSERT_EQUAL(TB_CLIENT_CONNECT_FAILURE_REASON_CONNACK_REJECTED,
			  s_last_connect_failure_reason);
	TEST_ASSERT_TRUE(s_last_connection_user_data == &callback_cookie);
	TEST_ASSERT_FALSE(s_connect_failure_cb_connected_state);

	mqtt_app_mock_simulate_connect_failure(
		MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED);
	TEST_ASSERT_EQUAL(2, s_connect_failure_cb_count);
	TEST_ASSERT_EQUAL(
		TB_CLIENT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED,
		s_last_connect_failure_reason);
	TEST_ASSERT_FALSE(s_connect_failure_cb_connected_state);

	tb_client_deinit(client);
}

static void test_client_qos_and_reconnect_policy(void)
{
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.client_id = "my_client",
		.device_name = "sensor_1",
		.use_custom_qos_defaults = true,
		.default_publish_qos = 0,
		.default_subscribe_qos = 0,
		.keepalive_sec = 1,
		.reconnect_initial_delay_ms = 200,
		.reconnect_max_delay_ms = 500,
		.reconnect_exponential_backoff = true,
	};

	tb_client_t *client = NULL;
	TEST_ASSERT_TRUE(tb_client_init(&client, &cfg) == 0);
	TEST_ASSERT_NOT_NULL(client);

	TEST_ASSERT_EQUAL(15, mock_connection_policy.keepalive_sec);
	TEST_ASSERT_EQUAL(1000, mock_connection_policy.reconnect_initial_delay_ms);
	TEST_ASSERT_EQUAL(1000, mock_connection_policy.reconnect_max_delay_ms);
	TEST_ASSERT_TRUE(mock_connection_policy.reconnect_exponential_backoff);

	TEST_ASSERT_TRUE(tb_client_connect(client) == 0);
	TEST_ASSERT_TRUE(tb_client_publish(client, "v1/devices/me/telemetry",
					   "{\"temp\":23}") == 0);
	TEST_ASSERT_TRUE(mock_publish_count >= 1 &&
			 mock_publishes[mock_publish_count - 1].qos == 0);

	TEST_ASSERT_TRUE(tb_client_publish_with_qos(client,
						   "v1/devices/me/telemetry",
						   "{\"temp\":24}", 1) == 0);
	TEST_ASSERT_TRUE(mock_publish_count >= 2 &&
			 mock_publishes[mock_publish_count - 1].qos == 1);

	TEST_ASSERT_TRUE(tb_client_subscribe(client, "v1/devices/me/test/+",
					     noop_topic_cb, 1000) == 0);
	TEST_ASSERT_TRUE(mock_subscribe_count >= 1 &&
			 mock_subscribes[mock_subscribe_count - 1].qos == 0);

	TEST_ASSERT_TRUE(tb_client_subscribe_with_qos(client,
						      "v1/devices/me/test2/+", 1,
						      noop_topic_cb, 1000) == 0);
	TEST_ASSERT_TRUE(mock_subscribe_count >= 2 &&
			 mock_subscribes[mock_subscribe_count - 1].qos == 1);

	mqtt_app_mock_simulate_connect_failure(
		MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED);
	TEST_ASSERT_EQUAL(0, mock_deinit_count);

	tb_client_deinit(client);
}

static void test_session_limits_enforcement(void)
{
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.client_id = "my_client",
		.device_name = "sensor_1",
		.enable_session_limits = true,
		.defer_queue_capacity = 2,
	};

	tb_client_t *client = NULL;
	TEST_ASSERT_TRUE(tb_client_init(&client, &cfg) == 0);
	TEST_ASSERT_NOT_NULL(client);
	TEST_ASSERT_TRUE(tb_client_connect(client) == 0);

	int limits_req_idx = find_last_publish_with_prefix(
		"v1/devices/me/rpc/request/");
	TEST_ASSERT_TRUE(limits_req_idx >= 0);

	cJSON *limits_req = cJSON_Parse(mock_publishes[limits_req_idx].message);
	TEST_ASSERT_NOT_NULL(limits_req);
	if (limits_req != NULL) {
		cJSON *method =
			cJSON_GetObjectItemCaseSensitive(limits_req, "method");
		TEST_ASSERT_TRUE(method != NULL && cJSON_IsString(method) &&
				 strcmp(method->valuestring,
					"getSessionLimits") == 0);
		cJSON_Delete(limits_req);
	}

	uint32_t req_id = parse_topic_suffix_id(mock_publishes[limits_req_idx].topic);
	char limits_resp_topic[128];
	snprintf(limits_resp_topic, sizeof(limits_resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	const char *strict_limits =
		"{\"result\":{\"maxMessageRate\":1,\"maxTelemetryRate\":1,"
		"\"maxTelemetryDataPointsRate\":2,\"maxPayloadSize\":12,"
		"\"maxInflightMessages\":2}}";
	mqtt_app_mock_deliver_message(limits_resp_topic, strict_limits,
				      strlen(strict_limits));

	mock_publish_count = 0;
	int ret = tb_telemetry_send_json(client, "{\"a\":1,\"b\":2,\"c\":3}");
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_TRUE(mock_publish_count >= 1);

	ret = tb_telemetry_send_json(client, "{\"d\":4}");
	TEST_ASSERT_TRUE(ret != 0);

	int published_before_delay = mock_publish_count;
	osal_task_delay_ms(1100);
	ret = tb_telemetry_send_json(client, "{\"e\":5}");
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_TRUE(mock_publish_count > published_before_delay);

	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	int limits_req_idx_after_reconnect = find_last_publish_with_prefix(
		"v1/devices/me/rpc/request/");
	TEST_ASSERT_TRUE(limits_req_idx_after_reconnect >= 0);

	uint32_t req_id2 =
		parse_topic_suffix_id(mock_publishes[limits_req_idx_after_reconnect].topic);
	snprintf(limits_resp_topic, sizeof(limits_resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id2);
	const char *relaxed_limits =
		"{\"result\":{\"maxMessageRate\":20,\"maxTelemetryRate\":20,"
		"\"maxTelemetryDataPointsRate\":20,\"maxPayloadSize\":256,"
		"\"maxInflightMessages\":8}}";
	mqtt_app_mock_deliver_message(limits_resp_topic, relaxed_limits,
				      strlen(relaxed_limits));

	mock_publish_count = 0;
	ret = tb_telemetry_send_json(client,
				     "{\"x\":1,\"y\":2,\"z\":3,\"w\":4}");
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_EQUAL(1, mock_publish_count);

	tb_client_deinit(client);
}

static void test_client_singleton_init_rejected(void)
{
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.device_name = "sensor_1",
	};

	tb_client_t *client1 = NULL;
	tb_client_t *client2 = NULL;

	TEST_ASSERT_TRUE(tb_client_init(&client1, &cfg) == 0);
	TEST_ASSERT_NOT_NULL(client1);
	TEST_ASSERT_TRUE(tb_client_init(&client2, &cfg) != 0);
	TEST_ASSERT_NULL(client2);

	tb_client_deinit(client1);
}

static void test_mock_transport_delayed_delivery_and_subscription_replay(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	s_delayed_received = false;
	memset(s_delayed_payload, 0, sizeof(s_delayed_payload));
	mqtt_app_mock_set_replay_subscriptions_on_connect(true);
	TEST_ASSERT_TRUE(tb_client_subscribe(client, "v1/devices/me/test/+", delayed_transport_cb,
					     1000) == 0);
	TEST_ASSERT_EQUAL(0, mock_subscription_replay_count);

	mqtt_app_mock_schedule_message("v1/devices/me/test/1", "later", 5, 250);
	mqtt_app_mock_advance_time_ms(200);
	TEST_ASSERT_FALSE(s_delayed_received);
	mqtt_app_mock_advance_time_ms(60);
	TEST_ASSERT_TRUE(s_delayed_received);
	TEST_ASSERT_EQUAL_STRING("later", s_delayed_payload);

	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	TEST_ASSERT_TRUE(mock_subscription_replay_count >= 1);

	destroy_test_client(client);
}

static void test_mock_transport_suback_and_puback_tracking(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	mqtt_app_mock_set_auto_suback(true, 120);
	mqtt_app_mock_set_auto_puback(true, 80);
	TEST_ASSERT_TRUE(tb_client_subscribe(client, "v1/devices/me/test/ack", noop_topic_cb,
					     1000) == 0);
	TEST_ASSERT_TRUE(tb_client_publish(client, "v1/devices/me/telemetry",
					   "{\"ack\":1}") == 0);
	TEST_ASSERT_EQUAL(0, mock_suback_count);
	TEST_ASSERT_EQUAL(0, mock_puback_count);
	mqtt_app_mock_advance_time_ms(90);
	TEST_ASSERT_EQUAL(1, mock_puback_count);
	TEST_ASSERT_EQUAL(0, mock_suback_count);
	mqtt_app_mock_advance_time_ms(40);
	TEST_ASSERT_EQUAL(1, mock_suback_count);

	destroy_test_client(client);
}

void run_client_tests(void)
{
	RUN_TEST(test_client_lifecycle);
	RUN_TEST(test_tls_adapter_configuration);
	RUN_TEST(test_client_init_null_params);
	RUN_TEST(test_client_request_id);
	RUN_TEST(test_client_connection_callbacks);
	RUN_TEST(test_client_connection_failure_callback);
	RUN_TEST(test_client_qos_and_reconnect_policy);
	RUN_TEST(test_session_limits_enforcement);
	RUN_TEST(test_client_singleton_init_rejected);
	RUN_TEST(test_mock_transport_delayed_delivery_and_subscription_replay);
	RUN_TEST(test_mock_transport_suback_and_puback_tracking);
}