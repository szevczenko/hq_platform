/*
 * MQTT Config Tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_config.h"
#include "osal_mount.h"

#ifdef ESP_PLATFORM
#define MQTT_TEST_IMAGE_PATH  "flash_test"
#define MQTT_TEST_MOUNT_POINT "/littlefs"
#else
#define MQTT_TEST_IMAGE_PATH  "/tmp/mqtt_config_test.img"
#define MQTT_TEST_MOUNT_POINT "/"
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static bool g_callback_called = false;

#define TEST_ASSERT(condition, message)                                           \
	do {                                                                        \
		tests_run++;                                                          \
		if (condition) {                                                      \
			tests_passed++;                                                 \
			printf("[PASS] %s\n", message);                                \
		} else {                                                              \
			tests_failed++;                                                 \
			printf("[FAIL] %s\n", message);                                \
		}                                                                     \
	} while (0)

#define TEST_START(name)                                                           \
	printf("\n==================================================\n");      \
	printf("TEST: %s\n", name);                                              \
	printf("==================================================\n")

#define TEST_END() printf("--------------------------------------------------\n")

static void on_apply_config(void)
{
	g_callback_called = true;
}

static bool mqtt_test_fs_setup(void)
{
	(void)osal_unmount(MQTT_TEST_MOUNT_POINT);
	(void)osal_rmfs(MQTT_TEST_IMAGE_PATH);

	if (osal_initfs(NULL, MQTT_TEST_IMAGE_PATH, MQTT_TEST_MOUNT_POINT,
			4096U, 256U) != OSAL_SUCCESS) {
		return false;
	}

	if (osal_mkfs(NULL, MQTT_TEST_IMAGE_PATH, MQTT_TEST_MOUNT_POINT, 4096U,
		      256U) != OSAL_SUCCESS) {
		return false;
	}

	if (osal_mount(MQTT_TEST_IMAGE_PATH, MQTT_TEST_MOUNT_POINT) !=
	    OSAL_SUCCESS) {
		return false;
	}

	return true;
}

static void mqtt_test_fs_teardown(void)
{
	(void)osal_unmount(MQTT_TEST_MOUNT_POINT);
	(void)osal_rmfs(MQTT_TEST_IMAGE_PATH);
}

static void test_defaults_loaded(void)
{
	bool ssl_enabled = true;
	const char *address = NULL;
	const char *prefix = NULL;
	const char *post_topic = NULL;

	TEST_START("Load Defaults");

	mqtt_config_init();
	address = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
	prefix = mqtt_config_get_string(MQTT_CONFIG_VALUE_TOPIC_PREFIX);
	post_topic = mqtt_config_get_string(MQTT_CONFIG_VALUE_POST_DATA_TOPIC);

	TEST_ASSERT(address != NULL && address[0] != '\0',
		    "Default address is available");
	TEST_ASSERT(prefix != NULL && prefix[0] != '\0',
		    "Default topic prefix is available");
	TEST_ASSERT(post_topic != NULL && post_topic[0] != '\0',
		    "Default post topic is available");
	TEST_ASSERT(mqtt_config_get_bool(&ssl_enabled, MQTT_CONFIG_VALUE_SSL),
		    "Default SSL flag can be read");

	TEST_END();
}

static void test_save_triggers_callback(void)
{
	bool save_ok;

	TEST_START("Save Triggers Callback");

	g_callback_called = false;
	mqtt_config_set_callback(on_apply_config);
	TEST_ASSERT(mqtt_config_set_string("mqtt://127.0.0.1:1883",
				      MQTT_CONFIG_VALUE_ADDRESS),
		    "Address can be updated");

	save_ok = mqtt_config_save();
	TEST_ASSERT(save_ok, "Config save succeeded");
	TEST_ASSERT(g_callback_called, "Apply callback was called");

	mqtt_config_set_callback(NULL);
	TEST_END();
}

static void mqtt_config_tests_reset(void)
{
	tests_run = 0;
	tests_passed = 0;
	tests_failed = 0;
}

int mqtt_config_tests_run(void)
{
	mqtt_config_tests_reset();

	if (!mqtt_test_fs_setup()) {
		printf("[FAIL] MQTT test filesystem setup failed\n");
		return 1;
	}

	printf("\n");
	printf("==================================================\n");
	printf("             MQTT Config Tests                    \n");
	printf("==================================================\n");
	printf("\n");

	test_defaults_loaded();
	test_save_triggers_callback();

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

	mqtt_test_fs_teardown();

	return tests_failed;
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	(void)mqtt_config_tests_run();
}
#else
int main(void)
{
	return mqtt_config_tests_run() == 0 ? 0 : 1;
}
#endif
