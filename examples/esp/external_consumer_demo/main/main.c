/*
 * external_consumer_demo — minimal standalone ESP-IDF application that
 * consumes hq_platform as an external collection of ESP-IDF components.
 *
 * This fixture demonstrates the documented external-component mechanism:
 *   - the hq_platform component directories under src/ are registered
 *     through EXTRA_COMPONENT_DIRS in the top-level CMakeLists.txt,
 *   - application-level dependencies are declared with normal ESP-IDF
 *     REQUIRES declarations in main/CMakeLists.txt,
 *   - only public hq_platform headers are included, and
 *   - at least one existing public API is called from each of the
 *     osal, wifi and thingsboard components.
 *
 * It deliberately does not copy or duplicate any hq_platform source file and
 * does not add hq_platform component-private include directories manually.
 */

#include <stdio.h>
#include <string.h>

#include "osal_log.h"
#include "osal_task.h"
#include "tb_client.h"
#include "tb_telemetry.h"
#include "wifi_managment.h"

#ifndef DEMO_WIFI_SSID
#define DEMO_WIFI_SSID "external-consumer-demo"
#endif

#ifndef DEMO_WIFI_PASSWORD
#define DEMO_WIFI_PASSWORD "hq-platform-demo"
#endif

static int demo_osal(void)
{
    uint32_t uptime_ms = osal_task_get_time_ms();

    osal_log_info("[demo] osal OK: uptime=%lu ms", (unsigned long)uptime_ms);

    return (osal_task_delay_ms(10) == OSAL_SUCCESS) ? 0 : -1;
}

static int demo_wifi(void)
{
    wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLIENT);
    wifi_mgmt_init();

    if (!wifi_mgmt_set_ap_name(DEMO_WIFI_SSID, strlen(DEMO_WIFI_SSID))) {
        osal_log_error("[demo] wifi: failed to set SSID");
        return -1;
    }
    if (!wifi_mgmt_set_password(DEMO_WIFI_PASSWORD, strlen(DEMO_WIFI_PASSWORD))) {
        osal_log_error("[demo] wifi: failed to set password");
        return -1;
    }

    osal_log_info("[demo] wifi OK: type=%d ssid=%s",
                  (int)T_WIFI_TYPE_CLIENT, DEMO_WIFI_SSID);
    return 0;
}

static int demo_thingsboard(void)
{
    tb_client_t *client = NULL;
    tb_client_config_t config;

    memset(&config, 0, sizeof(config));
    snprintf(config.server_url, sizeof(config.server_url),
             "mqtt://thingsboard.local:1883");
    snprintf(config.access_token, sizeof(config.access_token),
             "external-consumer-demo-token");
    snprintf(config.device_name, sizeof(config.device_name),
             "external_consumer_demo");

    if (tb_client_init(&client, &config) != 0) {
        osal_log_error("[demo] thingsboard: tb_client_init failed");
        return -1;
    }

    if (tb_client_is_connected(client)) {
        tb_telemetry_send_int(client, "demo_counter", 1);
    } else {
        osal_log_info("[demo] thingsboard: not connected, skipping telemetry");
    }

    tb_client_deinit(client);
    osal_log_info("[demo] thingsboard OK");
    return 0;
}

void app_main(void)
{
    osal_log_info("[demo] external consumer demo starting");

    if (demo_osal() != 0) {
        osal_log_error("[demo] osal demo failed");
        return;
    }
    if (demo_wifi() != 0) {
        osal_log_error("[demo] wifi demo failed");
        return;
    }
    if (demo_thingsboard() != 0) {
        osal_log_error("[demo] thingsboard demo failed");
        return;
    }

    osal_log_info("[demo] external consumer demo finished successfully");
}