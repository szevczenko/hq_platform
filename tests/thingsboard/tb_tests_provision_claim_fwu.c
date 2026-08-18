#include "tb_test_common.h"

static bool s_provision_received = false;
static char s_provision_response[512] = { 0 };
static int s_provision_response_count = 0;

static bool s_fw_applied_called = false;
static char s_fw_applied_title[128] = { 0 };
static char s_fw_applied_version[128] = { 0 };

static void provision_cb(const char *response_json, void *user_data)
{
	s_provision_received = true;
	s_provision_response_count++;
	memset(s_provision_response, 0, sizeof(s_provision_response));
	if (response_json) {
		strncpy(s_provision_response, response_json,
			sizeof(s_provision_response) - 1);
	}
	(void)user_data;
}

static void reset_provision_cb_state(void)
{
	s_provision_received = false;
	s_provision_response_count = 0;
	memset(s_provision_response, 0, sizeof(s_provision_response));
}

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

static void test_provisioning(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_provision_cb_state();
	tb_provision_request_t req = {
		.device_name = "new_device",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
		.credentials_type = NULL,
	};
	int ret = tb_provision_request(client, &req, provision_cb, NULL, 10000);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL(1, mock_publish_count);
	TEST_ASSERT_EQUAL_STRING("/provision/request", mock_publishes[0].topic);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *dk = cJSON_GetObjectItemCaseSensitive(
			root, "provisionDeviceKey");
		TEST_ASSERT_TRUE(dk != NULL && strcmp(dk->valuestring,
						 "my_provision_key") == 0);
		cJSON *dn =
			cJSON_GetObjectItemCaseSensitive(root, "deviceName");
		TEST_ASSERT_TRUE(dn != NULL &&
				    strcmp(dn->valuestring, "new_device") == 0);
		cJSON_Delete(root);
	}
	const char *resp =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
	mqtt_app_mock_deliver_message("/provision/response", resp,
				      strlen(resp));
	TEST_ASSERT_TRUE(s_provision_received);
	TEST_ASSERT_TRUE(strstr(s_provision_response, "abc123") != NULL);
	destroy_test_client(client);
}

static void test_provisioning_request_credentials_type_shapes(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_provision_cb_state();
	tb_provision_request_t access_token_req = {
		.device_name = "new_device_token",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
		.credentials_type = "ACCESS_TOKEN",
		.token = "tok_value",
	};
	mock_publish_count = 0;
	TEST_ASSERT_TRUE(tb_provision_request(client, &access_token_req,
					 provision_cb, NULL,
					 10000) == 0);
	TEST_ASSERT_EQUAL(1, mock_publish_count);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *ctype = cJSON_GetObjectItemCaseSensitive(root,
								"credentialsType");
		cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
		TEST_ASSERT_TRUE(cJSON_IsString(ctype) &&
				strcmp(ctype->valuestring, "ACCESS_TOKEN") == 0);
		TEST_ASSERT_TRUE(cJSON_IsString(token) &&
				strcmp(token->valuestring, "tok_value") == 0);
		cJSON_Delete(root);
	}
	mock_publish_count = 0;
	tb_provision_request_t basic_req = {
		.device_name = "new_device_basic",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
		.credentials_type = "MQTT_BASIC",
		.username = "u1",
		.password = "p1",
		.client_id = "cid1",
	};
	TEST_ASSERT_TRUE(tb_provision_request(client, &basic_req, provision_cb, NULL,
					 10000) == 0);
	root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
		cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
		cJSON *client_id = cJSON_GetObjectItemCaseSensitive(root, "clientId");
		TEST_ASSERT_TRUE(cJSON_IsString(username) &&
				strcmp(username->valuestring, "u1") == 0);
		TEST_ASSERT_TRUE(cJSON_IsString(password) &&
				strcmp(password->valuestring, "p1") == 0);
		TEST_ASSERT_TRUE(cJSON_IsString(client_id) &&
				strcmp(client_id->valuestring, "cid1") == 0);
		cJSON_Delete(root);
	}
	mock_publish_count = 0;
	tb_provision_request_t x509_req = {
		.device_name = "new_device_x509",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
		.credentials_type = "X509_CERTIFICATE",
		.certificate_hash = "abc_hash",
	};
	TEST_ASSERT_TRUE(tb_provision_request(client, &x509_req, provision_cb, NULL,
					 10000) == 0);
	root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "hash");
		TEST_ASSERT_TRUE(cJSON_IsString(hash) &&
				strcmp(hash->valuestring, "abc_hash") == 0);
		cJSON_Delete(root);
	}
	destroy_test_client(client);
}

static void test_provisioning_invalid_response_and_publish_failure(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_provision_cb_state();
	tb_provision_request_t req = {
		.device_name = "new_device",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
	};
	TEST_ASSERT_TRUE(tb_provision_request(client, &req, provision_cb, NULL,
					 10000) == 0);
	const char *invalid_resp = "{invalid_json";
	mqtt_app_mock_deliver_message("/provision/response", invalid_resp,
				      strlen(invalid_resp));
	TEST_ASSERT_TRUE(s_provision_received);
	TEST_ASSERT_TRUE(strstr(s_provision_response, "invalid_json") != NULL);
	mqtt_app_mock_simulate_error_disconnect();
	TEST_ASSERT_TRUE(tb_provision_request(client, &req, provision_cb, NULL,
					 10000) != 0);
	destroy_test_client(client);
}

static void test_provisioning_reconnect_and_reissue(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	reset_provision_cb_state();
	tb_provision_request_t req = {
		.device_name = "new_device",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
	};
	TEST_ASSERT_TRUE(tb_provision_request(client, &req, provision_cb, NULL,
					 10000) == 0);
	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	reset_provision_cb_state();
	TEST_ASSERT_TRUE(tb_provision_request(client, &req, provision_cb, NULL,
					 10000) == 0);
	const char *resp =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
	mqtt_app_mock_deliver_message("/provision/response", resp, strlen(resp));
	TEST_ASSERT_EQUAL(1, s_provision_response_count);
	TEST_ASSERT_TRUE(strstr(s_provision_response, "credentialsType") != NULL);
	destroy_test_client(client);
}

static tb_client_t *create_test_client_no_mock_reset(void)
{
	tb_client_config_t cfg = {
		.server_url = "mqtt://localhost:1883",
		.access_token = "test_token",
		.device_name = "test_device",
	};
	tb_client_t *client = NULL;

	if (tb_client_init(&client, &cfg) != 0 || client == NULL) {
		return NULL;
	}
	if (tb_client_connect(client) != 0) {
		tb_client_deinit(client);
		return NULL;
	}

	return client;
}

static void test_provisioning_state_cleared_on_client_deinit(void)
{
	tb_provision_request_t req = {
		.device_name = "new_device",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
	};
	const char *resp1 =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"token1\"}";
	const char *resp2 =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"token2\"}";

	tb_client_t *client1 = create_test_client();
	TEST_ASSERT_NOT_NULL(client1);
	if (client1 == NULL) {
		return;
	}

	reset_provision_cb_state();
	mock_subscribe_count = 0;
	TEST_ASSERT_TRUE(tb_provision_request(client1, &req, provision_cb, NULL,
					 10000) == 0);
	TEST_ASSERT_EQUAL(1, mock_subscribe_count);
	mqtt_app_mock_deliver_message("/provision/response", resp1, strlen(resp1));
	TEST_ASSERT_EQUAL(1, s_provision_response_count);
	destroy_test_client(client1);

	tb_client_t *client2 = create_test_client_no_mock_reset();
	TEST_ASSERT_NOT_NULL(client2);
	if (client2 == NULL) {
		return;
	}

	reset_provision_cb_state();
	mock_subscribe_count = 0;
	TEST_ASSERT_TRUE(tb_provision_request(client2, &req, provision_cb, NULL,
					 10000) == 0);
	TEST_ASSERT_EQUAL(1, mock_subscribe_count);
	mqtt_app_mock_deliver_message("/provision/response", resp2, strlen(resp2));
	TEST_ASSERT_EQUAL(1, s_provision_response_count);
	TEST_ASSERT_TRUE(strstr(s_provision_response, "token2") != NULL);
	destroy_test_client(client2);
}

static void test_claim_device(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	int ret = tb_claim_device(client, "my_secret", 60000);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL_STRING("v1/devices/me/claim", mock_publishes[0].topic);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "secretKey");
		TEST_ASSERT_TRUE(sk != NULL &&
				    strcmp(sk->valuestring, "my_secret") == 0);
		cJSON *dur =
			cJSON_GetObjectItemCaseSensitive(root, "durationMs");
		TEST_ASSERT_TRUE(dur != NULL && dur->valueint == 60000);
		cJSON_Delete(root);
	}
	destroy_test_client(client);
}

static void test_claim_device_no_secret(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	int ret = tb_claim_device(client, NULL, 30000);
	TEST_ASSERT_EQUAL(0, ret);
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT_NOT_NULL(root);
	if (root) {
		cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "secretKey");
		TEST_ASSERT_NULL(sk);
		cJSON *dur =
			cJSON_GetObjectItemCaseSensitive(root, "durationMs");
		TEST_ASSERT_TRUE(dur != NULL && dur->valueint == 30000);
		cJSON_Delete(root);
	}
	destroy_test_client(client);
}

static void test_claim_device_publish_failure(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	mqtt_app_mock_simulate_error_disconnect();
	int ret = tb_claim_device(client, "my_secret", 60000);
	TEST_ASSERT_TRUE(ret != 0);
	destroy_test_client(client);
}

static void test_osal_ota_checksum_validation(void)
{
	osal_ota_descriptor_t desc = {
		.title = "hq_platform.bin",
		.version = "1.1.0",
		.checksum = FW_SHA256_ABCDEFGH,
		.checksum_algorithm = "SHA256",
		.total_size = 8,
	};
	osal_ota_abort();
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_begin(&desc));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"ABCD", 4));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"EFGH", 4));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_verify());
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_finish(false));
}

static void test_osal_ota_checksum_failures(void)
{
	osal_ota_descriptor_t desc = {
		.title = "hq_platform.bin",
		.version = "1.1.0",
		.checksum = FW_SHA256_ABCDEFGH,
		.checksum_algorithm = "SHA256",
		.total_size = 8,
	};
	osal_ota_abort();
	desc.checksum = "0000000000000000000000000000000000000000000000000000000000000000";
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_begin(&desc));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"ABCD", 4));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"EFGH", 4));
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_verify());
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_finish(false));
	osal_ota_abort();
	desc.checksum = "not-a-valid-checksum";
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_begin(&desc));
	desc.checksum = FW_SHA256_ABCDEFGH;
	desc.checksum_algorithm = "MD5";
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_begin(&desc));
	desc.checksum_algorithm = "SHA256";
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_begin(&desc));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"ABCD", 4));
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_verify());
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_finish(false));
	osal_ota_abort();
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_begin(&desc));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"ABCDE", 5));
	TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, osal_ota_write((const uint8_t *)"FGHI", 4));
	osal_ota_abort();
}

static void test_osal_ota_health_confirmation_api(void)
{
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_init());
	TEST_ASSERT_FALSE(osal_ota_needs_confirmation());
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_confirm_running_image());
}

static void test_osal_ota_state_persistence(void)
{
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
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_state_save(&saved_state));
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_state_load(&loaded_state));
	TEST_ASSERT_EQUAL_STRING(saved_state.title, loaded_state.title);
	TEST_ASSERT_EQUAL_STRING(saved_state.version, loaded_state.version);
	TEST_ASSERT_EQUAL_STRING(saved_state.checksum, loaded_state.checksum);
	TEST_ASSERT_EQUAL(saved_state.download_state, loaded_state.download_state);
	TEST_ASSERT_EQUAL_STRING(saved_state.last_error, loaded_state.last_error);
	TEST_ASSERT_EQUAL(OSAL_SUCCESS, osal_ota_state_clear());
	TEST_ASSERT_EQUAL(OSAL_ERR_EMPTY_SET, osal_ota_state_load(&loaded_state));
}

static void test_firmware_update_flow(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
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
	TEST_ASSERT_EQUAL(0, ret);
	mock_publish_count = 0;
	ret = tb_firmware_update_request_check(client);
	TEST_ASSERT_EQUAL(0, ret);
	TEST_ASSERT_EQUAL(2, mock_subscribe_count);
	const char *fw_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
			      "\"fw_version\":\"1.1.0\","
			      "\"fw_checksum\":\"" FW_SHA256_ABCDEFGH "\","
			      "\"fw_checksum_algorithm\":\"SHA256\","
			      "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      fw_meta, strlen(fw_meta));
	int req_idx = find_last_publish_with_prefix("v2/fw/request/");
	TEST_ASSERT_TRUE(req_idx >= 0);
	uint32_t req_id = parse_fw_request_id(req_idx);
	TEST_ASSERT_TRUE(req_id > 0);
	char chunk_topic[128];
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/0",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "ABCD", 4);
	int req_idx_chunk1 = find_last_publish_with_prefix("v2/fw/request/");
	TEST_ASSERT_TRUE(req_idx_chunk1 >= 0);
	TEST_ASSERT_TRUE(strstr(mock_publishes[req_idx_chunk1].topic, "/chunk/1") !=
			  NULL);
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/1",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "EFGH", 4);
	TEST_ASSERT_FALSE(tb_firmware_update_is_in_progress());
	TEST_ASSERT_TRUE(s_fw_applied_called);
	TEST_ASSERT_EQUAL_STRING("hq_platform.bin", s_fw_applied_title);
	TEST_ASSERT_EQUAL_STRING("1.1.0", s_fw_applied_version);
	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

static void test_firmware_update_checksum_mismatch(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
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
	TEST_ASSERT_TRUE(tb_firmware_update_init(client, &cfg) == 0);
	mock_publish_count = 0;
	TEST_ASSERT_TRUE(tb_firmware_update_request_check(client) == 0);
	const char *fw_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
			      "\"fw_version\":\"1.1.0\","
			      "\"fw_checksum\":\"0000000000000000000000000000000000000000000000000000000000000000\","
			      "\"fw_checksum_algorithm\":\"SHA256\","
			      "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      fw_meta, strlen(fw_meta));
	uint32_t req_id = parse_fw_request_id(
		find_last_publish_with_prefix("v2/fw/request/"));
	TEST_ASSERT_TRUE(req_id > 0);
	char chunk_topic[128];
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/0",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "ABCD", 4);
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/1",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "EFGH", 4);
	TEST_ASSERT_FALSE(tb_firmware_update_is_in_progress());
	TEST_ASSERT_FALSE(s_fw_applied_called);
	TEST_ASSERT_TRUE(last_fw_telemetry_matches("FAILED",
				      "firmware checksum mismatch"));
	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

static void test_firmware_update_invalid_checksum_metadata(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	s_fw_applied_called = false;
	tb_firmware_update_config_t cfg = {
		.current_title = "hq_platform.bin",
		.current_version = "1.0.0",
		.chunk_size = 4,
		.on_applied = fw_applied_cb,
		.user_data = NULL,
	};
	TEST_ASSERT_TRUE(tb_firmware_update_init(client, &cfg) == 0);
	mock_publish_count = 0;
	TEST_ASSERT_TRUE(tb_firmware_update_request_check(client) == 0);
	const char *bad_checksum_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
				       "\"fw_version\":\"1.1.0\","
				       "\"fw_checksum\":\"not-a-valid-checksum\","
				       "\"fw_checksum_algorithm\":\"SHA256\","
				       "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      bad_checksum_meta,
				      strlen(bad_checksum_meta));
	TEST_ASSERT_TRUE(find_last_publish_with_prefix("v2/fw/request/") < 0);
	TEST_ASSERT_TRUE(last_fw_telemetry_matches("FAILED", "invalid firmware checksum"));
	mock_publish_count = 0;
	TEST_ASSERT_TRUE(tb_firmware_update_request_check(client) == 0);
	const char *bad_algorithm_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
				        "\"fw_version\":\"1.1.0\","
				        "\"fw_checksum\":\"" FW_SHA256_ABCDEFGH "\","
				        "\"fw_checksum_algorithm\":\"MD5\","
				        "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/2",
				      bad_algorithm_meta,
				      strlen(bad_algorithm_meta));
	TEST_ASSERT_TRUE(find_last_publish_with_prefix("v2/fw/request/") < 0);
	TEST_ASSERT_TRUE(last_fw_telemetry_matches(
			    "FAILED", "unsupported firmware checksum algorithm"));
	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

static void test_firmware_update_oversized_chunk_stream(void)
{
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	s_fw_applied_called = false;
	tb_firmware_update_config_t cfg = {
		.current_title = "hq_platform.bin",
		.current_version = "1.0.0",
		.chunk_size = 4,
		.on_applied = fw_applied_cb,
		.user_data = NULL,
	};
	TEST_ASSERT_TRUE(tb_firmware_update_init(client, &cfg) == 0);
	mock_publish_count = 0;
	TEST_ASSERT_TRUE(tb_firmware_update_request_check(client) == 0);
	const char *fw_meta = "{\"shared\":{\"fw_title\":\"hq_platform.bin\","
			      "\"fw_version\":\"1.1.0\","
			      "\"fw_checksum\":\"" FW_SHA256_ABCDEFGH "\","
			      "\"fw_checksum_algorithm\":\"SHA256\","
			      "\"fw_size\":8}}";
	mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/1",
				      fw_meta, strlen(fw_meta));
	uint32_t req_id = parse_fw_request_id(
		find_last_publish_with_prefix("v2/fw/request/"));
	TEST_ASSERT_TRUE(req_id > 0);
	char chunk_topic[128];
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/0",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "ABCDE", 5);
	snprintf(chunk_topic, sizeof(chunk_topic), "v2/fw/response/%u/chunk/1",
		 req_id);
	mqtt_app_mock_deliver_message(chunk_topic, "FGHI", 4);
	TEST_ASSERT_FALSE(tb_firmware_update_is_in_progress());
	TEST_ASSERT_FALSE(s_fw_applied_called);
	TEST_ASSERT_TRUE(last_fw_telemetry_matches("FAILED", "ota write failed"));
	tb_firmware_update_deinit(client);
	destroy_test_client(client);
}

void run_provision_claim_tests(void)
{
	RUN_TEST(test_provisioning);
	RUN_TEST(test_provisioning_request_credentials_type_shapes);
	RUN_TEST(test_provisioning_invalid_response_and_publish_failure);
	RUN_TEST(test_provisioning_reconnect_and_reissue);
	RUN_TEST(test_provisioning_state_cleared_on_client_deinit);
	RUN_TEST(test_claim_device);
	RUN_TEST(test_claim_device_no_secret);
	RUN_TEST(test_claim_device_publish_failure);
}

void run_fwu_tests(void)
{
	RUN_TEST(test_osal_ota_checksum_validation);
	RUN_TEST(test_osal_ota_checksum_failures);
	RUN_TEST(test_osal_ota_health_confirmation_api);
	RUN_TEST(test_osal_ota_state_persistence);
	RUN_TEST(test_firmware_update_flow);
	RUN_TEST(test_firmware_update_checksum_mismatch);
	RUN_TEST(test_firmware_update_invalid_checksum_metadata);
	RUN_TEST(test_firmware_update_oversized_chunk_stream);
}