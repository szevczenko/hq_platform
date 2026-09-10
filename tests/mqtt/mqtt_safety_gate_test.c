/*
 * MQTT lifecycle safety observer + configuration gate tests (TASK-121).
 *
 * POSIX-only.  Exercises the real mqtt_app transport against an in-process
 * mongoose MQTT broker (separate mg_mgr + poll thread) so no external broker
 * is required:
 *
 *   1. observer fan-out: the platform safety observer (registered with
 *      mqtt_app_set_safety_callbacks()) is invoked IN ADDITION to the
 *      product/client callbacks for connect, disconnect and connect-failure
 *      events; a later client-only re-registration cannot replace or erase
 *      the safety observer;
 *   2. rejected config update does not reconnect: a validation-callback
 *      rejection makes the apply-config handler leave the transport
 *      disconnected, notify the failure observer and never open a new
 *      broker connection;
 *   3. the gate refuses skip-verify / SSL-off / raw-cert mutations (the
 *      registered policy callback sees the exact values the reconnect would
 *      use) while allowing compliant updates to reconnect;
 *   4. the gate fails closed: without a registered validation callback the
 *      generic apply-config path refuses to reconnect, so an unvalidated
 *      mutated configuration can never reach the transport.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_app.h"
#include "mqtt_config.h"
#include "mongoose.h"
#include "mongoose_process.h"
#include "osal_bin_sem.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "osal_task.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#error "mqtt_safety_gate_test.c targets POSIX only"
#endif

#define TEST_IMAGE_PATH   "/tmp/mqtt_safety_gate_test.img"
#define TEST_MOUNT_POINT  "/"
#define TEST_WAIT_STEP_MS 25U
#define TEST_WAIT_TOTAL_MS 10000U

/* ------------------------------------------------------------------------- */
/* Observer counters                                                          */
/* ------------------------------------------------------------------------- */

static int s_safety_connect;
static int s_client_connect;
static int s_safety_disconnect_explicit;
static int s_safety_disconnect_remote;
static int s_client_disconnect_explicit;
static int s_client_disconnect_remote;
static int s_safety_failure_config_rejected;
static int s_safety_failure_other;
static int s_client_failure_config_rejected;
static int s_client_failure_other;

/* ------------------------------------------------------------------------- */
/* Validation-gate state (records the snapshot the platform hands to the      */
/* registered policy callback)                                                */
/* ------------------------------------------------------------------------- */

static int s_validation_calls;
static bool s_last_snapshot_valid;
static mqtt_config_snapshot_t s_last_snapshot;
static bool s_policy_reject_all;
static bool s_policy_verified_tls;

/* ------------------------------------------------------------------------- */
/* In-process broker state                                                    */
/* ------------------------------------------------------------------------- */

static struct mg_mgr s_broker_mgr;
static struct mg_connection *s_broker_listener;
static struct mg_connection *s_broker_client;
static int s_broker_port;
static int s_broker_connects;
static bool s_broker_stop;
static bool s_broker_close_requested;
static pthread_t s_broker_thread;
static osal_bin_sem_id_t s_broker_ready;

/* ------------------------------------------------------------------------- */
/* Observer callbacks (all run on the Mongoose poll thread)                   */
/* ------------------------------------------------------------------------- */

static void on_safety_connect(void)
{
	s_safety_connect++;
}

static void on_safety_disconnect(mqtt_disconnect_reason_t reason)
{
	if (reason == MQTT_DISCONNECT_REASON_EXPLICIT)
		s_safety_disconnect_explicit++;
	else
		s_safety_disconnect_remote++;
}

static void on_safety_failure(mqtt_connect_failure_reason_t reason)
{
	if (reason == MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED)
		s_safety_failure_config_rejected++;
	else
		s_safety_failure_other++;
}

static void on_client_connect(void)
{
	s_client_connect++;
}

static void on_client_disconnect(mqtt_disconnect_reason_t reason)
{
	if (reason == MQTT_DISCONNECT_REASON_EXPLICIT)
		s_client_disconnect_explicit++;
	else
		s_client_disconnect_remote++;
}

static void on_client_failure(mqtt_connect_failure_reason_t reason)
{
	if (reason == MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED)
		s_client_failure_config_rejected++;
	else
		s_client_failure_other++;
}

/* ------------------------------------------------------------------------- */
/* Policy callback.  Neutral stand-in for the product verified-TLS policy:    */
/* it only inspects the snapshot values the platform guarantees to provide.   */
/* ------------------------------------------------------------------------- */

static bool on_validate_config(const mqtt_config_snapshot_t *candidate)
{
	s_last_snapshot = *candidate;
	s_last_snapshot_valid = true;
	s_validation_calls++;

	if (!s_policy_reject_all && s_policy_verified_tls) {
		if (!candidate->ssl_enabled || candidate->skip_verify)
			return false;
		if (candidate->cert_source == MQTT_CERT_SOURCE_RAW ||
		    candidate->client_cert_source == MQTT_CERT_SOURCE_RAW ||
		    candidate->client_key_source == MQTT_CERT_SOURCE_RAW)
			return false;
	}
	if (s_policy_reject_all)
		return false;

	return true;
}

/* ------------------------------------------------------------------------- */
/* Broker (separate mg_mgr + poll thread inside the test binary)              */
/* ------------------------------------------------------------------------- */

static void broker_ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	switch (ev) {
	case MG_EV_ACCEPT:
		s_broker_connects++;
		s_broker_client = nc;
		break;

	case MG_EV_MQTT_CMD: {
		struct mg_mqtt_message *mm = (struct mg_mqtt_message *)ev_data;
		if (mm->cmd == MQTT_CMD_CONNECT) {
			/* MQTT 3.1.1 CONNACK: session present 0, return code 0 */
			(void)mg_send(nc, "\x20\x02\x00\x00", 4);
		}
		break;
	}

	default:
		break;
	}
}

static void *broker_thread_fn(void *arg)
{
	uint32_t waited = 0;

	(void)arg;
	memset(&s_broker_mgr, 0, sizeof(s_broker_mgr));
	mg_mgr_init(&s_broker_mgr);

	s_broker_listener = mg_mqtt_listen(&s_broker_mgr, "tcp://127.0.0.1:0",
					   broker_ev_handler, NULL);
	if (s_broker_listener == NULL) {
		(void)osal_bin_sem_give(s_broker_ready);
		return NULL;
	}

	s_broker_port = mg_ntohs(s_broker_listener->loc.port);
	(void)osal_bin_sem_give(s_broker_ready);

	while (!s_broker_stop) {
		if (s_broker_close_requested && s_broker_client != NULL) {
			s_broker_client->is_closing = 1;
			s_broker_client = NULL;
			s_broker_close_requested = false;
		}
		mg_mgr_poll(&s_broker_mgr, 20);
		if (waited < TEST_WAIT_TOTAL_MS)
			waited += 20;
	}

	mg_mgr_free(&s_broker_mgr);
	return NULL;
}

static bool broker_start(void)
{
	s_broker_connects = 0;
	s_broker_client = NULL;
	s_broker_stop = false;
	s_broker_close_requested = false;
	s_broker_port = 0;

	if (osal_bin_sem_create(&s_broker_ready, "broker_ready",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		return false;

	if (pthread_create(&s_broker_thread, NULL, broker_thread_fn,
			   NULL) != 0) {
		(void)osal_bin_sem_delete(s_broker_ready);
		s_broker_ready = NULL;
		return false;
	}

	if (osal_bin_sem_timed_wait(s_broker_ready, 5000) != OSAL_SUCCESS ||
	    s_broker_port == 0) {
		return false;
	}

	return true;
}

static void broker_stop(void)
{
	s_broker_stop = true;
	(void)pthread_join(s_broker_thread, NULL);
	s_broker_listener = NULL;
	(void)osal_bin_sem_delete(s_broker_ready);
	s_broker_ready = NULL;
}

/* ------------------------------------------------------------------------- */
/* Test filesystem                                                            */
/* ------------------------------------------------------------------------- */

static bool test_fs_setup(void)
{
	(void)osal_unmount(TEST_MOUNT_POINT);
	(void)osal_rmfs(TEST_IMAGE_PATH);

	if (osal_initfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U) !=
	    OSAL_SUCCESS)
		return false;

	if (osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U) !=
	    OSAL_SUCCESS)
		return false;

	if (osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT) != OSAL_SUCCESS)
		return false;

	return true;
}

static void test_fs_teardown(void)
{
	(void)osal_unmount(TEST_MOUNT_POINT);
	(void)osal_rmfs(TEST_IMAGE_PATH);
}

static bool write_test_file(const char *path, const char *content)
{
	osal_file_id_t fd;
	size_t len = strlen(content);
	int32_t n;

	fd = osal_open_create(
		path, OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
		OSAL_WRITE_ONLY);
	if (fd < 0)
		return false;

	n = osal_write(fd, content, len);
	(void)osal_close(fd);
	return n >= 0 && (size_t)n == len;
}

/* ------------------------------------------------------------------------- */
/* Wait helpers                                                               */
/* ------------------------------------------------------------------------- */

static bool wait_for_connected(bool expected)
{
	uint32_t waited = 0;

	while (waited < TEST_WAIT_TOTAL_MS) {
		if (mqtt_app_is_connected() == expected)
			return true;
		(void)osal_task_delay_ms(TEST_WAIT_STEP_MS);
		waited += TEST_WAIT_STEP_MS;
	}
	return false;
}

static bool wait_for_safety_failure_config_rejected(int expected)
{
	uint32_t waited = 0;

	while (waited < TEST_WAIT_TOTAL_MS) {
		if (s_safety_failure_config_rejected >= expected)
			return true;
		(void)osal_task_delay_ms(TEST_WAIT_STEP_MS);
		waited += TEST_WAIT_STEP_MS;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* Fixture                                                                     */
/* ------------------------------------------------------------------------- */

void setUp(void)
{
	s_safety_connect = 0;
	s_client_connect = 0;
	s_safety_disconnect_explicit = 0;
	s_safety_disconnect_remote = 0;
	s_client_disconnect_explicit = 0;
	s_client_disconnect_remote = 0;
	s_safety_failure_config_rejected = 0;
	s_safety_failure_other = 0;
	s_client_failure_config_rejected = 0;
	s_client_failure_other = 0;

	s_validation_calls = 0;
	s_last_snapshot_valid = false;
	s_policy_reject_all = false;
	s_policy_verified_tls = false;

	if (!test_fs_setup()) {
		TEST_FAIL_MESSAGE("safety gate test filesystem setup failed");
		return;
	}

	if (!broker_start()) {
		TEST_FAIL_MESSAGE("in-process MQTT broker failed to start");
		return;
	}

	MongooseProcess_Init();
}

void tearDown(void)
{
	mqtt_app_deinit();
	MongooseProcess_Deinit();
	broker_stop();
	test_fs_teardown();
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void reset_observer_registry(void)
{
	mqtt_app_set_safety_callbacks(NULL, NULL, NULL);
	mqtt_app_set_connect_callback(NULL);
	mqtt_app_set_disconnect_callback(NULL);
	mqtt_app_set_connect_failure_callback(NULL);
	mqtt_app_set_config_validation_callback(NULL);
}

static void configure_broker_address(void)
{
	char address[64];

	(void)snprintf(address, sizeof(address), "mqtt://127.0.0.1:%d",
		       s_broker_port);
	mqtt_config_init();
	TEST_ASSERT_TRUE_MESSAGE(
		mqtt_config_set_string(address, MQTT_CONFIG_VALUE_ADDRESS),
		"broker address configured");
	TEST_ASSERT_TRUE_MESSAGE(
		mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL),
		"SSL flag configured on (policy snapshot input)");
	TEST_ASSERT_TRUE_MESSAGE(
		mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY),
		"skip-verify disabled");
}

static void test_observer_fan_out(void)
{
	configure_broker_address();
	reset_observer_registry();

	/* Register the platform safety observer, then register it AGAIN with
	 * the same callbacks: registration must be idempotent (a repeated call
	 * yields exactly one observer set, not two). */
	mqtt_app_set_safety_callbacks(on_safety_connect, on_safety_disconnect,
				      on_safety_failure);
	mqtt_app_set_safety_callbacks(on_safety_connect, on_safety_disconnect,
				      on_safety_failure);
	mqtt_app_set_connect_callback(on_client_connect);
	mqtt_app_set_disconnect_callback(on_client_disconnect);
	mqtt_app_set_connect_failure_callback(on_client_failure);

	mqtt_app_init();
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "MQTT connected to in-process broker");

	/* Connect fan-out: both the safety observer and the client callback
	 * fire; the safety observer fired exactly once despite the double
	 * registration. */
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_safety_connect,
				      "safety connect observer fired once");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_client_connect,
				      "client connect callback still fires");
	TEST_ASSERT_EQUAL_INT(1, s_broker_connects);

	/* Disconnect fan-out: broker drops the link -> REMOTE_CLOSE is
	 * delivered to BOTH observer sets. */
	s_broker_close_requested = true;
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(false),
				 "transport dropped after broker close");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_safety_disconnect_remote,
				      "safety disconnect observer fired");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_client_disconnect_remote,
				      "client disconnect callback still fires");

	/* A later client-only re-registration (ThingsBoard-style replacement)
	 * must NOT erase the platform safety observer.  Drive the failure
	 * fan-out through a rejected apply-config. */
	mqtt_app_set_connect_callback(NULL);
	mqtt_app_set_disconnect_callback(NULL);
	mqtt_app_set_connect_failure_callback(on_client_failure);
	mqtt_app_set_config_validation_callback(on_validate_config);
	s_policy_reject_all = true;

	/* While disconnected, a mutated config save must still trigger the
	 * gate; the rejection fans out to both the safety failure observer
	 * and the (replaced) client failure callback. */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(1),
				 "rejected apply-config reached safety observer");

	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_safety_failure_config_rejected,
				      "safety failure observer fired");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_client_failure_config_rejected,
				     "client failure callback still fires");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_safety_failure_other,
				      "no unexpected safety failures");
	TEST_ASSERT_FALSE(mqtt_app_is_connected());
}

static void test_rejected_config_update_does_not_reconnect(void)
{
	configure_broker_address();
	reset_observer_registry();

	mqtt_app_set_safety_callbacks(on_safety_connect, on_safety_disconnect,
				      on_safety_failure);
	mqtt_app_set_connect_callback(on_client_connect);
	mqtt_app_set_disconnect_callback(on_client_disconnect);
	mqtt_app_set_connect_failure_callback(on_client_failure);

	/* Designated owner installs a rejecting gate (policy-neutral stand-in:
	 * reject every apply-config). */
	mqtt_app_set_config_validation_callback(on_validate_config);
	s_policy_reject_all = true;

	mqtt_app_init();
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "MQTT connected to in-process broker");

	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_broker_connects,
				      "one broker connection before mutation");

	/* Mutate the shared config behind the owner's back and save: the
	 * generic mqtt_config_save() -> APPLY_CONFIG path must be gated. */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_TRUE(mqtt_config_save());

	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(1),
				 "apply-config rejection observed");
	TEST_ASSERT_FALSE_MESSAGE(mqtt_app_is_connected(),
				  "transport left disconnected after rejection");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_safety_disconnect_explicit,
				      "safety disconnect observer fired (EXPLICIT)");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_client_disconnect_explicit,
				      "client disconnect callback fired (EXPLICIT)");

	/* No reconnect may follow a rejected configuration: give the transport
	 * time to (incorrectly) retry, then verify the broker saw nothing. */
	(void)osal_task_delay_ms(2000U);
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_broker_connects,
				      "no reconnect after rejected config update");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_safety_failure_other,
				      "no transport-level connect attempt");
}

static void test_gate_blocks_unverified_mutations(void)
{
	configure_broker_address();
	reset_observer_registry();

	mqtt_app_set_safety_callbacks(on_safety_connect, on_safety_disconnect,
				      on_safety_failure);
	mqtt_app_set_connect_failure_callback(on_client_failure);

	/* Verified-TLS-like policy stand-in: SSL must be on, skip-verify off,
	 * no raw certificate material. */
	mqtt_app_set_config_validation_callback(on_validate_config);
	s_policy_verified_tls = true;

	mqtt_app_init();
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "MQTT connected to in-process broker");
	TEST_ASSERT_EQUAL_INT(1, s_broker_connects);

	/* --- skip-verify mutation refused --- */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(1),
				 "skip-verify mutation rejected");
	TEST_ASSERT_TRUE_MESSAGE(s_last_snapshot_valid,
				 "gate received a snapshot");
	TEST_ASSERT_TRUE_MESSAGE(s_last_snapshot.skip_verify,
				 "gate saw skip_verify=true");
	TEST_ASSERT_FALSE(mqtt_app_is_connected());
	TEST_ASSERT_EQUAL_INT(1, s_broker_connects);

	/* Recovery: compliant state reconnects through the same gate. */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "compliant config reconnects after rejection");
	TEST_ASSERT_EQUAL_INT(2, s_broker_connects);

	/* --- SSL-off mutation refused --- */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SSL));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(2),
				 "SSL-off mutation rejected");
	TEST_ASSERT_TRUE_MESSAGE(!s_last_snapshot.ssl_enabled,
				 "gate saw ssl_enabled=false");
	TEST_ASSERT_FALSE(mqtt_app_is_connected());
	TEST_ASSERT_EQUAL_INT(2, s_broker_connects);

	/* Recovery. */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "compliant config reconnects after SSL refusal");
	TEST_ASSERT_EQUAL_INT(3, s_broker_connects);

	/* --- raw CA cert mutation refused --- */
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(
		MQTT_CERT_SOURCE_RAW,
		"-----BEGIN CERTIFICATE-----\nRAWCERT\n"
		"-----END CERTIFICATE-----\n",
		MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(3),
				 "raw CA mutation rejected");
	TEST_ASSERT_TRUE_MESSAGE(
		s_last_snapshot.cert_source == MQTT_CERT_SOURCE_RAW,
		"gate saw raw CA source");
	TEST_ASSERT_FALSE(mqtt_app_is_connected());
	TEST_ASSERT_EQUAL_INT(3, s_broker_connects);

	/* Recovery. */
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_NONE, NULL,
						     MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "compliant config reconnects after raw-cert refusal");
	TEST_ASSERT_EQUAL_INT(4, s_broker_connects);

	/* --- raw client-cert mutation refused --- */
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(
		MQTT_CERT_SOURCE_RAW,
		"-----BEGIN CERTIFICATE-----\nRAWCLIENT\n"
		"-----END CERTIFICATE-----\n",
		MQTT_CONFIG_VALUE_CLIENT_CERT));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(4),
				 "raw client-cert mutation rejected");
	TEST_ASSERT_TRUE_MESSAGE(
		s_last_snapshot.client_cert_source == MQTT_CERT_SOURCE_RAW,
		"gate saw raw client-cert source");
	TEST_ASSERT_FALSE(mqtt_app_is_connected());
	TEST_ASSERT_EQUAL_INT(4, s_broker_connects);

	/* --- file-path CA cert is accepted by the policy --- */
	TEST_ASSERT_TRUE_MESSAGE(write_test_file("ca.pem",
						 "-----BEGIN CERTIFICATE-----\n"
						 "FILEPATHCA\n"
						 "-----END CERTIFICATE-----\n"),
				 "CA file written to test FS");
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_NONE, NULL,
						     MQTT_CONFIG_VALUE_CLIENT_CERT));
	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(
		MQTT_CERT_SOURCE_FILE_PATH, "ca.pem", MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL));
	TEST_ASSERT_TRUE(mqtt_config_save());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "file-path CA accepted by policy");
	TEST_ASSERT_TRUE_MESSAGE(
		s_last_snapshot.cert_source == MQTT_CERT_SOURCE_FILE_PATH,
		"gate saw file-path CA source");
	TEST_ASSERT_EQUAL_INT(5, s_broker_connects);
	(void)osal_remove("ca.pem");
}

static void test_apply_config_fails_closed_without_validator(void)
{
	configure_broker_address();
	reset_observer_registry();

	mqtt_app_set_safety_callbacks(on_safety_connect, on_safety_disconnect,
				      on_safety_failure);

	mqtt_app_init();
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "MQTT connected to in-process broker");
	TEST_ASSERT_EQUAL_INT(1, s_broker_connects);

	/* No designated owner registered a validation callback: the gate must
	 * fail closed.  The generic apply-config path may NOT reconnect from
	 * an unvalidated mutated configuration. */
	TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SKIP_VERIFY));
	TEST_ASSERT_TRUE(mqtt_config_save());

	TEST_ASSERT_TRUE_MESSAGE(wait_for_safety_failure_config_rejected(1),
				 "apply-config rejected without validator");
	TEST_ASSERT_FALSE_MESSAGE(mqtt_app_is_connected(),
				  "transport left disconnected (fail closed)");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_safety_disconnect_explicit,
				      "safety disconnect observer fired (EXPLICIT)");

	/* No reconnect may follow the rejected apply-config: give the
	 * transport time to (incorrectly) retry, then verify the broker saw
	 * nothing new. */
	(void)osal_task_delay_ms(2000U);
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_broker_connects,
				      "no reconnect without registered owner");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_safety_failure_other,
				      "no transport-level connect attempt");
}

static void mqtt_safety_gate_tests_run(void)
{
	RUN_TEST(test_observer_fan_out);
	RUN_TEST(test_rejected_config_update_does_not_reconnect);
	RUN_TEST(test_gate_blocks_unverified_mutations);
	RUN_TEST(test_apply_config_fails_closed_without_validator);
}

int main(void)
{
	UNITY_BEGIN();
	mqtt_safety_gate_tests_run();
	return UNITY_END();
}