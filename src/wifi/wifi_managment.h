/**
 *******************************************************************************
 * @file    wifi_managment.h
 * @author  Dmytro Shevchenko
 * @brief   WiFi management driver — state machine, connect/scan/callback API
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/
#ifndef WIFI_MANAGMENT_H
#define WIFI_MANAGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public macros -------------------------------------------------------------*/

#ifndef WIFI_AP_NAME
#define WIFI_AP_NAME "Bimbrownik"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "SuperTrudne1!-_"
#endif

/** @brief Maximum SSID length in characters (without null terminator). */
#define MAX_SSID_SIZE        32

/** @brief Maximum password length in characters (without null terminator). */
#define MAX_PASSWORD_SIZE    64

/** @brief Maximum number of APs returned by a single scan. */
#define WIFI_DRV_MAX_SCAN_AP 32

/** @brief Default access point IP address. */
#define DEFAULT_AP_IP      "10.10.0.1"

/** @brief Default access point gateway address. Should match @c DEFAULT_AP_IP. */
#define DEFAULT_AP_GATEWAY "10.10.0.1"

/** @brief Default access point subnet mask. */
#define DEFAULT_AP_NETMASK "255.255.255.0"

#ifndef WIFI_TEST_TASK
#define WIFI_TEST_TASK 0
#endif

/* Public types --------------------------------------------------------------*/

/** @brief Selects the operating role of the Wi-Fi driver. */
typedef enum
{
  T_WIFI_TYPE_SERVER  = 1, /**< Access-point only mode. */
  T_WIFI_TYPE_CLIENT  = 2, /**< Station (client) only mode. */
  T_WIFI_TYPE_CLI_SER = 3, /**< Simultaneous AP + station mode. */
} wifi_type_t;

/** @brief Stores SSID and password of the last saved connection. */
typedef struct
{
  char ssid[33];
  char password[64];
} wifi_mgmt_con_data_t;

/**
 * @brief   Structured IP information provided after a successful connection.
 *
 * @note    The @c urc field encodes the update reason code:
 *          0 = connected, 1 = failed attempt, 2 = user disconnect,
 *          3 = lost connection.
 */
typedef struct
{
  char ssid[MAX_SSID_SIZE + 1]; /**< SSID of the associated access point.   */
  char ip[16];                  /**< Station IPv4 address string.            */
  char netmask[16];             /**< Subnet mask string.                     */
  char gw[16];                  /**< Default gateway string.                 */
  int  urc;                     /**< Update reason code.                     */
} wifi_mgmt_ip_info_t;

/** @brief Metadata for one access point returned by a scan. */
typedef struct
{
  char ssid[MAX_SSID_SIZE + 1]; /**< SSID of the access point.   */
  int  chan;                    /**< Wi-Fi channel number.        */
  int  rssi;                   /**< Signal strength in dBm.      */
  int  auth;                   /**< Authentication mode value.   */
} wifi_mgmt_ap_info_t;

/** @brief Collection of access point records from the last scan. */
typedef struct
{
  uint16_t            count;                       /**< Number of valid entries. */
  wifi_mgmt_ap_info_t items[WIFI_DRV_MAX_SCAN_AP]; /**< AP record array.         */
} wifi_mgmt_ap_list_t;

/** @brief Prototype for connect/disconnect event callbacks (legacy API). */
typedef void ( *wifi_mgmt_callback_t )( void );

/** @brief Typed Wi-Fi management events delivered to subscribers. */
typedef enum
{
  WIFI_MGMT_EVENT_CONNECTED      = 0, /**< Station connected and obtained IP.   */
  WIFI_MGMT_EVENT_DISCONNECTED   = 1, /**< Station disconnected (user or lost). */
  WIFI_MGMT_EVENT_CONNECT_FAILED = 2, /**< Connect attempt failed.              */
  WIFI_MGMT_EVENT_SCAN_COMPLETED = 3, /**< Scan finished; results are available. */
  WIFI_MGMT_EVENT_MODE_CHANGED   = 4, /**< Driver operating mode changed.       */
} wifi_mgmt_event_t;

/**
 * @brief Prototype for typed event subscribers.
 * @param [in] event     - event that triggered the callback
 * @param [in] user_data - user context supplied at subscription time
 */
typedef void ( *wifi_mgmt_event_cb_t )( wifi_mgmt_event_t event, void* user_data );

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Set the operating role before calling @c wifi_mgmt_init.
 * @param   [in] type - @c T_WIFI_TYPE_SERVER, @c T_WIFI_TYPE_CLIENT,
 *                      or @c T_WIFI_TYPE_CLI_SER
 * @note    Applies the mode change synchronously. Prefer
 *          @c wifi_mgmt_request_mode for runtime transitions.
 */
void wifi_mgmt_set_wifi_type( wifi_type_t type );

/**
 * @brief   Asynchronously request a runtime transition to @p type.
 *
 * @details The requested mode is serialized inside the Wi-Fi worker task so
 *          multiple transitions are applied one at a time. The HAL is stopped
 *          and restarted only when @p type differs from the current mode, and
 *          @c WIFI_MGMT_EVENT_MODE_CHANGED is emitted only after the HAL
 *          transition succeeds.
 *
 * @param   [in] type - @c T_WIFI_TYPE_SERVER, @c T_WIFI_TYPE_CLIENT,
 *                      or @c T_WIFI_TYPE_CLI_SER
 * @return  true if the request was accepted (or @p type already equals the
 *          current mode), false if @p type is invalid.
 * @note    This call never blocks on the transition result; a HAL start
 *          failure leaves the worker in a defined, recoverable state.
 */
bool wifi_mgmt_request_mode( wifi_type_t type );

/**
 * @brief   Initialize the Wi-Fi management module and spawn the worker task.
 *
 * @details Initialization is transactional: every mutex, semaphore, task
 *          attribute and the single worker task must be created successfully
 *          before the module is published as initialized. The worker is created
 *          last so a failed init can never run a task against a partially built
 *          module. On any creation failure, partially created objects are
 *          deleted in exact reverse order and the module is left
 *          indistinguishable from never initialized.
 *
 * @note    A single lifecycle owner must serialize init/start/stop calls.
 *          Calling this function again after a successful init is a no-op.
 *          Call @c wifi_mgmt_set_wifi_type before this function.
 */
void wifi_mgmt_init( void );

/**
 * @brief   Request an acknowledged, restartable Wi-Fi stop.
 *
 * @details One lifecycle owner must serialize init/start/stop calls. Two
 *          simultaneous stop callers and arbitrary init/start/stop races are
 *          unsupported. There is currently no wifi_mgmt_deinit() API; final
 *          manager teardown remains a later task.
 *
 *          A non-fast-path call allocates a fresh request generation and waits
 *          for the persistent Wi-Fi worker to call wifi_hal_stop(), then
 *          wifi_hal_deinit(), and publish WIFI_APP_DISABLE. The return value
 *          belongs to this call's exact generation: true means both worker-
 *          owned HAL calls succeeded and the disabled state was published.
 *          A timeout or HAL error returns false and requires a fresh,
 *          serialized stop before restart.
 *
 * @return  true when this call's exact stop generation completed cleanly;
 *          false on timeout or either HAL error.
 * @note    Stop-before-init and repeated stop after an acknowledged clean stop
 *          return true without a fresh HAL round.
 */
bool wifi_mgmt_stop( void );

/**
 * @brief   Final deinitialization: join the Wi-Fi worker and release every
 *          management synchronization object.
 *
 * @details Bounded, owner-driven teardown that builds on the restartable
 *          @c wifi_mgmt_stop() protocol and the transactional init from
 *          TASK-135A.  Dependents must unsubscribe and cease all Wi-Fi
 *          management calls before calling this; deinit does not race
 *          arbitrary published entry points.
 *
 *          If the worker-owned HAL teardown is not already acknowledged
 *          successful, @c wifi_mgmt_stop() is first invoked and joined (an
 *          owner-thread HAL fallback is never used).  A separate terminate
 *          request is then queued for the worker.  On observing it the worker
 *          leaves its state loop holding no management mutex, publishes the
 *          captured quiesced generation, signals quiescence, and parks until
 *          the owner calls osal_task_delete().  Only after the task is deleted
 *          and the HAL callback is quiescent are typed subscriptions and
 *          legacy callback lists cleared and ip_sem, scan_sem, ready/stop/quit
 *          semaphores, event_mutex, and state_mutex released in one documented
 *          reverse-reachability order.  The module is only published as
 *          uninitialized after every release.
 *
 * @return  true when called before init or after a successful deinit (a
 *          deinit-before-init is a no-op).  Returns false on stop
 *          timeout/HAL error, quiescence timeout, or task deletion failure.
 * @note    On failure every live object and state needed for a safe serialized
 *          retry is retained; a parked worker after a failed task deletion
 *          remains represented as live and retryable.
 */
bool wifi_mgmt_deinit( void );

/**
 * @brief   Start the Wi-Fi management state machine.
 * @note    Has no effect if already started.
 */
void wifi_mgmt_start( void );

/**
 * @brief   Wait until Wi-Fi startup completes.
 *
 * @details Blocks the calling task until the Wi-Fi worker has finished
 *          bringing the stack up: the HAL event callback is installed, the
 *          requested HAL mode has been started successfully, the initial
 *          snapshots (IP state, mode event) are published, and the machine
 *          reached the idle or ready state.  The readiness signal is emitted
 *          synchronously from the init transition, so callers may rely on it
 *          to distinguish "worker task created" from "Wi-Fi initialized".
 *
 * @param   [in] timeout_ms - maximum time to wait, in milliseconds
 * @return  true if startup completed successfully, false if the wait timed
 *          out or if HAL initialization / mode startup failed.
 * @note    When the module has not been started, or after @c wifi_mgmt_stop,
 *          the call returns false once @p timeout_ms elapses.
 */
bool wifi_mgmt_wait_ready( uint32_t timeout_ms );

/**
 * @brief   Select the station to connect to from the last scan result.
 * @param   [in] num - zero-based index into the scanned AP list
 * @return  true if the index is valid, otherwise false
 */
bool wifi_mgmt_set_from_ap_list( uint8_t num );

/**
 * @brief   Set the station SSID to connect to.
 * @param   [in] name - null-terminated SSID string
 * @param   [in] len  - length of @p name, must be < @c WIFI_HAL_SSID_MAX_LEN
 * @return  true if success, otherwise false
 */
bool wifi_mgmt_set_ap_name( const char* name, size_t len );

/**
 * @brief   Set the station password.
 * @param   [in] passwd - null-terminated password string
 * @param   [in] len    - length of @p passwd
 * @return  true if success, otherwise false
 */
bool wifi_mgmt_set_password( const char* passwd, size_t len );

/**
 * @brief   Request an asynchronous connect operation.
 * @return  false when driver is in AP-only mode, otherwise true
 */
bool wifi_mgmt_connect( void );

/**
 * @brief   Request an asynchronous disconnect operation.
 * @return  false when driver is in AP-only mode, otherwise true
 */
bool wifi_mgmt_disconnect( void );

/**
 * @brief   Check whether the driver is in the idle state and can connect.
 * @return  true if ready to accept @c wifi_mgmt_connect, otherwise false
 */
bool wifi_mgmt_ready_to_connect( void );

/**
 * @brief   Check whether a connection attempt is currently in progress.
 * @return  true if connecting or waiting for IP, otherwise false
 */
bool wifi_mgmt_trying_connect( void );

/**
 * @brief   Start a blocking Wi-Fi scan and wait for results.
 * @return  true if the scan was started successfully, otherwise false
 */
bool wifi_mgmt_start_scan( void );

/**
 * @brief   Start a non-blocking Wi-Fi scan; results delivered via event callback.
 * @return  true if the scan was started successfully, otherwise false
 */
bool wifi_mgmt_start_scan_no_block( void );

/**
 * @brief   Get the configured station SSID.
 * @param   [out] name - buffer to receive a null-terminated SSID string
 * @return  true if success, otherwise false
 */
bool wifi_mgmt_get_ap_name( char* name );

/**
 * @brief   Get the SSID of an access point from the last scan result.
 * @param   [in]  number - zero-based index into the scanned list
 * @param   [out] name   - buffer to receive the null-terminated SSID
 * @return  true if the index is valid, otherwise false
 */
bool wifi_mgmt_get_name_from_scanned_list( uint8_t number, char* name );

/**
 * @brief   Get the number of access points found in the last scan.
 * @param   [out] ap_count - receives the count of scanned APs
 */
void wifi_mgmt_get_scan_result( uint16_t* ap_count );

/**
 * @brief   Check whether a Wi-Fi scan is currently in progress.
 * @return  true if a scan is active, otherwise false
 * @note    Thread-safe; safe to call from the Mongoose task while the Wi-Fi
 *          worker task updates scan state.
 */
bool wifi_mgmt_is_scan_active( void );

/**
 * @brief   Get the scan generation number.
 *
 * @details The generation counter increments each time a scan completes so
 *          callers can detect that a fresh scan snapshot is available.
 * @return  current scan generation value
 * @note    Thread-safe; safe to call from the Mongoose task while the Wi-Fi
 *          worker task updates scan state.
 */
uint32_t wifi_mgmt_get_scan_generation( void );

/**
 * @brief   Get the last measured RSSI of the current connection.
 * @return  RSSI value in dBm
 */
int wifi_mgmt_get_rssi( void );

/**
 * @brief   Check whether saved Wi-Fi credentials were loaded at startup.
 * @return  true if credentials were read from persistent storage, otherwise false
 */
bool wifi_mgmt_is_read_data( void );

/**
 * @brief   Check whether the Wi-Fi management module is running.
 * @return  true once startup has completed (state @c WIFI_APP_IDLE or beyond),
 *          false while the module is still initializing (@c WIFI_APP_INIT), is
 *          being stopped (@c WIFI_APP_DEINIT), or has not been started
 *          (@c WIFI_APP_DISABLE) — i.e. the worker task existing alone does
 *          not make the module "running".
 * @note    Prefer @c wifi_mgmt_wait_ready over polling this function when the
 *          caller needs to distinguish task creation from completed init.
 */
bool wifi_mgmt_is_running( void );

/**
 * @brief   Check whether the driver is in the idle state.
 * @return  true if idle, otherwise false
 */
bool wifi_mgmt_is_idle( void );

/**
 * @brief   Check whether the station is currently connected to an AP.
 * @return  true if connected and no reconnect is pending, otherwise false
 */
bool wifi_mgmt_is_connected( void );

/**
 * @brief   Check whether a scan can be started in the current state.
 * @return  true if scanning is allowed (idle or ready), otherwise false
 */
bool wifi_mgmt_is_ready_to_scan( void );

/**
 * @brief   Enable or disable modem power-save mode.
 * @param   [in] state - true to enable power save, false to disable
 */
void wifi_mgmt_power_save( bool state );

/**
 * @brief   Subscribe to a typed Wi-Fi management event.
 * @param   [in] event     - event to subscribe to
 * @param   [in] cb        - callback invoked when @p event fires
 * @param   [in] user_data - user context passed to @p cb
 * @return  true on success, false on null callback or duplicate registration
 */
bool wifi_mgmt_subscribe( wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void* user_data );

/**
 * @brief   Remove a previously registered typed event subscription.
 * @param   [in] event     - event the subscription belongs to
 * @param   [in] cb        - callback registered for @p event
 * @param   [in] user_data - user context supplied at subscription time
 * @return  true if the subscription was found and removed, otherwise false
 */
bool wifi_mgmt_unsubscribe( wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void* user_data );

/**
 * @brief   Register a callback of the successful connect.
 * @param   [in] cb - callback function pointer
 * @note    Compatibility wrapper around the typed event subscription,
 *          equivalent to subscribing to @c WIFI_MGMT_EVENT_CONNECTED.
 */
void wifi_mgmt_register_connect_cb( wifi_mgmt_callback_t cb );

/**
 * @brief   Register a callback of the disconnected event.
 * @param   [in] cb - callback function pointer
 * @note    Compatibility wrapper around the typed event subscription,
 *          equivalent to subscribing to @c WIFI_MGMT_EVENT_DISCONNECTED.
 */
void wifi_mgmt_register_disconnect_cb( wifi_mgmt_callback_t cb );

/**
 * @brief   Get the number of clients currently connected to the soft-AP.
 * @return  client count
 */
uint32_t wifi_mgmt_get_client_count( void );

/**
 * @brief   Get the station IPv4 address as a string.
 * @param   [out] ip  - buffer to receive the null-terminated address string
 * @param   [in]  len - size of @p ip buffer in bytes
 * @return  true if success, otherwise false
 */
bool wifi_mgmt_get_ip_addr( char* ip, size_t len );

/**
 * @brief   Get structured IP information for the current connection.
 * @param   [out] info - pointer to @c wifi_mgmt_ip_info_t to be filled
 * @return  true if success, otherwise false
 */
bool wifi_mgmt_get_ip_info( wifi_mgmt_ip_info_t* info );

/**
 * @brief   Get a snapshot of the last scan result as a structured list.
 * @param   [out] list - pointer to @c wifi_mgmt_ap_list_t to be filled
 * @return  true if success, otherwise false
 */
bool wifi_mgmt_get_access_points( wifi_mgmt_ap_list_t* list );

#endif
