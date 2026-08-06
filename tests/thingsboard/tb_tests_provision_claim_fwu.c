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
	TEST_START("Device Provisioning");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_provision_cb_state();
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
	const char *resp =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
	mqtt_app_mock_deliver_message("/provision/response", resp,
				      strlen(resp));
	TEST_ASSERT(s_provision_received == true, "provision callback called");
	TEST_ASSERT(strstr(s_provision_response, "abc123") != NULL,
		    "provision response contains token");
	destroy_test_client(client);
}

static void test_provisioning_request_credentials_type_shapes(void)
{
	TEST_START("Provisioning Request Credential Type Shapes");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_provision_cb_state();
	tb_provision_request_t access_token_req = {
		.device_name = "new_device_token",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
		.credentials_type = "ACCESS_TOKEN",
		.token = "tok_value",
	};
	mock_publish_count = 0;
	TEST_ASSERT(tb_provision_request(client, &access_token_req,
				 provision_cb, NULL,
				 10000) == 0,
		    "access token provisioning request succeeds");
	TEST_ASSERT(mock_publish_count == 1,
		    "access token provisioning request is published");
	cJSON *root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "access token provisioning JSON valid");
	if (root) {
		cJSON *ctype = cJSON_GetObjectItemCaseSensitive(root,
							"credentialsType");
		cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
		TEST_ASSERT(cJSON_IsString(ctype) &&
				strcmp(ctype->valuestring, "ACCESS_TOKEN") == 0,
			    "access token request includes credentialsType");
		TEST_ASSERT(cJSON_IsString(token) &&
				strcmp(token->valuestring, "tok_value") == 0,
			    "access token request includes token field");
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
	TEST_ASSERT(tb_provision_request(client, &basic_req, provision_cb, NULL,
				 10000) == 0,
		    "mqtt basic provisioning request succeeds");
	root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "mqtt basic provisioning JSON valid");
	if (root) {
		cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
		cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
		cJSON *client_id = cJSON_GetObjectItemCaseSensitive(root, "clientId");
		TEST_ASSERT(cJSON_IsString(username) &&
				strcmp(username->valuestring, "u1") == 0,
			    "mqtt basic request includes username");
		TEST_ASSERT(cJSON_IsString(password) &&
				strcmp(password->valuestring, "p1") == 0,
			    "mqtt basic request includes password");
		TEST_ASSERT(cJSON_IsString(client_id) &&
				strcmp(client_id->valuestring, "cid1") == 0,
			    "mqtt basic request includes clientId");
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
	TEST_ASSERT(tb_provision_request(client, &x509_req, provision_cb, NULL,
				 10000) == 0,
		    "x509 provisioning request succeeds");
	root = cJSON_Parse(mock_publishes[0].message);
	TEST_ASSERT(root != NULL, "x509 provisioning JSON valid");
	if (root) {
		cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "hash");
		TEST_ASSERT(cJSON_IsString(hash) &&
				strcmp(hash->valuestring, "abc_hash") == 0,
			    "x509 request includes hash field");
		cJSON_Delete(root);
	}
	destroy_test_client(client);
}

static void test_provisioning_invalid_response_and_publish_failure(void)
{
	TEST_START("Provisioning Invalid Response And Publish Failure");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_provision_cb_state();
	tb_provision_request_t req = {
		.device_name = "new_device",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
	};
	TEST_ASSERT(tb_provision_request(client, &req, provision_cb, NULL,
				 10000) == 0,
		    "provision request succeeds before invalid response");
	const char *invalid_resp = "{invalid_json";
	mqtt_app_mock_deliver_message("/provision/response", invalid_resp,
			      strlen(invalid_resp));
	TEST_ASSERT(s_provision_received == true,
		    "invalid provisioning response still triggers callback");
	TEST_ASSERT(strstr(s_provision_response, "invalid_json") != NULL,
		    "invalid provisioning payload is delivered unchanged");
	mqtt_app_mock_simulate_error_disconnect();
	TEST_ASSERT(tb_provision_request(client, &req, provision_cb, NULL,
				 10000) != 0,
		    "provision request fails when publish transport is down");
	destroy_test_client(client);
}

static void test_provisioning_reconnect_and_reissue(void)
{
	TEST_START("Provisioning Reconnect And Reissue");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	reset_provision_cb_state();
	tb_provision_request_t req = {
		.device_name = "new_device",
		.provision_device_key = "my_provision_key",
		.provision_device_secret = "my_provision_secret",
	};
	TEST_ASSERT(tb_provision_request(client, &req, provision_cb, NULL,
				 10000) == 0,
		    "first provision request succeeds");
	mqtt_app_mock_simulate_remote_disconnect();
	mqtt_app_mock_simulate_connect();
	reset_provision_cb_state();
	TEST_ASSERT(tb_provision_request(client, &req, provision_cb, NULL,
				 10000) == 0,
		    "provision request succeeds after reconnect");
	const char *resp =
		"{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
	mqtt_app_mock_deliver_message("/provision/response", resp, strlen(resp));
	TEST_ASSERT(s_provision_response_count == 1,
		    "provision callback called once after reconnect request");
	TEST_ASSERT(strstr(s_provision_response, "credentialsType") != NULL,
		    "provision response payload delivered after reconnect");
	destroy_test_client(client);
}

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

static void test_claim_device_publish_failure(void)
{
	TEST_START("Device Claiming Publish Failure");
	tb_client_t *client = create_test_client();
	TEST_ASSERT(client != NULL, "client created");
	mqtt_app_mock_simulate_error_disconnect();
	int ret = tb_claim_device(client, "my_secret", 60000);
	TEST_ASSERT(ret != 0,
		    "claim request fails when transport publish fails");
	destroy_test_client(client);
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

void run_provision_claim_tests(void)
{
	test_provisioning();
	test_provisioning_request_credentials_type_shapes();
	test_provisioning_invalid_response_and_publish_failure();
	test_provisioning_reconnect_and_reissue();
	test_claim_device();
	test_claim_device_no_secret();
	test_claim_device_publish_failure();
}

void run_fwu_tests(void)
{
	test_osal_ota_checksum_validation();
	test_osal_ota_checksum_failures();
	test_osal_ota_health_confirmation_api();
	test_osal_ota_state_persistence();
	test_firmware_update_flow();
	test_firmware_update_checksum_mismatch();
	test_firmware_update_invalid_checksum_metadata();
	test_firmware_update_oversized_chunk_stream();
}
