#include <stdio.h>
#include <stdbool.h>
#include <osal_task.h>
#include <string.h>

#include "osal_ota.h"

#include "wifi_managment.h"

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "wifi_ssid"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "password"
#endif

extern int main_function(void);

static bool connected;
static bool failed;

void _connected(void)
{
	connected = true;
	printf("Wi-Fi connected.\n");
}

void _disconnected(void)
{
	connected = false;
	failed = true;
	printf("Wi-Fi disconnected.\n");
}

static int _init_wifi(void)
{
	wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLIENT);
	wifi_mgmt_init();
	wifi_mgmt_start();
	wifi_mgmt_register_connect_cb(_connected);
	wifi_mgmt_register_disconnect_cb(_disconnected);

	if (!wifi_mgmt_set_ap_name(CONFIG_WIFI_SSID, strlen(CONFIG_WIFI_SSID))) {
		printf("Cannot set SSID.\n");
		return -1;
	}
	if (!wifi_mgmt_set_password(CONFIG_WIFI_PASSWORD, strlen(CONFIG_WIFI_PASSWORD))) {
		printf("Cannot set password.\n");
		return -1;
	}

	if (!wifi_mgmt_connect()) {
		printf("Cannot connect to Wi-Fi.\n");
		return -1;
	}

	printf("Connecting to Wi-Fi SSID: %s\n", CONFIG_WIFI_SSID);

	uint32_t timeout_ms = 10000; // 10 seconds

	while (!connected && timeout_ms > 0) {
		if (failed) {
			printf("Wi-Fi connection failed.\n");
			return -1;
		}
		osal_task_delay_ms(100);
		timeout_ms -= 100;
	}
	if (!connected) {
		printf("Wi-Fi connection timed out.\n");
		return -1;
	}

	return 0;
}

void app_main(void)
{
	osal_ota_security_info_t security_info = { 0 };
	osal_status_t ota_rc = osal_ota_init();
	if (ota_rc != OSAL_SUCCESS) {
		printf("OTA init/confirm failed: %d\n", ota_rc);
	}
	if (osal_ota_get_security_info(&security_info) == OSAL_SUCCESS) {
		printf("OTA security: secure_boot=%s flash_encryption=%s "
		       "rollback=%s anti_rollback=%s boot_state=%d\n",
		       security_info.secure_boot_enforced ? "enforced" : "off",
		       security_info.flash_encryption_enabled ? "enabled" : "off",
		       security_info.rollback_enabled ? "enabled" : "off",
		       security_info.anti_rollback_enabled ? "enabled" : "off",
		       (int) security_info.boot_state);
	}

	if (_init_wifi() != 0) {
		printf("Wi-Fi initialization failed.\n");
		return;
	}
	int ret = main_function();
	printf("[LAMP] app_main() exiting with code %d\n", ret);
}