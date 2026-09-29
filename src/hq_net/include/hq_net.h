#ifndef HQ_NET_H
#define HQ_NET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HQ_NET_START_TIMEOUT_MS 10000u
#define HQ_NET_WAIT_POLL_INTERVAL_MS 50u

/** Available transport backends for this platform build. */
typedef enum hq_net_backend {
    HQ_NET_BACKEND_WIFI = 0
} hq_net_backend_t;

/** Startup behavior selected by the consuming project. */
typedef enum hq_net_startup_mode {
    /** Fresh Wi-Fi devices start AP+station; saved credentials use station. */
    HQ_NET_STARTUP_MODE_PROVISIONING = 0,
    /** Always start the station interface only. */
    HQ_NET_STARTUP_MODE_STATION_ONLY = 1
} hq_net_startup_mode_t;

/** Backend and startup policy; contains no backend-specific types. */
typedef struct hq_net_config {
    hq_net_backend_t backend;
    hq_net_startup_mode_t startup_mode;
} hq_net_config_t;

/**
 * Event callbacks are copied by value. The context must remain valid through
 * hq_net_stop(); callbacks run synchronously in the backend event task, and
 * lifecycle calls must not be made from a callback. A disconnect invokes
 * before_disconnected before on_disconnected while the event is current.
 * Callbacks are invoked with the hq_net lock held, so a stop cannot race a
 * delivery; keep them short and non-blocking.
 */
typedef struct hq_net_callbacks {
    void (*on_connected)(void *context);
    void (*before_disconnected)(void *context);
    void (*on_disconnected)(void *context);
    void *context;
} hq_net_callbacks_t;

typedef enum hq_net_status {
    HQ_NET_OK = 0,
    HQ_NET_ERR_INVALID_ARGUMENT = -1,
    HQ_NET_ERR_ALREADY_STARTED = -2,
    HQ_NET_ERR_START_FAILED = -3
} hq_net_status_t;

/** Start and synchronously wait for backend readiness before returning. */
int hq_net_start(const hq_net_config_t *config,
                 const hq_net_callbacks_t *callbacks);
/** Disarm callbacks, unsubscribe, and request backend disconnect. */
void hq_net_stop(void);
bool hq_net_is_connected(void);
bool hq_net_wait_connected(uint32_t timeout_ms);
int hq_net_reconnect(void);
bool hq_net_get_rssi(int *rssi_dbm);
/* RSSI maps linearly from -100 dBm (0%) to -50 dBm (100%), clamped. */
bool hq_net_get_link_quality(uint8_t *quality_percent, int *rssi_dbm);
/** Erase persisted network credentials without exposing their contents. */
bool hq_net_forget_credentials(void);

#ifdef __cplusplus
}
#endif

#endif