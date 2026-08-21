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
#include "unity.h"

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

static volatile bool g_msg_received = false;
static volatile int g_msg_count = 0;
static char g_last_topic[256] = { 0 };
static char g_last_payload[512] = { 0 };

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

static bool osal_write_file(const char *path, const char *data, size_t len)
{
	osal_file_id_t fd;
	int32_t n;

	fd = osal_open_create(
		path, OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
		OSAL_WRITE_ONLY);
	if (fd < 0)
		return false;

	n = osal_write(fd, data, len);
	(void)osal_close(fd);
	return n >= 0 && (size_t)n == len;
}

static bool osal_read_file(const char *path, char *buffer, size_t max_len)
{
	osal_fstat_t st = { 0 };
	osal_file_id_t fd;
	size_t to_read;
	int32_t n;

	if (!path || !buffer || max_len < 2)
		return false;

	if (osal_stat(path, &st) != OSAL_SUCCESS || st.file_size == 0)
		return false;

	fd = osal_open_create(path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
	if (fd < 0)
		return false;

	to_read = st.file_size < max_len - 1 ? st.file_size : max_len - 1;
	n = osal_read(fd, buffer, to_read);
	(void)osal_close(fd);

	if (n < 0)
		return false;

	buffer[n] = '\0';
	return true;
}

static bool provision_cert_from_host(const char *host_path,
				     const char *osal_path)
{
	FILE *fp;
	char buf[MQTT_CERT_MAX_SIZE];
	size_t n;

	fp = fopen(host_path, "rb");
	if (!fp)
		return false;

	n = fread(buf, 1, sizeof(buf) - 1, fp);
	(void)fclose(fp);
	if (n == 0)
		return false;

	buf[n] = '\0';
	return osal_write_file(osal_path, buf, n);
}

static bool provision_cert_file(const char *name, const char *osal_dest)
{
	char path[256] = { 0 };
	const char *prefixes[] = { "cert/", "../cert/", "../../cert/" };

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		(void)snprintf(path, sizeof(path), "%s%s", prefixes[i], name);
		if (provision_cert_from_host(path, osal_dest))
			return true;
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

void setUp(void)
{
	g_msg_received = false;
	g_msg_count = 0;
	memset(g_last_topic, 0, sizeof(g_last_topic));
	memset(g_last_payload, 0, sizeof(g_last_payload));

	if (!setup_fs()) {
		TEST_FAIL_MESSAGE("MQTT functional filesystem setup failed");
		return;
	}

	MongooseProcess_Init();
}

void tearDown(void)
{
	mqtt_app_deinit();
	MongooseProcess_Deinit();
	teardown_fs();
}

static void test_connect_sub_pub_unsub_and_reconfigure(void)
{
	int initial_count;

	/* Provision cert files from host FS into OSAL FS */
	TEST_ASSERT_TRUE_MESSAGE(provision_cert_file("ca.crt", "ca.crt"),
				 "CA cert provisioned to OSAL FS");
	TEST_ASSERT_TRUE_MESSAGE(provision_cert_file("client.crt", "client.crt"),
				 "Client cert provisioned to OSAL FS");
	TEST_ASSERT_TRUE_MESSAGE(provision_cert_file("client.key", "client.key"),
				 "Client key provisioned to OSAL FS");

	mqtt_config_init();
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_string("mqtts://127.0.0.1:8883",
						       MQTT_CONFIG_VALUE_ADDRESS),
				 "MQTT address set to TLS broker");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_string("hq-mqtt-client",
						       MQTT_CONFIG_VALUE_CLIENT_ID),
				 "MQTT client id set");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL),
				 "MQTT SSL flag enabled");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SKIP_VERIFY),
				 "MQTT skip verify enabled for local test broker");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
							     "ca.crt",
							     MQTT_CONFIG_VALUE_CERT),
				 "CA cert configured");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
							     "client.crt",
							     MQTT_CONFIG_VALUE_CLIENT_CERT),
				 "Client cert configured");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
							     "client.key",
							     MQTT_CONFIG_VALUE_CLIENT_KEY),
				 "Client key configured");

	mqtt_app_init();
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "MQTT connected to broker");

	TEST_ASSERT_TRUE_MESSAGE(mqtt_app_subscribe(TEST_TOPIC, 0,
						    on_mqtt_message,
						    TEST_TIMEOUT_MS),
				 "Subscribe succeeded");

	g_msg_received = false;
	g_msg_count = 0;
	memset(g_last_topic, 0, sizeof(g_last_topic));
	memset(g_last_payload, 0, sizeof(g_last_payload));

	TEST_ASSERT_TRUE_MESSAGE(mqtt_app_post_data(TEST_TOPIC,
						    "hello-from-functional-test",
						    0),
				 "Publish succeeded");
	TEST_ASSERT_TRUE_MESSAGE(wait_for_message(),
				 "Received callback on subscribed topic");
	TEST_ASSERT_EQUAL_STRING_MESSAGE(TEST_TOPIC, g_last_topic,
					 "Callback topic matches");
	TEST_ASSERT_EQUAL_STRING_MESSAGE("hello-from-functional-test",
					 g_last_payload,
					 "Callback payload matches");

	initial_count = g_msg_count;
	TEST_ASSERT_TRUE_MESSAGE(mqtt_app_unsubscribe(TEST_TOPIC, TEST_TIMEOUT_MS),
				 "Unsubscribe succeeded");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_app_post_data(TEST_TOPIC,
						    "should-not-be-received",
						    0),
				 "Publish after unsubscribe succeeded");
	(void)osal_task_delay_ms(1500U);
	TEST_ASSERT_EQUAL_INT_MESSAGE(initial_count, g_msg_count,
				      "No callback after unsubscribe");

	mqtt_app_deinit();
	TEST_ASSERT_FALSE_MESSAGE(mqtt_app_is_connected(),
				  "Disconnected after deinit");

	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_string("hq-mqtt-client-updated",
						       MQTT_CONFIG_VALUE_CLIENT_ID),
				 "Config changed (client id)");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_save(), "Config save succeeded");

	mqtt_app_init();
	TEST_ASSERT_TRUE_MESSAGE(wait_for_connected(true),
				 "Connected after applying updated config");

	mqtt_app_deinit();
	TEST_ASSERT_FALSE_MESSAGE(mqtt_app_is_connected(),
				  "Disconnected cleanly after final deinit");
}

static void mqtt_functional_tests_run(void)
{
	RUN_TEST(test_connect_sub_pub_unsub_and_reconfigure);
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	UNITY_BEGIN();
	mqtt_functional_tests_run();
	UNITY_END();
}
#else
int main(void)
{
	UNITY_BEGIN();
	mqtt_functional_tests_run();
	return UNITY_END();
}
#endif