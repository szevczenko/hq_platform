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

static void test_attributes_json_shape(void)
{
	TEST_START("Attributes JSON Shape");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	const char *json =
		"{\"firmware_version\":\"1.2.3\",\"serial\":\"SN-001\"}";
	int ret = tb_attributes_send_json(client, json);
	TEST_ASSERT(ret == 0, "send attributes JSON succeeds");
	TEST_ASSERT(strcmp(mock_publishes[0].topic,
			   "v1/devices/me/attributes") == 0,
		    "attributes JSON publishes to attribute topic");

	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "attributes JSON payload is valid");
	if (root != NULL) {
		cJSON *fw = cJSON_GetObjectItemCaseSensitive(root,
						   "firmware_version");
		cJSON *serial = cJSON_GetObjectItemCaseSensitive(root, "serial");

		TEST_ASSERT(fw != NULL && cJSON_IsString(fw),
			    "firmware_version field is present and string");
		TEST_ASSERT(serial != NULL && cJSON_IsString(serial),
			    "serial field is present and string");
		if (fw != NULL && serial != NULL) {
			TEST_ASSERT(strcmp(fw->valuestring, "1.2.3") == 0,
			    "firmware_version value matches input JSON");
			TEST_ASSERT(strcmp(serial->valuestring, "SN-001") == 0,
			    "serial value matches input JSON");
		}

		cJSON_Delete(root);
	}

	destroy_test_client(client);
}

static void test_attributes_request_reconnect_safety(void)
{
	TEST_START("Attributes Request Reconnect Safety");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");

	reset_attr_state();

	const char *keys[] = { "firmware_version" };
	TEST_ASSERT(tb_attributes_request_client(client, keys, 1,
					attr_response_cb, NULL,
					5000) == 0,
		    "first attribute request succeeds");

	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT(s_attr_null_response_count == 1,
		    "pending attribute request is failed on disconnect");
	TEST_ASSERT(s_attr_cancelled_count == 1,
		    "pending attribute request reports cancelled status");

	mqtt_app_mock_simulate_connect();
	TEST_ASSERT(tb_attributes_request_client(client, keys, 1,
					attr_response_cb, NULL,
					5000) == 0,
		    "attribute request succeeds after reconnect");

	const char *response = "{\"client\":{\"firmware_version\":\"3.0\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/2",
				      response, strlen(response));
	TEST_ASSERT(s_attr_response_count == 2,
		    "attribute callback called exactly once per request");
	TEST_ASSERT(s_attr_success_count == 1,
		    "only reconnect response completes with success status");
	TEST_ASSERT(strstr(s_attr_response_buf, "firmware_version") != NULL,
		    "attribute response is delivered after reconnect");

	destroy_test_client(client);
}

static void test_attributes_request(void)
{
	TEST_START("Attributes Request & Response");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_attr_state();
	const char *keys[] = { "firmware_version", "serial_number" };
	int ret = tb_attributes_request_client(client, keys, 2,
					       attr_response_cb, NULL, 5000);
	TEST_ASSERT(ret == 0, "attribute request succeeds");
	TEST_ASSERT(mock_publish_count == 1, "request message published");
	TEST_ASSERT(strstr(mock_publishes[0].topic,
			   "v1/devices/me/attributes/request/") != NULL,
		    "request published to correct topic");
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "request JSON valid");
	if (root) {
		cJSON *ck =
			cJSON_GetObjectItemCaseSensitive(root, "clientKeys");
		TEST_ASSERT(ck != NULL && cJSON_IsString(ck), "clientKeys present");
		TEST_ASSERT(ck != NULL && strstr(ck->valuestring,
						 "firmware_version") != NULL,
			    "clientKeys contains firmware_version");
		cJSON_Delete(root);
	}
	const char *response = "{\"client\":{\"firmware_version\":\"2.0\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      response, strlen(response));
	TEST_ASSERT(s_attr_response_received == true,
		    "attribute response callback called");
	TEST_ASSERT(s_attr_success_count == 1,
		    "attribute response callback status is success");
	TEST_ASSERT(strstr(s_attr_response_buf, "firmware_version") != NULL,
		    "response contains firmware_version");
	destroy_test_client(client);
}

static void test_attributes_request_shared(void)
{
	TEST_START("Shared Attributes Request & Response");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_attr_state();
	const char *keys[] = { "threshold", "mode" };
	int ret = tb_attributes_request_shared(client, keys, 2,
					      attr_response_cb, NULL, 5000);
	TEST_ASSERT(ret == 0, "shared attribute request succeeds");
	TEST_ASSERT(mock_publish_count == 1, "shared request message published");
	TEST_ASSERT(strstr(mock_publishes[0].topic,
			   "v1/devices/me/attributes/request/") != NULL,
		    "shared request published to correct topic");
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "shared request JSON valid");
	if (root) {
		cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "sharedKeys");
		TEST_ASSERT(sk != NULL && cJSON_IsString(sk),
			    "sharedKeys field present");
		TEST_ASSERT(sk != NULL && strstr(sk->valuestring, "threshold") != NULL,
			    "sharedKeys contains threshold");
		TEST_ASSERT(sk != NULL && strstr(sk->valuestring, "mode") != NULL,
			    "sharedKeys contains mode");
		cJSON_Delete(root);
	}
	const char *response =
		"{\"shared\":{\"threshold\":42,\"mode\":\"auto\"}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      response, strlen(response));
	TEST_ASSERT(s_attr_response_received == true,
		    "shared response callback called");
	TEST_ASSERT(s_attr_success_count == 1,
		    "shared response callback status is success");
	TEST_ASSERT(strstr(s_attr_response_buf, "threshold") != NULL,
		    "shared response contains threshold");
	destroy_test_client(client);
}

static void test_attributes_request_shared_timeout_and_reconnect(void)
{
	TEST_START("Shared Attributes Request Timeout And Reconnect");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_attr_state();
	const char *keys[] = { "threshold" };
	TEST_ASSERT(tb_attributes_request_shared(client, keys, 1,
					 attr_response_cb, NULL,
					 30) == 0,
		    "shared attribute request with short timeout succeeds");
	osal_task_delay_ms(120);
	TEST_ASSERT(s_attr_response_count == 1,
		    "shared attribute timeout callback fired once");
	TEST_ASSERT(s_attr_timeout_count == 1,
		    "shared attribute timeout status reported");
	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT(tb_attributes_request_shared(client, keys, 1,
					 attr_response_cb, NULL,
					 5000) != 0,
		    "shared request fails while disconnected");
	mqtt_app_mock_simulate_connect();
	int responses_before_success = s_attr_response_count;
	TEST_ASSERT(tb_attributes_request_shared(client, keys, 1,
					 attr_response_cb, NULL,
					 5000) == 0,
		    "shared request succeeds after reconnect");
	uint32_t req_id =
		parse_topic_suffix_id(mock_publishes[mock_publish_count - 1].topic);
	char response_topic[128];
	snprintf(response_topic, sizeof(response_topic),
		 "v1/devices/me/attributes/response/%u", req_id);
	mqtt_app_mock_deliver_message(response_topic,
			      "{\"shared\":{\"threshold\":55}}",
			      strlen("{\"shared\":{\"threshold\":55}}"));
	TEST_ASSERT(s_attr_response_count == responses_before_success + 1,
		    "shared callback is dispatched for reconnect request response");
	TEST_ASSERT(s_attr_success_count == 1,
		    "shared reconnect request completes with success");
	TEST_ASSERT(strstr(s_attr_response_buf, "threshold") != NULL,
		    "shared reconnect response payload delivered");
	destroy_test_client(client);
}

static void test_attributes_request_timeout_and_slot_reuse(void)
{
	TEST_START("Attributes Request Timeout And Slot Reuse");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_attr_state();
	const char *keys[] = { "firmware_version" };
	TEST_ASSERT(tb_attributes_request_client(client, keys, 1,
					attr_response_cb, NULL,
					30) == 0,
		    "attribute request with short timeout succeeds");
	osal_task_delay_ms(120);
	TEST_ASSERT(s_attr_response_count == 1,
		    "attribute timeout callback fired once");
	TEST_ASSERT(s_attr_timeout_count == 1,
		    "attribute timeout status reported");
	uint32_t timed_out_req_id = parse_topic_suffix_id(mock_publishes[0].topic);
	char late_resp_topic[128];
	snprintf(late_resp_topic, sizeof(late_resp_topic),
		 "v1/devices/me/attributes/response/%u", timed_out_req_id);
	mqtt_app_mock_deliver_message(late_resp_topic,
			      "{\"client\":{\"firmware_version\":\"late\"}}",
			      strlen("{\"client\":{\"firmware_version\":\"late\"}}"));
	TEST_ASSERT(s_attr_response_count == 1,
		    "late attribute response after deadline is ignored");
	TEST_ASSERT(tb_attributes_request_client(client, keys, 1,
					attr_response_cb, NULL,
					5000) == 0,
		    "slot reused after timeout for new request");
	uint32_t active_req_id =
		parse_topic_suffix_id(mock_publishes[mock_publish_count - 1].topic);
	char resp_topic[128];
	snprintf(resp_topic, sizeof(resp_topic),
		 "v1/devices/me/attributes/response/%u", active_req_id);
	mqtt_app_mock_deliver_message(resp_topic,
			      "{\"client\":{\"firmware_version\":\"ok\"}}",
			      strlen("{\"client\":{\"firmware_version\":\"ok\"}}"));
	TEST_ASSERT(s_attr_response_count == 2,
		    "second attribute callback delivered for reused slot");
	TEST_ASSERT(s_attr_success_count == 1,
		    "second attribute callback reports success");
	TEST_ASSERT(strstr(s_attr_response_buf, "firmware_version") != NULL,
		    "attribute success response payload delivered");
	destroy_test_client(client);
}

static void test_attributes_request_max_pending(void)
{
	TEST_START("Attributes Request Max Pending");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_attr_state();
	const char *keys[] = { "firmware_version" };
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		TEST_ASSERT(tb_attributes_request_client(client, keys, 1,
						attr_response_cb, NULL,
						5000) == 0,
			    "attribute request accepted while slots remain");
	}
	TEST_ASSERT(tb_attributes_request_client(client, keys, 1,
					attr_response_cb, NULL,
					5000) != 0,
		    "attribute request fails when pending slots are full");
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		uint32_t req_id = parse_topic_suffix_id(mock_publishes[i].topic);
		char topic[128];
		snprintf(topic, sizeof(topic),
			 "v1/devices/me/attributes/response/%u", req_id);
		mqtt_app_mock_deliver_message(topic,
				      "{\"client\":{\"firmware_version\":\"ok\"}}",
				      strlen("{\"client\":{\"firmware_version\":\"ok\"}}"));
	}
	TEST_ASSERT(s_attr_success_count == TB_MAX_PENDING_REQUESTS,
		    "all pending attribute requests complete successfully");
	TEST_ASSERT(s_attr_timeout_count == 0,
		    "no attribute timeout while responses arrive before deadline");
	destroy_test_client(client);
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

static void test_attributes_subscribe_shared_per_key(void)
{
	TEST_START("Shared Attribute Per-Key Subscribe");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
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
	TEST_ASSERT(tb_attributes_subscribe(client, shared_attr_cb, NULL) == 0,
		    "wildcard shared subscription succeeds");
	TEST_ASSERT(tb_attributes_subscribe_key(client, "threshold",
					shared_threshold_cb_a,
					NULL, &threshold_a) == 0,
		    "first threshold keyed subscription succeeds");
	TEST_ASSERT(threshold_a != NULL,
		    "first threshold keyed subscription handle is returned");
	TEST_ASSERT(tb_attributes_subscribe_key(client, "threshold",
					shared_threshold_cb_b,
					NULL, &threshold_b) == 0,
		    "second threshold keyed subscription succeeds");
	TEST_ASSERT(threshold_b != NULL,
		    "second threshold keyed subscription handle is returned");
	TEST_ASSERT(tb_attributes_subscribe_key(client, "mode", shared_mode_cb,
					NULL, &mode_sub) == 0,
		    "mode keyed subscription succeeds");
	TEST_ASSERT(mode_sub != NULL,
		    "mode keyed subscription handle is returned");
	TEST_ASSERT(tb_attributes_subscribe_key(client, "threshold",
					shared_threshold_self_remove_cb,
					&s_self_remove_ctx,
					&self_remove_sub) == 0,
		    "self-removing threshold keyed subscription succeeds");
	TEST_ASSERT(self_remove_sub != NULL,
		    "self-removing keyed subscription handle is returned");
	s_self_remove_ctx.handle = self_remove_sub;
	const char *payload_all = "{\"threshold\":42,\"mode\":\"auto\"}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_all,
				      strlen(payload_all));
	TEST_ASSERT(s_shared_attr_received == true,
		    "wildcard shared callback receives update");
	TEST_ASSERT(s_shared_attr_cb_count == 1,
		    "wildcard callback called once for first update");
	TEST_ASSERT(s_shared_threshold_cb_count_a == 1,
		    "first keyed threshold callback called for matching key");
	TEST_ASSERT(s_shared_threshold_cb_count_b == 1,
		    "second keyed threshold callback called for matching key");
	TEST_ASSERT(s_shared_mode_cb_count == 1,
		    "mode keyed callback called for matching key");
	TEST_ASSERT(s_shared_self_remove_cb_count == 1,
		    "self-removing keyed callback called once");
	TEST_ASSERT(strcmp(s_last_key_a, "threshold") == 0,
		    "first keyed callback receives subscribed key name");
	TEST_ASSERT(strcmp(s_last_key_b, "threshold") == 0,
		    "second keyed callback receives subscribed key name");
	TEST_ASSERT(strcmp(s_last_key_mode, "mode") == 0,
		    "mode keyed callback receives subscribed key name");
	const char *payload_nonmatch = "{\"other\":1}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_nonmatch,
				      strlen(payload_nonmatch));
	TEST_ASSERT(s_shared_attr_cb_count == 2,
		    "wildcard callback still receives nonmatching updates");
	TEST_ASSERT(s_shared_threshold_cb_count_a == 1,
		    "keyed callback ignores nonmatching payload keys");
	TEST_ASSERT(s_shared_threshold_cb_count_b == 1,
		    "second keyed callback ignores nonmatching payload keys");
	TEST_ASSERT(s_shared_mode_cb_count == 1,
		    "mode keyed callback ignores nonmatching payload keys");
	const char *payload_threshold = "{\"threshold\":43}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_threshold,
				      strlen(payload_threshold));
	TEST_ASSERT(s_shared_threshold_cb_count_a == 2,
		    "first keyed threshold callback is called again");
	TEST_ASSERT(s_shared_threshold_cb_count_b == 2,
		    "second keyed threshold callback is called again");
	TEST_ASSERT(s_shared_self_remove_cb_count == 1,
		    "self-removing keyed callback is not called after removal");
	TEST_ASSERT(tb_attributes_unsubscribe(client) == 0,
		    "wildcard unsubscribe succeeds without removing keyed subscriptions");
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_threshold,
				      strlen(payload_threshold));
	TEST_ASSERT(s_shared_attr_cb_count == 3,
		    "wildcard callback is not called after wildcard unsubscribe");
	TEST_ASSERT(s_shared_threshold_cb_count_a == 3,
		    "keyed subscription remains active after wildcard unsubscribe");
	TEST_ASSERT(tb_attributes_unsubscribe_key(client, threshold_a) == 0,
		    "first keyed threshold unsubscribe succeeds");
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_threshold,
				      strlen(payload_threshold));
	TEST_ASSERT(s_shared_threshold_cb_count_a == 3,
		    "first keyed threshold callback stops after unsubscribe");
	TEST_ASSERT(s_shared_threshold_cb_count_b == 4,
		    "second keyed threshold callback remains active");
	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	const char *payload_mode = "{\"mode\":\"manual\"}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes", payload_mode,
				      strlen(payload_mode));
	TEST_ASSERT(s_shared_mode_cb_count == 2,
		    "mode keyed callback survives reconnect");
	TEST_ASSERT(tb_attributes_unsubscribe_key(client, threshold_b) == 0,
		    "second keyed threshold unsubscribe succeeds");
	TEST_ASSERT(tb_attributes_unsubscribe_key(client, mode_sub) == 0,
		    "mode keyed unsubscribe succeeds");
	destroy_test_client(client);
}

static void test_server_side_rpc(void)
{
	TEST_START("Server-Side RPC");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	s_server_rpc_received = false;
	s_server_rpc_count = 0;
	memset(s_rpc_method, 0, sizeof(s_rpc_method));
	memset(s_rpc_params, 0, sizeof(s_rpc_params));
	int ret = tb_rpc_subscribe_server(client, server_rpc_cb, NULL);
	TEST_ASSERT(ret == 0, "subscribe server RPC succeeds");
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

static void test_server_side_rpc_malformed_json_and_request_id(void)
{
	TEST_START("Server-Side RPC Malformed JSON And Request ID Edge");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	s_server_rpc_received = false;
	s_server_rpc_count = 0;
	memset(s_rpc_method, 0, sizeof(s_rpc_method));
	memset(s_rpc_params, 0, sizeof(s_rpc_params));
	s_rpc_request_id = 0;
	int ret = tb_rpc_subscribe_server(client, server_rpc_cb, NULL);
	TEST_ASSERT(ret == 0, "subscribe server RPC succeeds");
	mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/101",
			      "{\"method\":\"setLed\",\"params\":",
			      strlen("{\"method\":\"setLed\",\"params\":"));
	TEST_ASSERT(s_server_rpc_count == 0,
		    "malformed RPC JSON does not dispatch callback");
	mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/not-a-number",
			      "{\"method\":\"setLed\",\"params\":{\"pin\":1}}",
			      strlen("{\"method\":\"setLed\",\"params\":{\"pin\":1}}"));
	TEST_ASSERT(s_server_rpc_received == true,
		    "valid RPC payload dispatches callback even with nonnumeric topic suffix");
	TEST_ASSERT(s_rpc_request_id == 0,
		    "nonnumeric RPC request id maps to zero");
	TEST_ASSERT(strcmp(s_rpc_method, "setLed") == 0,
		    "method parsing remains correct with request id edge case");
	ret = tb_rpc_unsubscribe_server(client);
	TEST_ASSERT(ret == 0, "unsubscribe server RPC succeeds");
	destroy_test_client(client);
}

static void test_client_side_rpc_reconnect_safety(void)
{
	TEST_START("Client RPC Reconnect Safety");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_client_rpc_state();
	TEST_ASSERT(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			   5000) == 0,
		    "first client RPC request succeeds");
	mqtt_app_mock_simulate_remote_disconnect();
	TEST_ASSERT(s_client_rpc_null_count == 1,
		    "pending client RPC request is failed on disconnect");
	TEST_ASSERT(s_client_rpc_cancelled_count == 1,
		    "pending client RPC request reports cancelled status");
	mqtt_app_mock_simulate_connect();
	TEST_ASSERT(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			   5000) == 0,
		    "client RPC request succeeds after reconnect");
	const char *id_start = strrchr(mock_publishes[mock_publish_count - 1].topic,
				      '/');
	TEST_ASSERT(id_start != NULL,
		    "reconnected client RPC publish includes request ID");
	uint32_t req_id = (uint32_t)strtoul(id_start + 1, NULL, 10);
	char resp_topic[128];
	snprintf(resp_topic, sizeof(resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	mqtt_app_mock_deliver_message(resp_topic, "{\"time\":1700000010}",
			      strlen("{\"time\":1700000010}"));
	TEST_ASSERT(s_client_rpc_response_count == 2,
		    "client RPC callback called exactly once per request");
	TEST_ASSERT(s_client_rpc_success_count == 1,
		    "only reconnect response completes with success status");
	TEST_ASSERT(strstr(s_client_rpc_response, "1700000010") != NULL,
		    "client RPC response is delivered after reconnect");
	destroy_test_client(client);
}

static void test_client_side_rpc(void)
{
	TEST_START("Client-Side RPC");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_client_rpc_state();
	int ret = tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			 5000);
	TEST_ASSERT(ret == 0, "client RPC request succeeds");
	TEST_ASSERT(mock_publish_count == 1, "RPC request published");
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
	const char *id_start = strrchr(mock_publishes[0].topic, '/');
	TEST_ASSERT(id_start != NULL, "topic has request ID");
	uint32_t req_id = (uint32_t)strtoul(id_start + 1, NULL, 10);
	char resp_topic[128];
	snprintf(resp_topic, sizeof(resp_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	const char *resp = "{\"time\":1700000000}";
	mqtt_app_mock_deliver_message(resp_topic, resp, strlen(resp));
	TEST_ASSERT(s_client_rpc_received == true,
		    "client RPC response received");
	TEST_ASSERT(s_client_rpc_success_count == 1,
		    "client RPC response callback status is success");
	TEST_ASSERT(strstr(s_client_rpc_response, "1700000000") != NULL,
		    "response contains time value");
	destroy_test_client(client);
}

static void test_client_side_rpc_timeout_and_slot_reuse(void)
{
	TEST_START("Client RPC Timeout And Slot Reuse");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_client_rpc_state();
	TEST_ASSERT(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			   30) == 0,
		    "client RPC request with short timeout succeeds");
	osal_task_delay_ms(120);
	TEST_ASSERT(s_client_rpc_response_count == 1,
		    "client RPC timeout callback fired once");
	TEST_ASSERT(s_client_rpc_timeout_count == 1,
		    "client RPC timeout status reported");
	uint32_t timed_out_req_id = parse_topic_suffix_id(mock_publishes[0].topic);
	char late_topic[128];
	snprintf(late_topic, sizeof(late_topic),
		 "v1/devices/me/rpc/response/%u", timed_out_req_id);
	mqtt_app_mock_deliver_message(late_topic, "{\"time\":1}",
			      strlen("{\"time\":1}"));
	TEST_ASSERT(s_client_rpc_response_count == 1,
		    "late client RPC response after deadline is ignored");
	TEST_ASSERT(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			   5000) == 0,
		    "client RPC request succeeds after timeout slot is freed");
	uint32_t req_id =
		parse_topic_suffix_id(mock_publishes[mock_publish_count - 1].topic);
	char response_topic[128];
	snprintf(response_topic, sizeof(response_topic),
		 "v1/devices/me/rpc/response/%u", req_id);
	mqtt_app_mock_deliver_message(response_topic, "{\"time\":1700001234}",
			      strlen("{\"time\":1700001234}"));
	TEST_ASSERT(s_client_rpc_response_count == 2,
		    "second client RPC callback delivered for reused slot");
	TEST_ASSERT(s_client_rpc_success_count == 1,
		    "second client RPC callback reports success");
	TEST_ASSERT(strstr(s_client_rpc_response, "1700001234") != NULL,
		    "client RPC success response payload delivered");
	destroy_test_client(client);
}

static void test_client_side_rpc_max_pending(void)
{
	TEST_START("Client RPC Max Pending");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_client_rpc_state();
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		TEST_ASSERT(tb_rpc_request(client, "getTime", NULL, client_rpc_cb,
					   NULL, 5000) == 0,
			    "client RPC request accepted while slots remain");
	}
	TEST_ASSERT(tb_rpc_request(client, "getTime", NULL, client_rpc_cb, NULL,
			   5000) != 0,
		    "client RPC request fails when pending slots are full");
	for (int i = 0; i < TB_MAX_PENDING_REQUESTS; i++) {
		uint32_t req_id = parse_topic_suffix_id(mock_publishes[i].topic);
		char topic[128];
		snprintf(topic, sizeof(topic),
			 "v1/devices/me/rpc/response/%u", req_id);
		mqtt_app_mock_deliver_message(topic, "{\"time\":1700004321}",
				      strlen("{\"time\":1700004321}"));
	}
	TEST_ASSERT(s_client_rpc_success_count == TB_MAX_PENDING_REQUESTS,
		    "all pending client RPC requests complete successfully");
	TEST_ASSERT(s_client_rpc_timeout_count == 0,
		    "no client RPC timeout while responses arrive before deadline");
	destroy_test_client(client);
}

void run_attributes_tests(void)
{
	test_attributes_send();
	test_attributes_json_shape();
	test_attributes_request();
	test_attributes_request_shared();
	test_attributes_request_shared_timeout_and_reconnect();
	test_attributes_request_reconnect_safety();
	test_attributes_request_timeout_and_slot_reuse();
	test_attributes_request_max_pending();
	test_attributes_subscribe_shared();
	test_attributes_subscribe_shared_per_key();
}

void run_rpc_tests(void)
{
	test_server_side_rpc();
	test_server_side_rpc_malformed_json_and_request_id();
	test_client_side_rpc();
	test_client_side_rpc_reconnect_safety();
	test_client_side_rpc_timeout_and_slot_reuse();
	test_client_side_rpc_max_pending();
}
