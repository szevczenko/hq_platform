#include "tb_test_common.h"

static bool s_attr_response_received = false;
static char s_attr_response_buf[512] = { 0 };
static int s_attr_null_response_count = 0;
static int s_attr_response_count = 0;
static int s_attr_success_count = 0;
static int s_attr_timeout_count = 0;
static int s_attr_cancelled_count = 0;
static int s_attr_error_count = 0;

static bool s_shared_attr_received = false;
static char s_shared_attr_buf[512] = { 0 };
static int s_shared_attr_cb_count = 0;
static int s_shared_threshold_cb_count_a = 0;
static int s_shared_threshold_cb_count_b = 0;
static int s_shared_mode_cb_count = 0;
static int s_shared_self_remove_cb_count = 0;
static char s_last_key_a[64] = { 0 };
static char s_last_key_b[64] = { 0 };
static char s_last_key_mode[64] = { 0 };

static bool s_server_rpc_received = false;
static char s_rpc_method[64] = { 0 };
static char s_rpc_params[256] = { 0 };
static uint32_t s_rpc_request_id = 0;
static int s_server_rpc_count = 0;

static bool s_client_rpc_received = false;
static char s_client_rpc_response[512] = { 0 };
static int s_client_rpc_null_count = 0;
static int s_client_rpc_response_count = 0;
static int s_client_rpc_success_count = 0;
static int s_client_rpc_timeout_count = 0;
static int s_client_rpc_cancelled_count = 0;
static int s_client_rpc_error_count = 0;

typedef struct {
	tb_client_t *client;
	tb_shared_attribute_subscription_t *handle;
} self_remove_ctx_t;

static self_remove_ctx_t s_self_remove_ctx = { 0 };

static void reset_attr_state(void)
{
	s_attr_response_received = false;
	s_attr_response_count = 0;
	s_attr_null_response_count = 0;
	s_attr_success_count = 0;
	s_attr_timeout_count = 0;
	s_attr_cancelled_count = 0;
	s_attr_error_count = 0;
	memset(s_attr_response_buf, 0, sizeof(s_attr_response_buf));
}

static void reset_client_rpc_state(void)
{
	s_client_rpc_received = false;
	s_client_rpc_response_count = 0;
	s_client_rpc_null_count = 0;
	s_client_rpc_success_count = 0;
	s_client_rpc_timeout_count = 0;
	s_client_rpc_cancelled_count = 0;
	s_client_rpc_error_count = 0;
	memset(s_client_rpc_response, 0, sizeof(s_client_rpc_response));
}

static void attr_response_cb(tb_request_result_t result,
			     const char *json_response, void *user_data)
{
	s_attr_response_received = true;
	s_attr_response_count++;
	if (result == TB_REQUEST_RESULT_SUCCESS && json_response) {
		s_attr_success_count++;
		strncpy(s_attr_response_buf, json_response,
			sizeof(s_attr_response_buf) - 1);
		s_attr_response_buf[sizeof(s_attr_response_buf) - 1] = '\0';
	} else if (result == TB_REQUEST_RESULT_TIMEOUT) {
		s_attr_timeout_count++;
		s_attr_null_response_count++;
	} else if (result == TB_REQUEST_RESULT_CANCELLED) {
		s_attr_cancelled_count++;
		s_attr_null_response_count++;
	} else {
		s_attr_error_count++;
		s_attr_null_response_count++;
	}
	(void)user_data;
}

static void shared_attr_cb(const char *json_payload, void *user_data)
{
	s_shared_attr_received = true;
	s_shared_attr_cb_count++;
	if (json_payload) {
		strncpy(s_shared_attr_buf, json_payload,
			sizeof(s_shared_attr_buf) - 1);
		s_shared_attr_buf[sizeof(s_shared_attr_buf) - 1] = '\0';
	}
	(void)user_data;
}

static void shared_threshold_cb_a(const char *key, const char *json_payload,
				  void *user_data)
{
	(void)user_data;
	if (json_payload == NULL) {
		return;
	}
	s_shared_threshold_cb_count_a++;
	strncpy(s_last_key_a, key, sizeof(s_last_key_a) - 1);
	s_last_key_a[sizeof(s_last_key_a) - 1] = '\0';
}

static void shared_threshold_cb_b(const char *key, const char *json_payload,
				  void *user_data)
{
	(void)user_data;
	if (json_payload == NULL) {
		return;
	}
	s_shared_threshold_cb_count_b++;
	strncpy(s_last_key_b, key, sizeof(s_last_key_b) - 1);
	s_last_key_b[sizeof(s_last_key_b) - 1] = '\0';
}

static void shared_mode_cb(const char *key, const char *json_payload,
			   void *user_data)
{
	(void)user_data;
	if (json_payload == NULL) {
		return;
	}
	s_shared_mode_cb_count++;
	strncpy(s_last_key_mode, key, sizeof(s_last_key_mode) - 1);
	s_last_key_mode[sizeof(s_last_key_mode) - 1] = '\0';
}

static void shared_threshold_self_remove_cb(const char *key,
					    const char *json_payload,
					    void *user_data)
{
	(void)key;
	(void)json_payload;
	self_remove_ctx_t *ctx = (self_remove_ctx_t *)user_data;
	s_shared_self_remove_cb_count++;
	if (ctx != NULL && ctx->client != NULL && ctx->handle != NULL) {
		(void)tb_attributes_unsubscribe_key(ctx->client, ctx->handle);
	}
}

static void server_rpc_cb(const char *method, const char *params_json,
			  uint32_t request_id, void *user_data)
{
	s_server_rpc_received = true;
	s_server_rpc_count++;
	if (method)
		strncpy(s_rpc_method, method, sizeof(s_rpc_method) - 1);
	if (params_json)
		strncpy(s_rpc_params, params_json, sizeof(s_rpc_params) - 1);
	s_rpc_request_id = request_id;
	(void)user_data;
}

static void client_rpc_cb(tb_request_result_t result,
			  const char *response_json, void *user_data)
{
	s_client_rpc_received = true;
	s_client_rpc_response_count++;
	if (result == TB_REQUEST_RESULT_SUCCESS && response_json) {
		s_client_rpc_success_count++;
		strncpy(s_client_rpc_response, response_json,
			sizeof(s_client_rpc_response) - 1);
		s_client_rpc_response[sizeof(s_client_rpc_response) - 1] = '\0';
	} else if (result == TB_REQUEST_RESULT_TIMEOUT) {
		s_client_rpc_timeout_count++;
		s_client_rpc_null_count++;
	} else if (result == TB_REQUEST_RESULT_CANCELLED) {
		s_client_rpc_cancelled_count++;
		s_client_rpc_null_count++;
	} else {
		s_client_rpc_error_count++;
		s_client_rpc_null_count++;
	}
	(void)user_data;
}

static void test_attributes_send(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	int ret =
		tb_attributes_send_string(client, "firmware_version", "1.2.3");
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", mock_publishes[0].topic);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *fw = cJSON_GetObjectItemCaseSensitive(
			root, "firmware_version");
		TEST_ASSERT_TRUE(fw != NULL && strcmp(fw->valuestring, "1.2.3") == 0);
		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_attributes_json_shape(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	const char *json =
		"{\"firmware_version\":\"1.2.3\",\"serial\":\"SN-001\"}";
	int ret = tb_attributes_send_json(client, json);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", mock_publishes[0].topic);

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root != NULL) {
		cJSON *fw = cJSON_GetObjectItemCaseSensitive(root,
						   "firmware_version");
		cJSON *serial = cJSON_GetObjectItemCaseSensitive(root, "serial");

		TEST_ASSERT_TRUE(fw != NULL && cJSON_IsString(fw));
		TEST_ASSERT_TRUE(serial != NULL && cJSON_IsString(serial));
		if (fw != NULL && serial != NULL) {
			TEST_ASSERT_EQUAL_STRING("1.2.3", fw->valuestring);
			TEST_ASSERT_EQUAL_STRING("SN-001", serial->valuestring);
		}

		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_attributes_request_reconnect_safety(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);

	reset_attr_state();

	const char *keys[] = { "firmware_version" };
	TEST_ASSERT_TRUE(tb_attributes_request_client(client, keys, 1, attr_response_cb, NULL, 5000) == 0);

	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT_EQUAL(1, s_attr_null_response_count);
	TEST_ASSERT_EQUAL(1, s_attr_cancelled_count);

	mqtt_app_mock_simulate_connect();
	TEST_ASSERT_TRUE(tb_attributes_request_client(client, keys, 1, attr_response_cb, NULL, 5000) == 0);

	const char *response = "{\"client\":{\"firmware_version\":\"3.0\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/2",
				      response, strlen(response));
	TEST_ASSERT_EQUAL(2, s_attr_response_count);
	TEST_ASSERT_EQUAL(1, s_attr_success_count);
	TEST_ASSERT_TRUE(strstr(s_attr_response_buf, "firmware_version") != NULL);

	destroy_test_client(client);
}

static void test_attributes_request(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_attr_state();
	const char *keys[] = { "firmware_version", "serial_number" };
	int ret = tb_attributes_request_client(client, keys, 2,
					       attr_response_cb, NULL, 5000);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL(1, mock_publish_count);
	TEST_ASSERT_TRUE(strstr(mock_publishes[0].topic, "v1/devices/me/attributes/request/") != NULL);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *ck =
			cJSON_GetObjectItemCaseSensitive(root, "clientKeys");
		TEST_ASSERT_TRUE(ck != NULL && cJSON_IsString(ck));
		TEST_ASSERT_TRUE(ck != NULL && strstr(ck->valuestring, "firmware_version") != NULL);
		cJSON_Delete(root);
	}
	const char *response = "{\"client\":{\"firmware_version\":\"2.0\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      response, strlen(response));
	TEST_ASSERT_TRUE(s_attr_response_received == true);
	TEST_ASSERT_EQUAL(1, s_attr_success_count);
	TEST_ASSERT_TRUE(strstr(s_attr_response_buf, "firmware_version") != NULL);
	destroy_test_client(client);
}

static void test_attributes_request_shared(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_attr_state();
	const char *keys[] = { "threshold", "mode" };
	int ret = tb_attributes_request_shared(client, keys, 2,
					      attr_response_cb, NULL, 5000);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL(1, mock_publish_count);
	TEST_ASSERT_TRUE(strstr(mock_publishes[0].topic, "v1/devices/me/attributes/request/") != NULL);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "sharedKeys");
		TEST_ASSERT_TRUE(sk != NULL && cJSON_IsString(sk));
		TEST_ASSERT_TRUE(sk != NULL && strstr(sk->valuestring, "threshold") != NULL);
		TEST_ASSERT_TRUE(sk != NULL && strstr(sk->valuestring, "mode") != NULL);
		cJSON_Delete(root);
	}
	const char *response =
		"{\"shared\":{\"threshold\":42,\"mode\":\"auto\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      response, strlen(response));
	TEST_ASSERT_TRUE(s_attr_response_received == true);
	TEST_ASSERT_EQUAL(1, s_attr_success_count);
	TEST_ASSERT_TRUE(strstr(s_attr_response_buf, "threshold") != NULL);
	destroy_test_client(client);
}

static void test_attributes_request_shared_timeout_and_reconnect(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_attr_state();
	const char *keys[] = { "threshold" };
	TEST_ASSERT_TRUE(tb_attributes_request_shared(client, keys, 1, attr_response_cb, NULL, 30) == 0);
	osal_task_delay_ms(120);
	TEST_ASSERT_EQUAL(1, s_attr_response_count);
	TEST_ASSERT_EQUAL(1, s_attr_timeout_count);
	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT_TRUE(tb_attributes_request_shared(client, keys, 1, attr_response_cb, NULL, 5000) != 0);
	mqtt_app_mock_simulate_connect();
	int responses_before_success = s_attr_response_count;
	TEST_ASSERT_TRUE(tb_attributes_request_shared(client, keys, 1, attr_response_cb, NULL, 5000) == 0);
	uint32_t req_id =
		parse_topic_suffix_id(mock_publishes[mock_publish_count - 1].topic);
	char response_topic[128];
	snprintf(response_topic, sizeof(response_topic),
		 "v1/devices/me/attributes/response/%u", req_id);
	mqtt_app_mock_deliver_message(response_topic,
			      "{\"shared\":{\"threshold\":55}}",
			      strlen("{\"shared\":{\"threshold\":55}}"));
	TEST_ASSERT_TRUE(s_attr_response_count == responses_before_success + 1);
	TEST_ASSERT_EQUAL(1, s_attr_success_count);
	TEST_ASSERT_TRUE(strstr(s_attr_response_buf, "threshold") != NULL);
	destroy_test_client(client);
}

static void test_attributes_request_timeout_and_slot_reuse(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_attr_state();
	const char *keys[] = { "firmware_version" };
	TEST_ASSERT_TRUE(tb_attributes_request_client(client, keys, 1, attr_response_cb, NULL, 30) == 0);
	osal_task_delay_ms(120);
	TEST_ASSERT_EQUAL(1, s_attr_response_count);
	TEST_ASSERT_EQUAL(1, s_attr_timeout_count);
	uint32_t timed_out_req_id = parse_topic_suffix_id(mock_publishes[0].topic);
	char late_resp_topic[128];
	snprintf(late_resp_topic, sizeof(late_resp_topic),
		 "v1/devices/me/attributes/response/%u", timed_out_req_id);
	mqtt_app_mock_deliver_message(late_resp_topic,
			      "{\"client\":{\"firmware_version\":\"late\"}}",
			      strlen("{\"client\":{\"firmware_version\":\"late\"}}"));
	TEST_ASSERT_EQUAL(1, s_attr_response_count);
	TEST_ASSERT_TRUE(tb_attributes_request_client(client, keys, 1, attr_response_cb, NULL, 5000) == 0);
	uint32_t active_req_id =
		parse_topic_suffix_id(mock_publishes[mock_publish_count - 1].topic);
	char resp_topic[128];
	snprintf(resp_topic, sizeof(resp_topic),
		 "v1/devices/me/attributes/response/%u", active_req_id);
	mqtt_app_mock_deliver_message(resp_topic,
			      "{\"client\":{\"firmware_version\":\"ok\"}}",
			      strlen("{\"client\":{\"firmware_version\":\"ok\"}}"));
	TEST_ASSERT_EQUAL(2, s_attr_response_count);
	TEST_ASSERT_EQUAL(1, s_attr_success_count);
	TEST_ASSERT_TRUE(strstr(s_attr_response_buf, "firmware_version") != NULL);
	destroy_test_client(client);
}

static void test_attributes_request_max_pending(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_attr_state();
	const char *keys[] = { "firmware_version" };
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		TEST_ASSERT_TRUE(tb_attributes_request_client(client, keys, 1, attr_response_cb, NULL, 5000) == 0);
	}
	TEST_ASSERT_TRUE(tb_attributes_request_client(client, keys, 1, attr_response_cb, NULL, 5000) != 0);
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		uint32_t req_id = parse_topic_suffix_id(mock_publishes[i].topic);
		char topic[128];
		snprintf(topic, sizeof(topic),
			 "v1/devices/me/attributes/response/%u", req_id);
		mqtt_app_mock_deliver_message(topic,
				      "{\"client\":{\"firmware_version\":\"ok\"}}",
				      strlen("{\"client\":{\"firmware_version\":\"ok\"}}"));
	}
	TEST_ASSERT_EQUAL(TB_MAX_PENDING_REQUESTS, s_attr_success_count);
	TEST_ASSERT_EQUAL(0, s_attr_timeout_count);
	destroy_test_client(client);
}

static void test_attributes_subscribe_shared(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	s_shared_attr_received = false;
	memset(s_shared_attr_buf, 0, sizeof(s_shared_attr_buf));
	int ret = tb_attributes_subscribe(client, shared_attr_cb, NULL);
	TEST_ASSERT_EQUAL(0, ret);
	const char *update = "{\"threshold\":42}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", update,
				      strlen(update));
	TEST_ASSERT_TRUE(s_shared_attr_received == true);
	TEST_ASSERT_TRUE(strstr(s_shared_attr_buf, "threshold") != NULL);
	ret = tb_attributes_unsubscribe(client);
	TEST_ASSERT_EQUAL(0, ret);
	destroy_test_client(client);
}

static void test_attributes_subscribe_shared_per_key(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	s_shared_attr_received = false;
	s_shared_attr_cb_count = 0;
	s_shared_threshold_cb_count_a = 0;
	s_shared_threshold_cb_count_b = 0;
	s_shared_mode_cb_count = 0;
	s_shared_self_remove_cb_count = 0;
	memset(s_shared_attr_buf, 0, sizeof(s_shared_attr_buf));
	memset(s_last_key_a, 0, sizeof(s_last_key_a));
	memset(s_last_key_b, 0, sizeof(s_last_key_b));
	memset(s_last_key_mode, 0, sizeof(s_last_key_mode));
	s_self_remove_ctx.client = client;
	s_self_remove_ctx.handle = NULL;
	tb_shared_attribute_subscription_t *threshold_a = NULL;
	tb_shared_attribute_subscription_t *threshold_b = NULL;
	tb_shared_attribute_subscription_t *mode_sub = NULL;
	tb_shared_attribute_subscription_t *self_remove_sub = NULL;
	TEST_ASSERT_TRUE(tb_attributes_subscribe(client, shared_attr_cb, NULL) == 0);
	TEST_ASSERT_TRUE(tb_attributes_subscribe_key(client, "threshold", shared_threshold_cb_a, NULL, &threshold_a) == 0);
	TEST_ASSERT_NOT_NULL(threshold_a);
	TEST_ASSERT_TRUE(tb_attributes_subscribe_key(client, "threshold", shared_threshold_cb_b, NULL, &threshold_b) == 0);
	TEST_ASSERT_NOT_NULL(threshold_b);
	TEST_ASSERT_TRUE(tb_attributes_subscribe_key(client, "mode", shared_mode_cb, NULL, &mode_sub) == 0);
	TEST_ASSERT_NOT_NULL(mode_sub);
	TEST_ASSERT_TRUE(tb_attributes_subscribe_key(client, "threshold", shared_threshold_self_remove_cb, &s_self_remove_ctx, &self_remove_sub) == 0);
	TEST_ASSERT_NOT_NULL(self_remove_sub);
	s_self_remove_ctx.handle = self_remove_sub;
	const char *payload_all = "{\"threshold\":42,\"mode\":\"auto\"}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_all,
				      strlen(payload_all));
	TEST_ASSERT_TRUE(s_shared_attr_received == true);
	TEST_ASSERT_EQUAL(1, s_shared_attr_cb_count);
	TEST_ASSERT_EQUAL(1, s_shared_threshold_cb_count_a);
	TEST_ASSERT_EQUAL(1, s_shared_threshold_cb_count_b);
	TEST_ASSERT_EQUAL(1, s_shared_mode_cb_count);
	TEST_ASSERT_EQUAL(1, s_shared_self_remove_cb_count);
	TEST_ASSERT_EQUAL_STRING("threshold", s_last_key_a);
	TEST_ASSERT_EQUAL_STRING("threshold", s_last_key_b);
	TEST_ASSERT_EQUAL_STRING("mode", s_last_key_mode);
	const char *payload_nonmatch = "{\"other\":1}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_nonmatch,
				      strlen(payload_nonmatch));
	TEST_ASSERT_EQUAL(2, s_shared_attr_cb_count);
	TEST_ASSERT_EQUAL(1, s_shared_threshold_cb_count_a);
	TEST_ASSERT_EQUAL(1, s_shared_threshold_cb_count_b);
	TEST_ASSERT_EQUAL(1, s_shared_mode_cb_count);
	const char *payload_threshold = "{\"threshold\":43}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_threshold,
				      strlen(payload_threshold));
	TEST_ASSERT_EQUAL(2, s_shared_threshold_cb_count_a);
	TEST_ASSERT_EQUAL(2, s_shared_threshold_cb_count_b);
	TEST_ASSERT_EQUAL(1, s_shared_self_remove_cb_count);
	TEST_ASSERT_EQUAL(0, tb_attributes_unsubscribe(client));
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_threshold,
				      strlen(payload_threshold));
	TEST_ASSERT_EQUAL(3, s_shared_attr_cb_count);
	TEST_ASSERT_EQUAL(3, s_shared_threshold_cb_count_a);
	TEST_ASSERT_TRUE(tb_attributes_unsubscribe_key(client, threshold_a) == 0);
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_threshold,
				      strlen(payload_threshold));
	TEST_ASSERT_EQUAL(3, s_shared_threshold_cb_count_a);
	TEST_ASSERT_EQUAL(4, s_shared_threshold_cb_count_b);
	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	const char *payload_mode = "{\"mode\":\"manual\"}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_mode,
				      strlen(payload_mode));
	TEST_ASSERT_EQUAL(2, s_shared_mode_cb_count);
	TEST_ASSERT_TRUE(tb_attributes_unsubscribe_key(client, threshold_b) == 0);
	TEST_ASSERT_TRUE(tb_attributes_unsubscribe_key(client, mode_sub) == 0);
	destroy_test_client(client);
}

static void test_server_side_rpc(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	s_server_rpc_received = false;
	s_server_rpc_count = 0;
	memset(s_rpc_method, 0, sizeof(s_rpc_method));
	memset(s_rpc_params, 0, sizeof(s_rpc_params));
	int ret = tb_rpc_subscribe_server(client, server_rpc_cb, NULL);
	TEST_ASSERT_EQUAL(0, ret);
	const char *rpc_msg =
		"{\"method\":\"setLed\",\"params\":{\"pin\":4,\"value\":1}}";
	mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/42", rpc_msg,
				      strlen(rpc_msg));
	TEST_ASSERT_TRUE(s_server_rpc_received == true);
	TEST_ASSERT_EQUAL_STRING("setLed", s_rpc_method);
	TEST_ASSERT_EQUAL(42, s_rpc_request_id);
	TEST_ASSERT_TRUE(strstr(s_rpc_params, "pin") != NULL);
	mock_publish_count = 0;
	ret = tb_rpc_respond(client, 42, "{\"result\":\"ok\"}");
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_TRUE(strstr(mock_publishes[0].topic, "v1/devices/me/rpc/response/42") != NULL);
	ret = tb_rpc_unsubscribe_server(client);
	TEST_ASSERT_EQUAL(0, ret);
	destroy_test_client(client);
}

static void test_server_side_rpc_malformed_json_and_request_id(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	s_server_rpc_received = false;
	s_server_rpc_count = 0;
	memset(s_rpc_method, 0, sizeof(s_rpc_method));
	memset(s_rpc_params, 0, sizeof(s_rpc_params));
	s_rpc_request_id = 0;
	int ret = tb_rpc_subscribe_server(client, server_rpc_cb, NULL);
	TEST_ASSERT_EQUAL(0, ret);
	mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/101",
			      "{\"method\":\"setLed\",\"params\":",
			      strlen("{\"method\":\"setLed\",\"params\":"));
	TEST_ASSERT_EQUAL(0, s_server_rpc_count);
	mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/not-a-number",
			      "{\"method\":\"setLed\",\"params\":{\"pin\":1}}",
			      strlen("{\"method\":\"setLed\",\"params\":{\"pin\":1}}"));
	TEST_ASSERT_TRUE(s_server_rpc_received == true);
	TEST_ASSERT_EQUAL(0, s_rpc_request_id);
	TEST_ASSERT_EQUAL_STRING("setLed", s_rpc_method);
	ret = tb_rpc_unsubscribe_server(client);
	TEST_ASSERT_EQUAL(0, ret);
	destroy_test_client(client);
}

static void test_client_side_rpc_reconnect_safety(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_client_rpc_state();
	TEST_ASSERT_TRUE(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL, 5000) == 0);
	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT_EQUAL(1, s_client_rpc_null_count);
	TEST_ASSERT_EQUAL(1, s_client_rpc_cancelled_count);
	mqtt_app_mock_simulate_connect();
	TEST_ASSERT_TRUE(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL, 5000) == 0);
	const char *id_start = strrchr(mock_publishes[mock_publish_count - 1].topic,
				      '/');
	TEST_ASSERT_NOT_NULL(id_start);
	uint32_t req_id = (uint32_t)strtoul(id_start + 1, NULL, 10);
	char resp_topic[128];
	snprintf(resp_topic, sizeof(resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	mqtt_app_mock_deliver_message(resp_topic, "{\"time\":1700000010}",
			      strlen("{\"time\":1700000010}"));
	TEST_ASSERT_EQUAL(2, s_client_rpc_response_count);
	TEST_ASSERT_EQUAL(1, s_client_rpc_success_count);
	TEST_ASSERT_TRUE(strstr(s_client_rpc_response, "1700000010") != NULL);
	destroy_test_client(client);
}

static void test_client_side_rpc(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_client_rpc_state();
	int ret = tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			 5000);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL(1, mock_publish_count);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *method =
			cJSON_GetObjectItemCaseSensitive(root, "method");
		TEST_ASSERT_TRUE(method != NULL && strcmp(method->valuestring, "getTime") == 0);
		cJSON_Delete(root);
	}
	const char *id_start = strrchr(mock_publishes[0].topic, '/');
	TEST_ASSERT_NOT_NULL(id_start);
	uint32_t req_id = (uint32_t)strtoul(id_start + 1, NULL, 10);
	char resp_topic[128];
	snprintf(resp_topic, sizeof(resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	const char *resp = "{\"time\":1700000000}";
	mqtt_app_mock_deliver_message(resp_topic, resp, strlen(resp));
	TEST_ASSERT_TRUE(s_client_rpc_received == true);
	TEST_ASSERT_EQUAL(1, s_client_rpc_success_count);
	TEST_ASSERT_TRUE(strstr(s_client_rpc_response, "1700000000") != NULL);
	destroy_test_client(client);
}

static void test_client_side_rpc_timeout_and_slot_reuse(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_client_rpc_state();
	TEST_ASSERT_TRUE(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL, 30) == 0);
	osal_task_delay_ms(120);
	TEST_ASSERT_EQUAL(1, s_client_rpc_response_count);
	TEST_ASSERT_EQUAL(1, s_client_rpc_timeout_count);
	uint32_t timed_out_req_id = parse_topic_suffix_id(mock_publishes[0].topic);
	char late_topic[128];
	snprintf(late_topic, sizeof(late_topic),
		 "v1/devices/me/rpc/response/%u", timed_out_req_id);
	mqtt_app_mock_deliver_message(late_topic, "{\"time\":1}",
			      strlen("{\"time\":1}"));
	TEST_ASSERT_EQUAL(1, s_client_rpc_response_count);
	TEST_ASSERT_TRUE(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL, 5000) == 0);
	uint32_t req_id =
		parse_topic_suffix_id(mock_publishes[mock_publish_count - 1].topic);
	char response_topic[128];
	snprintf(response_topic, sizeof(response_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	mqtt_app_mock_deliver_message(response_topic, "{\"time\":1700001234}",
			      strlen("{\"time\":1700001234}"));
	TEST_ASSERT_EQUAL(2, s_client_rpc_response_count);
	TEST_ASSERT_EQUAL(1, s_client_rpc_success_count);
	TEST_ASSERT_TRUE(strstr(s_client_rpc_response, "1700001234") != NULL);
	destroy_test_client(client);
}

static void test_client_side_rpc_max_pending(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_client_rpc_state();
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		TEST_ASSERT_TRUE(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL, 5000) == 0);
	}
	TEST_ASSERT_TRUE(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL, 5000) != 0);
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		uint32_t req_id = parse_topic_suffix_id(mock_publishes[i].topic);
		char topic[128];
		snprintf(topic, sizeof(topic),
			 "v1/devices/me/rpc/response/%u", req_id);
		mqtt_app_mock_deliver_message(topic, "{\"time\":1700004321}",
				      strlen("{\"time\":1700004321}"));
	}
	TEST_ASSERT_EQUAL(TB_MAX_PENDING_REQUESTS, s_client_rpc_success_count);
	TEST_ASSERT_EQUAL(0, s_client_rpc_timeout_count);
	destroy_test_client(client);
}

void run_attributes_tests(void)
{
	RUN_TEST(test_attributes_send);
	RUN_TEST(test_attributes_json_shape);
	RUN_TEST(test_attributes_request);
	RUN_TEST(test_attributes_request_shared);
	RUN_TEST(test_attributes_request_shared_timeout_and_reconnect);
	RUN_TEST(test_attributes_request_reconnect_safety);
	RUN_TEST(test_attributes_request_timeout_and_slot_reuse);
	RUN_TEST(test_attributes_request_max_pending);
	RUN_TEST(test_attributes_subscribe_shared);
	RUN_TEST(test_attributes_subscribe_shared_per_key);
}

void run_rpc_tests(void)
{
	RUN_TEST(test_server_side_rpc);
	RUN_TEST(test_server_side_rpc_malformed_json_and_request_id);
	RUN_TEST(test_client_side_rpc);
	RUN_TEST(test_client_side_rpc_reconnect_safety);
	RUN_TEST(test_client_side_rpc_timeout_and_slot_reuse);
	RUN_TEST(test_client_side_rpc_max_pending);
}
