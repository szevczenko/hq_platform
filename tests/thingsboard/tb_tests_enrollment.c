#include "tb_test_common.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "tb_enrollment.h"

typedef struct {
	const char *response;
	long delay_ms;
} scheduled_response_t;

typedef struct {
	int complete_count;
	int persist_count;
	tb_enrollment_status_t status;
	bool persist_success;
	char credentials_type[TB_ENROLLMENT_CREDENTIAL_TYPE_MAX_LEN + 1U];
	char credentials_value[TB_ENROLLMENT_CREDENTIAL_VALUE_MAX_LEN + 1U];
} enrollment_test_state_t;

static const tb_enrollment_config_t s_config = {
	.device_name = "enrollment_test_device",
	.provision_device_key = "enrollment_test_key",
	.provision_device_secret = "enrollment_secret_must_not_be_logged",
};

static void *deliver_scheduled_response(void *user_data)
{
	scheduled_response_t *scheduled = (scheduled_response_t *)user_data;
	struct timespec delay = {
		.tv_sec = scheduled->delay_ms / 1000,
		.tv_nsec = (scheduled->delay_ms % 1000) * 1000000L,
	};
	(void)nanosleep(&delay, NULL);
	mqtt_app_mock_deliver_message("/provision/response", scheduled->response,
				      strlen(scheduled->response));
	return NULL;
}

static bool persist_credentials(const tb_enrollment_credentials_t *credentials,
				void *user_data)
{
	enrollment_test_state_t *state = (enrollment_test_state_t *)user_data;
	state->persist_count++;
	(void)strncpy(state->credentials_type, credentials->credentials_type,
		      sizeof(state->credentials_type) - 1U);
	(void)strncpy(state->credentials_value, credentials->credentials_value,
		      sizeof(state->credentials_value) - 1U);
	return state->persist_success;
}

static void enrollment_complete(tb_enrollment_status_t status,
				const tb_enrollment_credentials_t *credentials,
				void *user_data)
{
	enrollment_test_state_t *state = (enrollment_test_state_t *)user_data;
	state->complete_count++;
	state->status = status;
	if (credentials != NULL) {
		(void)strncpy(state->credentials_type, credentials->credentials_type,
			      sizeof(state->credentials_type) - 1U);
		(void)strncpy(state->credentials_value, credentials->credentials_value,
			      sizeof(state->credentials_value) - 1U);
	}
}

static bool capture_stdout_start(FILE **capture, int *saved_stdout)
{
	*capture = tmpfile();
	if (*capture == NULL) {
		return false;
	}
	(void)fflush(stdout);
	*saved_stdout = dup(STDOUT_FILENO);
	if (*saved_stdout < 0 || dup2(fileno(*capture), STDOUT_FILENO) < 0) {
		if (*saved_stdout >= 0) {
			(void)close(*saved_stdout);
		}
		(void)fclose(*capture);
		return false;
	}
	return true;
}

static bool capture_stdout_stop(FILE *capture, int saved_stdout)
{
	char output[512] = { 0 };
	bool secret_logged;
	(void)fflush(stdout);
	(void)dup2(saved_stdout, STDOUT_FILENO);
	(void)close(saved_stdout);
	rewind(capture);
	(void)fread(output, 1U, sizeof(output) - 1U, capture);
	secret_logged = strstr(output, s_config.provision_device_secret) != NULL;
	(void)fclose(capture);
	return !secret_logged;
}

static void test_enrollment_success_returns_and_persists_credentials(void)
{
	static const char response[] =
		"{\"status\":\"SUCCESS\",\"credentialsType\":\"ACCESS_TOKEN\","
		"\"credentialsValue\":\"enrolled_access_token\"}";
	scheduled_response_t scheduled = { .response = response, .delay_ms = 20 };
	enrollment_test_state_t state = { .persist_success = true };
	FILE *capture = NULL;
	int saved_stdout = -1;
	pthread_t delivery_thread;
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	TEST_ASSERT_TRUE(capture_stdout_start(&capture, &saved_stdout));
	TEST_ASSERT_EQUAL(0, pthread_create(&delivery_thread, NULL,
						 deliver_scheduled_response, &scheduled));
	tb_enrollment_status_t status = tb_enrollment_enroll(
		client, &s_config, 500U, persist_credentials, &state,
		enrollment_complete, &state);
	TEST_ASSERT_EQUAL(0, pthread_join(delivery_thread, NULL));
	TEST_ASSERT_TRUE(capture_stdout_stop(capture, saved_stdout));
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_OK, status);
	TEST_ASSERT_EQUAL(1, state.persist_count);
	TEST_ASSERT_EQUAL(1, state.complete_count);
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_OK, state.status);
	TEST_ASSERT_EQUAL_STRING("ACCESS_TOKEN", state.credentials_type);
	TEST_ASSERT_EQUAL_STRING("enrolled_access_token", state.credentials_value);
	destroy_test_client(client);
}

static void test_enrollment_rejected_response(void)
{
	static const char response[] = "{\"status\":\"FAILURE\"}";
	scheduled_response_t scheduled = { .response = response, .delay_ms = 20 };
	enrollment_test_state_t state = { .persist_success = true };
	pthread_t delivery_thread;
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	TEST_ASSERT_EQUAL(0, pthread_create(&delivery_thread, NULL,
						 deliver_scheduled_response, &scheduled));
	tb_enrollment_status_t status = tb_enrollment_enroll(
		client, &s_config, 500U, persist_credentials, &state,
		enrollment_complete, &state);
	TEST_ASSERT_EQUAL(0, pthread_join(delivery_thread, NULL));
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_ERR_RESPONSE, status);
	TEST_ASSERT_EQUAL(0, state.persist_count);
	TEST_ASSERT_EQUAL(1, state.complete_count);
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_ERR_RESPONSE, state.status);
	destroy_test_client(client);
}

static void test_enrollment_persistence_failure(void)
{
	static const char response[] =
		"{\"status\":\"SUCCESS\",\"credentialsType\":\"ACCESS_TOKEN\","
		"\"credentialsValue\":\"unenstored_access_token\"}";
	scheduled_response_t scheduled = { .response = response, .delay_ms = 20 };
	enrollment_test_state_t state = { .persist_success = false };
	pthread_t delivery_thread;
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	TEST_ASSERT_EQUAL(0, pthread_create(&delivery_thread, NULL,
						 deliver_scheduled_response, &scheduled));
	tb_enrollment_status_t status = tb_enrollment_enroll(
		client, &s_config, 500U, persist_credentials, &state,
		enrollment_complete, &state);
	TEST_ASSERT_EQUAL(0, pthread_join(delivery_thread, NULL));
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_ERR_PERSIST, status);
	TEST_ASSERT_EQUAL(1, state.persist_count);
	TEST_ASSERT_EQUAL(1, state.complete_count);
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_ERR_PERSIST, state.status);
	destroy_test_client(client);
}

static void test_enrollment_timeout_cancels_late_response(void)
{
	static const char response[] =
		"{\"status\":\"SUCCESS\",\"credentialsType\":\"ACCESS_TOKEN\","
		"\"credentialsValue\":\"late_access_token\"}";
	enrollment_test_state_t state = { .persist_success = true };
	tb_client_t *client = create_test_client();
	TEST_ASSERT_NOT_NULL(client);
	tb_enrollment_status_t status = tb_enrollment_enroll(
		client, &s_config, 20U, persist_credentials, &state,
		enrollment_complete, &state);
	TEST_ASSERT_EQUAL(TB_ENROLLMENT_ERR_RESPONSE, status);
	TEST_ASSERT_EQUAL(0, state.persist_count);
	TEST_ASSERT_EQUAL(1, state.complete_count);
	mqtt_app_mock_deliver_message("/provision/response", response,
				      strlen(response));
	TEST_ASSERT_EQUAL(1, state.complete_count);
	TEST_ASSERT_EQUAL(0, state.persist_count);
	destroy_test_client(client);
}

void run_enrollment_tests(void)
{
	RUN_TEST(test_enrollment_success_returns_and_persists_credentials);
	RUN_TEST(test_enrollment_rejected_response);
	RUN_TEST(test_enrollment_persistence_failure);
	RUN_TEST(test_enrollment_timeout_cancels_late_response);
}