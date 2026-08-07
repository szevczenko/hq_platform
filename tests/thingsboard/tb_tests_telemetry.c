#include "tb_test_common.h"

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

static void test_telemetry_timestamped_json_shape(void)
{
	TEST_START("Telemetry Timestamped JSON Shape");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	const char *json =
		"{\"ts\":1715232000123,\"values\":{\"temperature\":22.5,\"sample\":7}}";
	int ret = tb_telemetry_send_json(client, json);
	TEST_ASSERT(ret == 0, "send timestamped telemetry JSON succeeds");
	TEST_ASSERT(strcmp(mock_publishes[0].topic,
			   "v1/devices/me/telemetry") == 0,
		    "timestamped telemetry publishes to telemetry topic");

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "timestamped telemetry JSON is valid");
	if (root) {
		cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
		cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
		cJSON *temperature = NULL;
		cJSON *sample = NULL;

		TEST_ASSERT(ts != NULL && cJSON_IsNumber(ts),
			    "timestamp field is present and numeric");
		TEST_ASSERT(values != NULL && cJSON_IsObject(values),
			    "values object is present");
		if (values != NULL) {
			temperature =
				cJSON_GetObjectItemCaseSensitive(values, "temperature");
			sample = cJSON_GetObjectItemCaseSensitive(values, "sample");
			TEST_ASSERT(temperature != NULL && cJSON_IsNumber(temperature),
				    "values.temperature is numeric");
			TEST_ASSERT(sample != NULL && cJSON_IsNumber(sample),
				    "values.sample is numeric");
		}

		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_telemetry_batch_split_shape_under_limits(void)
{
	TEST_START("Telemetry Batch Split Shape Under Limits");
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://tb.example.com:1883",
		.access_token = "my_device_token",
		.client_id = "my_client",
		.device_name = "sensor_1",
		.enable_session_limits = true,
		.defer_queue_capacity = 8,
	};

	tb_client_t *client = NULL;
	TEST_ASSERT(tb_client_init(&client, &cfg) == 0,
		    "tb_client_init succeeds with session limits enabled");
	TEST_ASSERT(client != NULL, "client handle is not NULL");
	TEST_ASSERT(tb_client_connect(client) == 0, "tb_client_connect succeeds");

	int limits_req_idx =
		find_last_publish_with_prefix("v1/devices/me/rpc/request/");
	TEST_ASSERT(limits_req_idx >= 0,
		    "getSessionLimits request is published on connect");

	uint32_t req_id = parse_topic_suffix_id(mock_publishes[limits_req_idx].topic);
	char limits_resp_topic[128];
	snprintf(limits_resp_topic, sizeof(limits_resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	const char *strict_limits =
		"{\"result\":{\"maxMessageRate\":20,\"maxTelemetryRate\":20,"
		"\"maxTelemetryDataPointsRate\":20,\"maxPayloadSize\":16,"
		"\"maxInflightMessages\":8}}";
	mqtt_app_mock_deliver_message(limits_resp_topic, strict_limits,
			      strlen(strict_limits));

	mock_publish_count = 0;
	int ret = tb_telemetry_send_json(client, "{\"a\":1,\"b\":2,\"c\":3,\"d\":4}");
	TEST_ASSERT(ret == 0,
		    "batch telemetry publish succeeds under strict payload limit");
	TEST_ASSERT(mock_publish_count >= 2,
		    "batch telemetry is split into multiple publishes");

	int total_fields = 0;
	for (int i = 0; i < mock_publish_count; i++) {
		TEST_ASSERT(strcmp(mock_publishes[i].topic,
				   "v1/devices/me/telemetry") == 0,
			    "each split chunk uses telemetry topic");

		cJSON *chunk = cJSON_Parse(mock_publishes[i].message);
		TEST_ASSERT(chunk != NULL && cJSON_IsObject(chunk),
			    "each split chunk payload is a JSON object");
		if (chunk) {
			int fields = 0;
			for (cJSON *item = chunk->child; item != NULL;
			     item = item->next) {
				fields++;
			}
			TEST_ASSERT(fields > 0,
			    "each split chunk contains at least one field");
			total_fields += fields;
			cJSON_Delete(chunk);
		}
	}

	TEST_ASSERT(total_fields == 4,
		    "split chunks preserve all original telemetry fields");

	tb_client_deinit(client);
}

static void test_telemetry_publish_failure_propagates(void)
{
	TEST_START("Telemetry Publish Failure Propagates");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	mqtt_app_mock_simulate_error_disconnect();

	int ret = tb_telemetry_send_json(client, "{\"temperature\":22}");
	TEST_ASSERT(ret != 0,
		    "telemetry send returns error when transport publish fails");

	ret = tb_attributes_send_json(client, "{\"firmware\":\"1.0.0\"}");
	TEST_ASSERT(ret != 0,
		    "attribute send returns error when transport publish fails");

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

void run_telemetry_tests(void)
{
	test_telemetry_send_int();
	test_telemetry_send_double();
	test_telemetry_send_bool();
	test_telemetry_send_string();
	test_telemetry_send_json();
	test_telemetry_timestamped_json_shape();
	test_telemetry_batch_split_shape_under_limits();
	test_telemetry_publish_failure_propagates();
	test_telemetry_null_params();
}
