/**
 *******************************************************************************
 * @file    wifi_hal_driver.h
 * @author  Dmytro Shevchenko
 * @brief   Wi-Fi HAL — platform-agnostic interface implemented per target
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/
#ifndef WIFI_HAL_DRIVER_H
#define WIFI_HAL_DRIVER_H

#include "osal_error.h"
#include <stdbool.h>
#include <stdint.h>

/* Public macros -------------------------------------------------------------*/

/** @brief Maximum SSID length in characters (without null terminator). */
#define WIFI_HAL_SSID_MAX_LEN     32

/** @brief Maximum password length in characters (without null terminator). */
#define WIFI_HAL_PASSWORD_MAX_LEN 64

/* Public types --------------------------------------------------------------*/

/** @brief Wi-Fi operating mode passed to @c wifi_hal_start. */
typedef enum
{
  WIFI_HAL_MODE_STA   = 0, /**< Station (client) mode only. */
  WIFI_HAL_MODE_AP,        /**< Access point mode only.     */
  WIFI_HAL_MODE_APSTA,     /**< Simultaneous AP + station.  */
} wifi_hal_mode_t;

/** @brief Events delivered to the callback registered via @c wifi_hal_init. */
typedef enum
{
  WIFI_HAL_EVT_STA_DISCONNECTED       = 0, /**< Station lost its connection.            */
  WIFI_HAL_EVT_STA_GOT_IP,                 /**< Station obtained an IP address.         */
  WIFI_HAL_EVT_SCAN_DONE,                  /**< Background scan completed.              */
  WIFI_HAL_EVT_AP_CLIENT_CONNECTED,        /**< A client connected to the soft-AP.      */
  WIFI_HAL_EVT_AP_CLIENT_DISCONNECTED,     /**< A client disconnected from the soft-AP. */
} wifi_hal_event_t;

/** @brief Station credentials used by @c wifi_hal_set_sta_config. */
typedef struct
{
  char ssid[WIFI_HAL_SSID_MAX_LEN + 1];      /**< Target network SSID.     */
  char password[WIFI_HAL_PASSWORD_MAX_LEN + 1]; /**< Target network password. */
} wifi_hal_sta_config_t;

/** @brief Soft-AP configuration used by @c wifi_hal_set_ap_config. */
typedef struct
{
  char    ssid[WIFI_HAL_SSID_MAX_LEN + 1];      /**< AP broadcast SSID.                  */
  char    password[WIFI_HAL_PASSWORD_MAX_LEN + 1]; /**< AP password.                      */
  uint8_t max_connection;                        /**< Maximum simultaneous clients.       */
  uint8_t authmode;                              /**< Authentication mode (platform enum).*/
} wifi_hal_ap_config_t;

/** @brief Single access point record from a scan. */
typedef struct
{
  char ssid[WIFI_HAL_SSID_MAX_LEN + 1]; /**< Network SSID.               */
  int  channel;                          /**< Wi-Fi channel number.       */
  int  rssi;                             /**< Signal strength in dBm.     */
  int  authmode;                         /**< Authentication mode value.  */
} wifi_hal_ap_record_t;

/** @brief IPv4 network information returned by @c wifi_hal_get_sta_ip_info. */
typedef struct
{
  char ip[16];      /**< IPv4 address string.   */
  char netmask[16]; /**< Subnet mask string.    */
  char gw[16];      /**< Gateway address string.*/
} wifi_hal_ip_info_t;

/** @brief Data accompanying a HAL event (not all fields valid for every event). */
typedef struct
{
  uint32_t           disconnect_reason; /**< Reason code for disconnect events. */
  wifi_hal_ip_info_t ip_info;           /**< IP info for got-IP events.         */
} wifi_hal_event_data_t;

/**
 * @brief   Prototype for the HAL event callback.
 * @param   [in] event     - event identifier
 * @param   [in] data      - event-specific data, may be NULL
 * @param   [in] user_data - pointer passed during @c wifi_hal_init
 */
typedef void ( *wifi_hal_event_cb_t )( wifi_hal_event_t             event,
                                       const wifi_hal_event_data_t* data,
                                       void*                        user_data );

/** @brief Parameters passed to @c wifi_hal_init. */
typedef struct
{
  const char*         ap_ip;      /**< Soft-AP static IP address string.  */
  const char*         ap_gateway; /**< Soft-AP gateway string.            */
  const char*         ap_netmask; /**< Soft-AP subnet mask string.        */
  const char*         ap_dns;     /**< IPv4 DNS advertised by the AP DHCP
                                      server.  NULL or empty to omit the
                                      override (non-captive DHCP uses the
                                      platform default DNS).              */
  wifi_hal_event_cb_t event_cb;   /**< Event callback (must not be NULL). */
  void*               user_data;  /**< Passed unchanged to @p event_cb.  */
} wifi_hal_init_t;

/* Public functions ----------------------------------------------------------*/

/* Lifecycle / concurrency contract
 * ---------------------------------------------------------------------------
 * The HAL uses a process-lifetime lifecycle state machine with explicit
 * states @c UNINITIALIZED, @c ACTIVE, @c DEINITIALIZING and
 * @c CLEANUP_REQUIRED.  Exactly one init/deinit caller owns a lifecycle
 * transition at a time; later init/deinit callers wait on the lifecycle
 * condition and re-check the state, so a callback that re-enters a HAL API
 * while teardown is in progress receives the teardown-state error instead of
 * deadlocking.
 *
 * Concurrency boundary for the remaining HAL API (start/stop/connect/
 * disconnect/scan/getters/configuration):
 *   - a call that began before @c wifi_hal_deinit either completes while
 *     holding a session lease or fails without touching session resources,
 *   - a call that arrives once teardown has begun returns an error code
 *     without locking any per-session primitive,
 *   - @c wifi_hal_init waits for/serializes with teardown and never replaces a
 *     session left in the @c CLEANUP_REQUIRED state (a further
 *     @c wifi_hal_deinit must finish the cleanup first).
 */

/**
 * @brief   Initialize the Wi-Fi HAL.
 *
 * @details Registers the event callback and allocates the per-session platform
 *          resources (synchronization objects, event handlers, Wi-Fi stack).
 *          When the HAL is already initialized the call is an idempotent
 *          no-op: it returns @c OSAL_SUCCESS without resetting the active
 *          session and without touching live synchronization objects.
 *
 *          @c wifi_hal_init serializes with teardown: a call arriving while a
 *          @c wifi_hal_deinit is in progress waits for it to finish and then
 *          re-evaluates the session state.  It returns an error instead of
 *          replacing a session in @c CLEANUP_REQUIRED; a further deinit must
 *          finish that session first.
 *
 * @param   [in] init - initialisation parameters (@p event_cb must not be NULL)
 * @return  OSAL_SUCCESS on success, error code otherwise
 * @note    On failure, resources allocated before the failing step are
 *          released in exact reverse order and a later call to
 *          @c wifi_hal_deinit (or a retry of @c wifi_hal_init after
 *          @c wifi_hal_deinit) is safe.  If a resource could not be released,
 *          exact per-resource validity state and the @c CLEANUP_REQUIRED
 *          lifecycle state are retained; the session is then quiescent (no
 *          callbacks, no operation admission) and only a further
 *          @c wifi_hal_deinit retries the release.
 */
osal_status_t wifi_hal_init( const wifi_hal_init_t* init );

/**
 * @brief   Deinitialize the Wi-Fi HAL and release platform resources.
 *
 * @details Callback-quiescence and session-operation barrier.  When this
 *          function returns @c OSAL_SUCCESS the following guarantees hold:
 *            - all platform event sources (event-loop handlers, background
 *              threads) are unregistered or stopped (and joined),
 *            - every callback that had already started has returned,
 *            - every public HAL operation that had already started has
 *              completed while holding a session lease,
 *            - the event callback and user-data pointers are cleared,
 *            - no callback or public operation can begin afterwards.
 *
 *          Operations that begin while teardown is in progress return an error
 *          code without touching per-session resources; they never deadlock on
 *          a session-owned primitive, even when issued from inside a callback
 *          that is itself being drained by deinit.
 *
 *          Calling this function while the HAL is uninitialized, or calling it
 *          more than once between two @c wifi_hal_init calls, is a successful
 *          no-op that changes nothing.  Only one init/deinit caller owns a
 *          lifecycle transition at a time; later deinit callers wait for the
 *          owner and then re-check the session state.
 *
 *          When a platform resource cannot be released (for example a
 *          background thread cannot be joined), an error code is returned
 *          while enough exact per-resource lifecycle state (object-validity
 *          flags and the thread handle) is kept for a safe re-call; callback
 *          delivery and operation admission remain disabled in that case, and
 *          a later @c wifi_hal_deinit retries only the failed step.
 *
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_deinit( void );

/**
 * @brief   Start Wi-Fi in the requested operating mode.
 * @param   [in] mode - @c WIFI_HAL_MODE_STA, @c WIFI_HAL_MODE_AP,
 *                      or @c WIFI_HAL_MODE_APSTA
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_start( wifi_hal_mode_t mode );

/**
 * @brief   Stop Wi-Fi.
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_stop( void );

/**
 * @brief   Apply station credentials. Call before @c wifi_hal_connect.
 * @param   [in] config - SSID and password for the target network
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config );

/**
 * @brief   Apply soft-AP configuration. Call before @c wifi_hal_start.
 * @param   [in] config - SSID, password, and AP parameters
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config );

/**
 * @brief   Initiate a station connection to the configured AP.
 * @return  OSAL_SUCCESS if the connection attempt was accepted, error otherwise
 */
osal_status_t wifi_hal_connect( void );

/**
 * @brief   Disconnect the station from the current AP.
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_disconnect( void );

/**
 * @brief   Start an active Wi-Fi scan.
 * @param   [in] block - true to block until scan completion, false for async
 * @return  OSAL_SUCCESS if the scan started successfully, error otherwise
 */
osal_status_t wifi_hal_start_scan( bool block );

/**
 * @brief   Retrieve the access point records collected by the last scan.
 * @param   [out]    records       - array to be filled with AP records
 * @param   [in,out] in_out_count  - on input: capacity of @p records;
 *                                   on output: number of records written
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records,
                                       uint16_t*             in_out_count );

/**
 * @brief   Get the IP information of the station interface.
 * @param   [out] out_info - receives ip, netmask, and gateway strings
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info );

/**
 * @brief   Get the RSSI of the currently associated AP.
 * @param   [out] out_rssi - receives the RSSI value in dBm
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_get_sta_rssi( int* out_rssi );

/**
 * @brief   Enable or disable modem power-save mode.
 * @param   [in] enabled - true to enable, false to disable
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_set_power_save( bool enabled );

/**
 * @brief   Get the default MAC address of the device.
 * @param   [out] mac - 6-byte buffer to receive the MAC address
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] );

/**
 * @brief   Get the number of clients currently connected to the soft-AP.
 * @param   [out] out_client_count - receives the client count
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count );

/**
 * @brief   Check whether a string is a syntactically valid IPv4 address.
 * @param   [in] str - candidate string (may be NULL)
 * @return  true if @p str is a valid dotted-decimal IPv4 address, false otherwise
 *
 *          Platform-neutral validator used by HAL implementations to reject
 *          malformed IP string configuration (e.g. @c ap_dns) without exposing
 *          an ESP-IDF or POSIX-specific type in the public interface.
 */
bool wifi_hal_is_valid_ipv4( const char* str );

#endif
