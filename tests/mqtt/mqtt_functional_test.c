#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqtt_app.h"
#include "mqtt_config.h"
#include "mongoose_process.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "osal_task.h"

#ifdef ESP_PLATFORM
#define TEST_IMAGE_PATH "flash_test"
#define TEST_MOUNT_POINT "/littlefs"
#else
#define TEST_IMAGE_PATH "/tmp/mqtt_functional_test.img"
#define TEST_MOUNT_POINT "/"
#endif

#define TEST_TOPIC "hq/test/functional"
#define TEST_TIMEOUT_MS 3000U
#define TEST_WAIT_STEP_MS 50U
#define TEST_WAIT_TOTAL_MS 10000U

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static volatile bool g_msg_received = false;
static volatile int g_msg_count = 0;
static char g_last_topic[256] = { 0 };
static char g_last_payload[512] = { 0 };

#define TEST_ASSERT(condition, message)                 \
	do {                                            \
		tests_run++;                            \
		if (condition) {                        \
			tests_passed++;                 \
			printf("[PASS] %s\n", message); \
		} else {                                \
			tests_failed++;                 \
			printf("[FAIL] %s\n", message); \
		}                                       \
	} while (0)

#define TEST_START(name)                                                  \
	printf("\n==================================================\n"); \
	printf("TEST: %s\n", name);                                       \
	printf("==================================================\n")

#define TEST_END() \
	printf("--------------------------------------------------\n")

static bool setup_fs(void)
{
	(void)osal_unmount(TEST_MOUNT_POINT);
	(void)osal_rmfs(TEST_IMAGE_PATH);

	if (osal_initfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U) !=
	    OSAL_SUCCESS) {
		return false;
	}
	if (osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U) !=
	    OSAL_SUCCESS) {
		return false;
	}
	if (osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT) != OSAL_SUCCESS) {
		return false;
	}

	return true;
}

static void teardown_fs(void)
{
	(void)osal_unmount(TEST_MOUNT_POINT);
	(void)osal_rmfs(TEST_IMAGE_PATH);
}

static bool load_host_file_to_buffer(const char *path, char *buffer,
				     size_t max_len)
{
	FILE *fp;
	size_t read_len;

	if (!path || !buffer || max_len < 2) {
		return false;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		return false;
	}

	read_len = fread(buffer, 1, max_len - 1, fp);
	(void)fclose(fp);
	if (read_len == 0) {
		return false;
	}

	buffer[read_len] = '\0';
	return true;
}

static bool load_cert_file(const char *name, char *buffer, size_t max_len)
{
	char path[256] = { 0 };
	const char *prefixes[] = { "cert/", "../cert/", "../../cert/" };

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		(void)snprintf(path, sizeof(path), "%s%s", prefixes[i], name);
		if (load_host_file_to_buffer(path, buffer, max_len)) {
			return true;
		}
	}

	return false;
}

static bool wait_for_connected(bool expected)
{
	uint32_t waited = 0;
	while (waited < TEST_WAIT_TOTAL_MS) {
		if (mqtt_app_is_connected() == expected) {
			return true;
		}
		(void)osal_task_delay_ms(TEST_WAIT_STEP_MS);
		waited += TEST_WAIT_STEP_MS;
	}
	return false;
}

static bool wait_for_message(void)
{
	uint32_t waited = 0;
	while (waited < TEST_WAIT_TOTAL_MS) {
		if (g_msg_received) {
			return true;
		}
		(void)osal_task_delay_ms(TEST_WAIT_STEP_MS);
		waited += TEST_WAIT_STEP_MS;
	}
	return false;
}

static void on_mqtt_message(const char *topic, const char *message,
			    size_t message_len)
{
	size_t topic_len = strlen(topic);
	size_t copy_len = message_len < sizeof(g_last_payload) - 1 ?
				  message_len :
				  sizeof(g_last_payload) - 1;

	if (topic_len >= sizeof(g_last_topic)) {
		topic_len = sizeof(g_last_topic) - 1;
	}

	memcpy(g_last_topic, topic, topic_len);
	g_last_topic[topic_len] = '\0';
	memcpy(g_last_payload, message, copy_len);
	g_last_payload[copy_len] = '\0';
	g_msg_count++;
	g_msg_received = true;
}

static void test_connect_sub_pub_unsub_and_reconfigure(void)
{
	char ca_buf[MQTT_CERT_MAX_SIZE] = { 0 };
	char client_cert_buf[MQTT_CERT_MAX_SIZE] = { 0 };
	char client_key_buf[MQTT_CERT_MAX_SIZE] = { 0 };
	int initial_count;

	TEST_START("Connect, subscribe, publish, unsubscribe, reconfigure");

	TEST_ASSERT(load_cert_file("ca.crt", ca_buf, sizeof(ca_buf)),
		    "CA cert loaded");
	TEST_ASSERT(load_cert_file("client.crt", client_cert_buf,
				   sizeof(client_cert_buf)),
		    "Client cert loaded");
	TEST_ASSERT(load_cert_file("client.key", client_key_buf,
				   sizeof(client_key_buf)),
		    "Client key loaded");

	TEST_ASSERT(mqtt_config_set_string("mqtts://127.0.0.1:8883",
					   MQTT_CONFIG_VALUE_ADDRESS),
		    "MQTT address set to TLS broker");
	TEST_ASSERT(mqtt_config_set_string("hq-mqtt-client",
					   MQTT_CONFIG_VALUE_CLIENT_ID),
		    "MQTT client id set");
	TEST_ASSERT(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL),
		    "MQTT SSL flag enabled");
	TEST_ASSERT(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SKIP_VERIFY),
		    "MQTT skip verify enabled for local test broker");
	TEST_ASSERT(mqtt_config_set_cert(ca_buf, strlen(ca_buf), 0,
					 MQTT_CONFIG_VALUE_CERT),
		    "CA cert configured");
	TEST_ASSERT(mqtt_config_set_cert(client_cert_buf,
					 strlen(client_cert_buf), 0,
					 MQTT_CONFIG_VALUE_CLIENT_CERT),
		    "Client cert configured");
	TEST_ASSERT(mqtt_config_set_cert(client_key_buf, strlen(client_key_buf),
					 0, MQTT_CONFIG_VALUE_CLIENT_KEY),
		    "Client key configured");

	mqtt_app_init();
	TEST_ASSERT(wait_for_connected(true), "MQTT connected to broker");

	TEST_ASSERT(mqtt_app_subscribe(TEST_TOPIC, 0, on_mqtt_message,
				       TEST_TIMEOUT_MS),
		    "Subscribe succeeded");

	g_msg_received = false;
	g_msg_count = 0;
	memset(g_last_topic, 0, sizeof(g_last_topic));
	memset(g_last_payload, 0, sizeof(g_last_payload));

	TEST_ASSERT(mqtt_app_post_data(TEST_TOPIC, "hello-from-functional-test",
				       0),
		    "Publish succeeded");
	TEST_ASSERT(wait_for_message(),
		    "Received callback on subscribed topic");
	TEST_ASSERT(strcmp(g_last_topic, TEST_TOPIC) == 0,
		    "Callback topic matches");
	TEST_ASSERT(strcmp(g_last_payload, "hello-from-functional-test") == 0,
		    "Callback payload matches");

	initial_count = g_msg_count;
	TEST_ASSERT(mqtt_app_unsubscribe(TEST_TOPIC, TEST_TIMEOUT_MS),
		    "Unsubscribe succeeded");
	TEST_ASSERT(mqtt_app_post_data(TEST_TOPIC, "should-not-be-received", 0),
		    "Publish after unsubscribe succeeded");
	(void)osal_task_delay_ms(1500U);
	TEST_ASSERT(g_msg_count == initial_count,
		    "No callback after unsubscribe");

	mqtt_app_deinit();
	TEST_ASSERT(!mqtt_app_is_connected(), "Disconnected after deinit");

	TEST_ASSERT(mqtt_config_set_string("hq-mqtt-client-updated",
					   MQTT_CONFIG_VALUE_CLIENT_ID),
		    "Config changed (client id)");
	TEST_ASSERT(mqtt_config_save(), "Config save succeeded");

	mqtt_app_init();
	TEST_ASSERT(wait_for_connected(true),
		    "Connected after applying updated config");

	mqtt_app_deinit();
	TEST_ASSERT(!mqtt_app_is_connected(),
		    "Disconnected cleanly after final deinit");
	TEST_END();
}

int mqtt_functional_tests_run(void)
{
	tests_run = 0;
	tests_passed = 0;
	tests_failed = 0;

	if (!setup_fs()) {
		printf("[FAIL] Filesystem setup failed\n");
		return 1;
	}

	printf("\n");
	printf("==================================================\n");
	printf("          MQTT Functional Tests (Broker)          \n");
	printf("==================================================\n");
	MongooseProcess_Init();
	test_connect_sub_pub_unsub_and_reconfigure();

	printf("\n");
	printf("==================================================\n");
	printf("                  TEST SUMMARY                    \n");
	printf("==================================================\n");
	printf("  Total tests:  %d\n", tests_run);
	printf("  Passed:       %d\n", tests_passed);
	printf("  Failed:       %d\n", tests_failed);
	printf("  Success rate: %.1f%%\n",
	       (tests_run > 0) ? (100.0 * tests_passed / tests_run) : 0.0);
	printf("==================================================\n");

	mqtt_app_deinit();
	MongooseProcess_Deinit();
	teardown_fs();

	return tests_failed;
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	(void)mqtt_functional_tests_run();
}
#else
int main(void)
{
	return mqtt_functional_tests_run() == 0 ? 0 : 1;
}
#endif
