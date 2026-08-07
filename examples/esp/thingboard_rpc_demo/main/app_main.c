#include <stdio.h>
#include <stdbool.h>
#include <osal_task.h>
#include <string.h>

#include "wifi_managment.h"

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "YourSSID"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "YourPassword"
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

	uint32_t timeout_ms = 10000;

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
	if (_init_wifi() != 0) {
		printf("Wi-Fi initialization failed.\n");
		return;
	}
	int ret = main_function();
	printf("[RPC_DEMO] app_main() exiting with code %d\n", ret);
}
