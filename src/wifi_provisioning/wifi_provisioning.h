/* Generic Wi-Fi provisioning lifecycle and event API. */
#ifndef WIFI_PROVISIONING_H_
#define WIFI_PROVISIONING_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  WIFI_PROVISIONING_OK = 0,
  WIFI_PROVISIONING_ERR_NOT_INITIALIZED = -1,
  WIFI_PROVISIONING_ERR_MONGOOSE_NOT_RUNNING = -2,
  WIFI_PROVISIONING_ERR_START_FAILED = -3,
  WIFI_PROVISIONING_ERR_STOP_FAILED = -4,
  WIFI_PROVISIONING_ERR_DEPENDENCY = -5,
  WIFI_PROVISIONING_ERR_MODE_TRANSITION = -6,
  WIFI_PROVISIONING_ERR_HTTP_BIND = -7,
  WIFI_PROVISIONING_ERR_DNS_BIND = -8,
  WIFI_PROVISIONING_ERR_NO_AP = -9,
  WIFI_PROVISIONING_ERR_RESOURCE = -10
} wifi_provisioning_status_t;

typedef enum
{
  WIFI_PROVISIONING_STATE_STOPPED = 0,
  WIFI_PROVISIONING_STATE_STARTING = 1,
  WIFI_PROVISIONING_STATE_RUNNING = 2,
  WIFI_PROVISIONING_STATE_STOPPING = 3,
  WIFI_PROVISIONING_STATE_ERROR = 4
} wifi_provisioning_state_t;

typedef enum
{
  WIFI_PROVISIONING_EVENT_NONE = 0,
  WIFI_PROVISIONING_EVENT_STARTED = 1,
  WIFI_PROVISIONING_EVENT_SUCCEEDED = 2,
  WIFI_PROVISIONING_EVENT_FAILED = 3
} wifi_provisioning_event_t;

wifi_provisioning_status_t wifi_provisioning_init(void);
wifi_provisioning_status_t wifi_provisioning_deinit(void);
wifi_provisioning_status_t wifi_provisioning_start(void);
wifi_provisioning_status_t wifi_provisioning_stop(void);
wifi_provisioning_state_t wifi_provisioning_get_state(void);
bool wifi_provisioning_is_active(void);
bool wifi_provisioning_has_saved_credentials(void);
wifi_provisioning_event_t wifi_provisioning_poll_event(void);

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
bool wifi_provisioning_set_http_url(const char *url);
bool wifi_provisioning_set_dns_url(const char *url);
#endif

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROVISIONING_H_ */