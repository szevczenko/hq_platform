#include "tb_test_common.h"

static void test_telemetry_send_int(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	int ret = tb_telemetry_send_int(client, "temperature", 25);
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_EQUAL(1, mock_publish_count);
	TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry",
				 mock_publishes[0].topic);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *temp =
			cJSON_GetObjectItemCaseSensitive(root, "temperature");
		TEST_ASSERT_TRUE(temp != NULL && cJSON_IsNumber(temp));
		TEST_ASSERT_TRUE(temp != NULL && temp->valueint == 25);
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_telemetry_send_double(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	int ret = tb_telemetry_send_double(client, "humidity", 65.5);
	TEST_ASSERT_TRUE(ret == 0);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *hum = cJSON_GetObjectItemCaseSensitive(root, "humidity");
		TEST_ASSERT_TRUE(hum != NULL && cJSON_IsNumber(hum));
		TEST_ASSERT_TRUE(hum != NULL && hum->valuedouble > 65.4 &&
				    hum->valuedouble < 65.6);
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_telemetry_send_bool(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	int ret = tb_telemetry_send_bool(client, "active", true);
	TEST_ASSERT_TRUE(ret == 0);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *active =
			cJSON_GetObjectItemCaseSensitive(root, "active");
		TEST_ASSERT_TRUE(active != NULL && cJSON_IsTrue(active));
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_telemetry_send_string(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	int ret = tb_telemetry_send_string(client, "status", "running");
	TEST_ASSERT_TRUE(ret == 0);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *status =
			cJSON_GetObjectItemCaseSensitive(root, "status");
		TEST_ASSERT_TRUE(status != NULL && cJSON_IsString(status));
		TEST_ASSERT_EQUAL_STRING("running", status->valuestring);
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_telemetry_send_json(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	const char *json = "{\"temp\":22,\"hum\":55}";
	int ret = tb_telemetry_send_json(client, json);
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_EQUAL_STRING(json, mock_publishes[0].message);

	destroy_test_client(client);
}

static void test_telemetry_timestamped_json_shape(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	const char *json =
		"{\"ts\":1715232000123,\"values\":{\"temperature\":22.5,\"sample\":7}}";
	int ret = tb_telemetry_send_json(client, json);
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry",
				 mock_publishes[0].topic);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
		cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
		cJSON *temperature = NULL;
		cJSON *sample = NULL;

		TEST_ASSERT_TRUE(ts != NULL && cJSON_IsNumber(ts));
		TEST_ASSERT_TRUE(values != NULL && cJSON_IsObject(values));
		if (values != NULL) {
			temperature =
				cJSON_GetObjectItemCaseSensitive(values,
					"temperature");
			sample = cJSON_GetObjectItemCaseSensitive(values,
					"sample");
			TEST_ASSERT_TRUE(temperature != NULL &&
					 cJSON_IsNumber(temperature));
			TEST_ASSERT_TRUE(sample != NULL && cJSON_IsNumber(sample));
		}

		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_telemetry_batch_split_shape_under_limits(void)
{
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
	TEST_ASSERT_TRUE(tb_client_init(&client, &cfg) == 0);
	TEST_ASSERT_NOT_NULL(client);
	TEST_ASSERT_TRUE(tb_client_connect(client) == 0);

	int limits_req_idx =
		find_last_publish_with_prefix("v1/devices/me/rpc/request/");
	TEST_ASSERT_TRUE(limits_req_idx >= 0);

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
	TEST_ASSERT_TRUE(ret == 0);
	TEST_ASSERT_TRUE(mock_publish_count >= 2);

	int total_fields = 0;
	for (int i = 0; i < mock_publish_count; i++) {
		TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry",
					 mock_publishes[i].topic);

		cJSON *chunk = cJSON_Parse(mock_publishes[i].message);
		TEST_ASSERT_TRUE(chunk != NULL && cJSON_IsObject(chunk));
		if (chunk) {
			int fields = 0;
			for (cJSON *item = chunk->child; item != NULL;
			     item = item->next) {
				fields++;
			}
			TEST_ASSERT_TRUE(fields > 0);
			total_fields += fields;
			cJSON_Delete(chunk);
		}
	}

	TEST_ASSERT_EQUAL(4, total_fields);

	tb_client_deinit(client);
}

static void test_telemetry_publish_failure_propagates(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	mqtt_app_mock_simulate_error_disconnect();

	int ret = tb_telemetry_send_json(client, "{\"temperature\":22}");
	TEST_ASSERT_TRUE(ret != 0);

	ret = tb_attributes_send_json(client, "{\"firmware\":\"1.0.0\"}");
	TEST_ASSERT_TRUE(ret != 0);

	destroy_test_client(client);
}

static void test_telemetry_null_params(void)
{
	int ret = tb_telemetry_send_int(NULL, "key", 1);
	TEST_ASSERT_TRUE(ret != 0);

	tb_client_t *client = create_test_client();
	ret = tb_telemetry_send_int(client, NULL, 1);
	TEST_ASSERT_TRUE(ret != 0);

	destroy_test_client(client);
}

void run_telemetry_tests(void)
{
	RUN_TEST(test_telemetry_send_int);
	RUN_TEST(test_telemetry_send_double);
	RUN_TEST(test_telemetry_send_bool);
	RUN_TEST(test_telemetry_send_string);
	RUN_TEST(test_telemetry_send_json);
	RUN_TEST(test_telemetry_timestamped_json_shape);
	RUN_TEST(test_telemetry_batch_split_shape_under_limits);
	RUN_TEST(test_telemetry_publish_failure_propagates);
	RUN_TEST(test_telemetry_null_params);
}