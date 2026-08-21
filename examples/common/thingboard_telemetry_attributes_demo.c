/*
 * ThingsBoard Telemetry + Attributes Demo
 *
 * Sends scalar telemetry, timestamped telemetry JSON, client attributes,
 * and a small telemetry batch payload.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_task.h"

#include "tb_attributes.h"
#include "tb_client.h"
#include "tb_telemetry.h"

#include "mongoose_process.h"
#include "mqtt_config.h"

#ifndef CONFIG_TA_DEMO_CLIENT_ID
#define CONFIG_TA_DEMO_CLIENT_ID "tb_telem_attr_0001"
#endif
#ifndef CONFIG_TA_DEMO_USERNAME
#define CONFIG_TA_DEMO_USERNAME "tb_telem_attr"
#endif
#ifndef CONFIG_TA_DEMO_PASSWORD
#define CONFIG_TA_DEMO_PASSWORD "tb_telem_attr"
#endif
#ifndef CONFIG_TA_DEMO_DEVICE_NAME
#define CONFIG_TA_DEMO_DEVICE_NAME "Telemetry Attributes Demo"
#endif
#ifndef CONFIG_TA_DEMO_MQTT_URL
#define CONFIG_TA_DEMO_MQTT_URL "mqtt://localhost:1883"
#endif
#ifndef CONFIG_TA_DEMO_SEND_PERIOD_MS
#define CONFIG_TA_DEMO_SEND_PERIOD_MS 7000
#endif
#ifndef CONFIG_TA_DEMO_RECONNECT_DELAY_MS
#define CONFIG_TA_DEMO_RECONNECT_DELAY_MS 3000
#endif
#ifndef CONFIG_TA_DEMO_ENABLE_QOS_DEMO
#define CONFIG_TA_DEMO_ENABLE_QOS_DEMO 1
#endif

#define TA_DEMO_CLIENT_ID CONFIG_TA_DEMO_CLIENT_ID
#define TA_DEMO_USERNAME CONFIG_TA_DEMO_USERNAME
#define TA_DEMO_PASSWORD CONFIG_TA_DEMO_PASSWORD
#define TA_DEMO_DEVICE_NAME CONFIG_TA_DEMO_DEVICE_NAME
#define TA_DEMO_MQTT_URL CONFIG_TA_DEMO_MQTT_URL
#define TA_DEMO_SEND_PERIOD_MS CONFIG_TA_DEMO_SEND_PERIOD_MS
#define TA_DEMO_RECONNECT_DELAY_MS CONFIG_TA_DEMO_RECONNECT_DELAY_MS

#define TB_TELEMETRY_TOPIC "v1/devices/me/telemetry"

static tb_client_t *g_tb_client;

static void log_publish_result(const char *label, int rc)
{
    if (rc != 0) {
        printf("[TA_DEMO] WARN: %s failed (rc=%d)\n", label, rc);
    } else {
        printf("[TA_DEMO] %s sent\n", label);
    }
}

static int connect_and_wait(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[TA_DEMO] connect request failed: %d\n", rc);
        return rc;
    }

    for (int i = 0; i < 20; i++) {
        if (tb_client_is_connected(g_tb_client)) {
            return 0;
        }
        osal_task_delay_ms(250);
    }

    tb_client_disconnect(g_tb_client);
    return -1;
}

static void send_scalar_telemetry(uint32_t sample_no)
{
    int rc;

    rc = tb_telemetry_send_double(g_tb_client, "temperature", 21.5 + (sample_no % 5));
    log_publish_result("scalar telemetry (double)", rc);

    rc = tb_telemetry_send_int(g_tb_client, "pressure_hpa", 1000 + (int64_t)(sample_no % 7));
    log_publish_result("scalar telemetry (int)", rc);

    rc = tb_telemetry_send_bool(g_tb_client, "fan_enabled", (sample_no % 2) == 0);
    log_publish_result("scalar telemetry (bool)", rc);

    rc = tb_telemetry_send_string(g_tb_client, "mode", (sample_no % 2) == 0 ? "normal" : "eco");
    log_publish_result("scalar telemetry (string)", rc);
}

static void send_timestamped_telemetry(uint32_t sample_no)
{
    char payload[256];
    uint64_t ts = (uint64_t)osal_task_get_time_ms();

    snprintf(payload, sizeof(payload),
             "{\"ts\":%" PRIu64 ",\"values\":{\"voltage\":%.2f,\"sample\":%u}}",
             ts, 3.25 + ((double)(sample_no % 4) * 0.01), (unsigned)sample_no);

    log_publish_result("timestamped telemetry JSON",
                       tb_telemetry_send_json(g_tb_client, payload));
}

static void send_client_attributes(void)
{
    int rc;

    rc = tb_attributes_send_json(
        g_tb_client,
        "{\"hw_model\":\"HQ-Edge-1\",\"serial\":\"SN-TA-0001\","
        "\"fw_version\":\"1.0.0\",\"os\":\"posix\"}");
    log_publish_result("client attributes JSON", rc);

    rc = tb_attributes_send_string(g_tb_client, "sdk", "hq_platform");
    log_publish_result("client attributes scalar", rc);
}

static void send_small_telemetry_batch(uint32_t sample_no)
{
    char payload[256];

    snprintf(payload, sizeof(payload),
             "{\"cpu\":%u,\"ram_kb\":%u,\"uptime_s\":%u,\"batch_no\":%u}",
             (unsigned)(20U + (sample_no % 10)),
             (unsigned)(16000U + (sample_no % 100)),
             (unsigned)(osal_task_get_time_ms() / 1000U),
             (unsigned)sample_no);

    log_publish_result("small telemetry batch", tb_telemetry_send_json(g_tb_client, payload));
}

static void send_qos_demo(uint32_t sample_no)
{
#if CONFIG_TA_DEMO_ENABLE_QOS_DEMO
    char payload[128];

    snprintf(payload, sizeof(payload),
             "{\"qos_demo\":true,\"sample\":%u}",
             (unsigned)sample_no);

    log_publish_result("telemetry QoS1",
                       tb_client_publish_with_qos(g_tb_client, TB_TELEMETRY_TOPIC,
                                                  payload, 1));
#else
    (void)sample_no;
#endif
}

int main_function(void)
{
    int rc;
    uint32_t sample_no = 1;

    printf("\n=============================================\n");
    printf("   ThingsBoard Telemetry + Attributes Demo\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", TA_DEMO_CLIENT_ID);
    printf("  Broker    : %s\n", TA_DEMO_MQTT_URL);
    printf("=============================================\n\n");

    MongooseProcess_Init();

    tb_client_config_t tb_cfg = {
        .server_url = TA_DEMO_MQTT_URL,
        .access_token = TA_DEMO_USERNAME,
        .client_id = TA_DEMO_CLIENT_ID,
        .device_name = TA_DEMO_DEVICE_NAME,
    };

    rc = tb_client_init(&g_tb_client, &tb_cfg);
    if (rc != 0) {
        printf("[TA_DEMO] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    mqtt_config_set_string(TA_DEMO_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[TA_DEMO] Connecting...\n");
            rc = connect_and_wait();
            if (rc != 0) {
                printf("[TA_DEMO] Reconnect in %d ms\n", TA_DEMO_RECONNECT_DELAY_MS);
                osal_task_delay_ms(TA_DEMO_RECONNECT_DELAY_MS);
                continue;
            }
            printf("[TA_DEMO] Connected\n");
        }

        send_scalar_telemetry(sample_no);
        send_timestamped_telemetry(sample_no);
        send_client_attributes();
        send_small_telemetry_batch(sample_no);
        send_qos_demo(sample_no);

        sample_no++;
        osal_task_delay_ms(TA_DEMO_SEND_PERIOD_MS);
    }
}

#ifndef CONFIG_HQ_PLATFORM_ESP
int main(void)
{
    return main_function();
}
#endif
