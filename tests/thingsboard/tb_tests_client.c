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

static void test_tls_adapter_configuration(void)
{
	TEST_START("TLS Adapter Configuration");
	mqtt_app_mock_reset();
	mqtt_config_init();

	TEST_ASSERT(mqtt_config_set_string("mqtts://localhost:8883",
				      MQTT_CONFIG_VALUE_ADDRESS),
		    "set mqtts address succeeds");
	TEST_ASSERT(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL),
		    "set ssl enabled succeeds");
	TEST_ASSERT(mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY),
		    "set skip verify disabled succeeds");
	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
					"ca.crt",
					MQTT_CONFIG_VALUE_CERT),
		    "set CA cert source succeeds");
	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
					"client.crt",
					MQTT_CONFIG_VALUE_CLIENT_CERT),
		    "set client cert source succeeds");
	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
					"client.key",
					MQTT_CONFIG_VALUE_CLIENT_KEY),
		    "set client key source succeeds");

	bool ssl = false;
	bool skip_verify = true;
	mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
	const char *source_value = NULL;

	TEST_ASSERT(mqtt_config_get_bool(&ssl, MQTT_CONFIG_VALUE_SSL),
		    "get ssl flag succeeds");
	TEST_ASSERT(ssl == true, "ssl flag is enabled");
	TEST_ASSERT(mqtt_config_get_bool(&skip_verify,
				 MQTT_CONFIG_VALUE_SKIP_VERIFY),
		    "get skip verify flag succeeds");
	TEST_ASSERT(skip_verify == false, "skip verify flag is disabled");

	TEST_ASSERT(mqtt_config_get_cert_source(&source, &source_value,
					MQTT_CONFIG_VALUE_CERT),
		    "get CA cert source succeeds");
	TEST_ASSERT(source == MQTT_CERT_SOURCE_FILE_PATH,
		    "CA cert source is file path");
	TEST_ASSERT(source_value != NULL && strcmp(source_value, "ca.crt") == 0,
		    "CA cert source value matches");

	TEST_ASSERT(mqtt_config_get_cert_source(&source, &source_value,
					MQTT_CONFIG_VALUE_CLIENT_CERT),
		    "get client cert source succeeds");
	TEST_ASSERT(source == MQTT_CERT_SOURCE_FILE_PATH,
		    "client cert source is file path");
	TEST_ASSERT(source_value != NULL && strcmp(source_value, "client.crt") == 0,
		    "client cert source value matches");

	TEST_ASSERT(mqtt_config_get_cert_source(&source, &source_value,
					MQTT_CONFIG_VALUE_CLIENT_KEY),
		    "get client key source succeeds");
	TEST_ASSERT(source == MQTT_CERT_SOURCE_FILE_PATH,
		    "client key source is file path");
	TEST_ASSERT(source_value != NULL && strcmp(source_value, "client.key") == 0,
		    "client key source value matches");
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

static void test_client_qos_and_reconnect_policy(void)
{
	TEST_START("Client QoS And Reconnect Policy");
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
	TEST_ASSERT(tb_client_init(&client, &cfg) == 0,
		    "tb_client_init succeeds with QoS/policy config");
	TEST_ASSERT(client != NULL, "client handle is not NULL");

	TEST_ASSERT(mock_connection_policy.keepalive_sec == 15,
		    "keepalive policy is bounded to minimum");
	TEST_ASSERT(mock_connection_policy.reconnect_initial_delay_ms == 1000,
		    "reconnect initial delay is bounded to minimum");
	TEST_ASSERT(mock_connection_policy.reconnect_max_delay_ms == 1000,
		    "reconnect max delay is bounded and not below initial");
	TEST_ASSERT(mock_connection_policy.reconnect_exponential_backoff == true,
		    "reconnect backoff policy is propagated");

	TEST_ASSERT(tb_client_connect(client) == 0, "tb_client_connect succeeds");
	TEST_ASSERT(tb_client_publish(client, "v1/devices/me/telemetry",
			      "{\"temp\":23}") == 0,
		    "default QoS publish succeeds");
	TEST_ASSERT(mock_publish_count >= 1 &&
			    mock_publishes[mock_publish_count - 1].qos == 0,
		    "default publish QoS is applied");

	TEST_ASSERT(tb_client_publish_with_qos(client, "v1/devices/me/telemetry",
				       "{\"temp\":24}", 1) == 0,
		    "per-call QoS publish succeeds");
	TEST_ASSERT(mock_publish_count >= 2 &&
			    mock_publishes[mock_publish_count - 1].qos == 1,
		    "per-call publish QoS override is applied");

	TEST_ASSERT(tb_client_subscribe(client, "v1/devices/me/test/+",
				noop_topic_cb, 1000) == 0,
		    "default QoS subscribe succeeds");
	TEST_ASSERT(mock_subscribe_count >= 1 &&
			    mock_subscribes[mock_subscribe_count - 1].qos == 0,
		    "default subscribe QoS is applied");

	TEST_ASSERT(tb_client_subscribe_with_qos(client,
					 "v1/devices/me/test2/+", 1,
					 noop_topic_cb, 1000) == 0,
		    "per-call QoS subscribe succeeds");
	TEST_ASSERT(mock_subscribe_count >= 2 &&
			    mock_subscribes[mock_subscribe_count - 1].qos == 1,
		    "per-call subscribe QoS override is applied");

	mqtt_app_mock_simulate_connect_failure(
		MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED);
	TEST_ASSERT(mock_deinit_count == 0,
		    "connect failure path does not deinit mqtt app");

	tb_client_deinit(client);
}

static void test_session_limits_enforcement(void)
{
	TEST_START("Session Limits Enforcement");
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
	TEST_ASSERT(tb_client_init(&client, &cfg) == 0,
		    "tb_client_init succeeds with session limits enabled");
	TEST_ASSERT(client != NULL, "client handle is not NULL");
	TEST_ASSERT(tb_client_connect(client) == 0, "tb_client_connect succeeds");

	int limits_req_idx = find_last_publish_with_prefix(
		"v1/devices/me/rpc/request/");
	TEST_ASSERT(limits_req_idx >= 0,
		    "getSessionLimits client RPC request is published on connect");

	cJSON *limits_req = cJSON_Parse(mock_publishes[limits_req_idx].message);
	TEST_ASSERT(limits_req != NULL, "getSessionLimits request JSON is valid");
	if (limits_req != NULL) {
		cJSON *method =
			cJSON_GetObjectItemCaseSensitive(limits_req, "method");
		TEST_ASSERT(method != NULL && cJSON_IsString(method) &&
				    strcmp(method->valuestring,
					   "getSessionLimits") == 0,
			    "getSessionLimits method is requested");
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
	TEST_ASSERT(ret == 0,
		    "telemetry publish succeeds with payload split/defer under strict limits");
	TEST_ASSERT(mock_publish_count >= 1,
		    "at least one telemetry chunk is published immediately");

	ret = tb_telemetry_send_json(client, "{\"d\":4}");
	TEST_ASSERT(ret != 0,
		    "queue saturation rejects additional telemetry when defer queue is full");

	int published_before_delay = mock_publish_count;
	osal_task_delay_ms(1100);
	ret = tb_telemetry_send_json(client, "{\"e\":5}");
	TEST_ASSERT(ret == 0,
		    "token bucket allows deferred progress after refill interval");
	TEST_ASSERT(mock_publish_count > published_before_delay,
		    "deferred telemetry is flushed after limiter refill");

	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	int limits_req_idx_after_reconnect = find_last_publish_with_prefix(
		"v1/devices/me/rpc/request/");
	TEST_ASSERT(limits_req_idx_after_reconnect >= 0,
		    "getSessionLimits is requested again after reconnect");

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
	TEST_ASSERT(ret == 0,
		    "telemetry publish succeeds after relaxed limits update");
	TEST_ASSERT(mock_publish_count == 1,
		    "relaxed payload limit avoids split after reconnect limits refresh");

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

static void test_mock_transport_delayed_delivery_and_subscription_replay(void)
{
	TEST_START("Mock Transport Delayed Delivery And Replay");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	s_delayed_received = false;
	memset(s_delayed_payload, 0, sizeof(s_delayed_payload));
	mqtt_app_mock_set_replay_subscriptions_on_connect(true);
	TEST_ASSERT(tb_client_subscribe(client, "v1/devices/me/test/+", delayed_transport_cb,
				1000) == 0,
		    "subscription succeeds before delayed delivery");
	TEST_ASSERT(mock_subscription_replay_count == 0,
		    "no replay recorded before reconnect");

	mqtt_app_mock_schedule_message("v1/devices/me/test/1", "later", 5, 250);
	mqtt_app_mock_advance_time_ms(200);
	TEST_ASSERT(s_delayed_received == false, "scheduled message is not delivered early");
	mqtt_app_mock_advance_time_ms(60);
	TEST_ASSERT(s_delayed_received == true, "scheduled message is delivered after delay");
	TEST_ASSERT(strcmp(s_delayed_payload, "later") == 0,
		    "scheduled message payload matches");

	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	TEST_ASSERT(mock_subscription_replay_count >= 1,
		    "active subscriptions are replayed after reconnect");

	destroy_test_client(client);
}

static void test_mock_transport_suback_and_puback_tracking(void)
{
	TEST_START("Mock Transport SUBACK And PUBACK Tracking");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	mqtt_app_mock_set_auto_suback(true, 120);
	mqtt_app_mock_set_auto_puback(true, 80);
	TEST_ASSERT(tb_client_subscribe(client, "v1/devices/me/test/ack", noop_topic_cb,
				1000) == 0,
		    "subscription succeeds with delayed suback tracking");
	TEST_ASSERT(tb_client_publish(client, "v1/devices/me/telemetry",
			      "{\"ack\":1}") == 0,
		    "publish succeeds with delayed puback tracking");
	TEST_ASSERT(mock_suback_count == 0 && mock_puback_count == 0,
		    "ack counters remain pending before time advance");
	mqtt_app_mock_advance_time_ms(90);
	TEST_ASSERT(mock_puback_count == 1,
		    "puback is released after configured delay");
	TEST_ASSERT(mock_suback_count == 0,
		    "suback is still pending before configured delay");
	mqtt_app_mock_advance_time_ms(40);
	TEST_ASSERT(mock_suback_count == 1,
		    "suback is released after configured delay");

	destroy_test_client(client);
}

void run_client_tests(void)
{
	test_client_lifecycle();
	test_tls_adapter_configuration();
	test_client_init_null_params();
	test_client_request_id();
	test_client_connection_callbacks();
	test_client_connection_failure_callback();
	test_client_qos_and_reconnect_policy();
	test_session_limits_enforcement();
	test_client_singleton_init_rejected();
	test_mock_transport_delayed_delivery_and_subscription_replay();
	test_mock_transport_suback_and_puback_tracking();
}
