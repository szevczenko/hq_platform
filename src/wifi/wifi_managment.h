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

/** @brief Prototype for connect/disconnect event callbacks. */
typedef void ( *wifi_mgmt_callback_t )( void );

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Set the operating role before calling @c wifi_mgmt_init.
 * @param   [in] type - @c T_WIFI_TYPE_SERVER, @c T_WIFI_TYPE_CLIENT,
 *                      or @c T_WIFI_TYPE_CLI_SER
 */
void wifi_mgmt_set_wifi_type( wifi_type_t type );

/**
 * @brief   Initialize the Wi-Fi management module and spawn the worker task.
 * @note    Call @c wifi_mgmt_set_wifi_type before this function.
 */
void wifi_mgmt_init( void );

/**
 * @brief   Stop the Wi-Fi driver and release HAL resources.
 * @note    Blocks until the internal task reaches the disabled state.
 */
void wifi_mgmt_stop( void );

/**
 * @brief   Start the Wi-Fi management state machine.
 * @note    Has no effect if already started.
 */
void wifi_mgmt_start( void );

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
 * @brief   Register a callback invoked after a successful connection.
 * @param   [in] cb - callback function pointer
 */
void wifi_mgmt_register_connect_cb( wifi_mgmt_callback_t cb );

/**
 * @brief   Register a callback invoked after disconnection.
 * @param   [in] cb - callback function pointer
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
