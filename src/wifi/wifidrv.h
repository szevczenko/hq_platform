/**
 *******************************************************************************
 * @file    wifidrv.h
 * @author  Dmytro Shevchenko
 * @brief   WiFi driver
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/
#ifndef _WIFI_DRV_H
#define _WIFI_DRV_H

#include "app_config.h"

/* Public macros -------------------------------------------------------------*/

#ifndef WIFI_AP_NAME
#define WIFI_AP_NAME "Bimbrownik"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "SuperTrudne1!-_"
#endif

#define MAX_SSID_SIZE     32
#define MAX_PASSWORD_SIZE 64

/** @brief Defines the access point's default IP address. Default: "10.10.0.1 */
#define DEFAULT_AP_IP "10.10.0.1"

/** @brief Defines the access point's gateway. This should be the same as your IP. Default: "10.10.0.1" */
#define DEFAULT_AP_GATEWAY "10.10.0.1"

/** @brief Defines the access point's netmask. Default: "255.255.255.0" */
#define DEFAULT_AP_NETMASK "255.255.255.0"

#ifndef WIFI_TEST_TASK
#define WIFI_TEST_TASK 0
#endif

/* Public types --------------------------------------------------------------*/
typedef enum
{
  T_WIFI_TYPE_SERVER = 1,
  T_WIFI_TYPE_CLIENT = 2,
  T_WIFI_TYPE_CLI_SER = 3
} wifi_type_t;

typedef struct
{
  char ssid[33];
  char password[64];
} wifiConData_t;

typedef void ( *wifi_drv_callback )( void );

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Init input device. Call this function before init
 * @param   [in] type - sta or access point 
 */
void wifiDrvSetWifiType( wifi_type_t type );

/**
 * @brief   Driver initialization. Before call this function call @c wifiDrvSetWifiType
 */
void wifiDrvInit( void );

/**
 * @brief   Driver stop. Use this function before Set wifi type
 */
void wifiDrvStop( void );

/**
 * @brief   Driver start. Use this after changing wifi type
 */
void wifiDrvStart( void );

/**
 * @brief   Set access point from scanned list returned by @c wifiDrvGetNameFromScannedList
 * @param   [in] num - index number in list. Similar as number from @c wifiDrvGetNameFromScannedList
 * @return  true if success, otherwise false
 */
bool wifiDrvSetFromAPList( uint8_t num );

/**
 * @brief   Set access point name
 * @param   [in] name - access point name
 * @param   [in] len - length of name
 * @return  true if success, otherwise false
 */
bool wifiDrvSetAPName( const char* name, size_t len );

/**
 * @brief   Set access point password
 * @param   [in] passwd - access point password
 * @param   [in] len - length of password
 * @return  true if success, otherwise false
 */
bool wifiDrvSetPassword( const char* passwd, size_t len );

/**
 * @brief   Start connecting process
 * @return  false if WiFi driver type in STA mode, otherwise true
 */
bool wifiDrvConnect( void );

/**
 * @brief   Start disconnecting process
 * @return  false if WiFi driver type in STA mode, otherwise true
 */
bool wifiDrvDisconnect( void );

/**
 * @brief   Check if WiFi driver ready to start wifi connect
 * @return  true if ready, otherwise false
 */
bool wifiDrvReadyToConnect( void );

/**
 * @brief   Check if WiFi driver in connecting state
 * @return  true if driver in connecting state, otherwise false
 */
bool wifiDrvTryingConnect( void );

/**
 * @brief   Start scanning devices
 * @return  true if success, otherwise false
 */
bool wifiDrvStartScan( void );

/**
 * @brief   Start scanning devices no blocking process
 * @return  true if success, otherwise false
 */
bool wifiDrvStartScanNoBlock( void );

/**
 * @brief   Get access point name
 * @param   [out] name - buffer where copy AP name
 * @return  true if success, otherwise false
 */
bool wifiDrvGetAPName( char* name );

/**
 * @brief   Get AP name from scanned list
 * @param   [in] number - index list number
 * @param   [out] name - buffer where copy AP name
 * @return  true if success, otherwise false
 */
bool wifiDrvGetNameFromScannedList( uint8_t number, char* name );

/**
 * @brief   Get count of scanned AP
 * @param   [out] ap_count - number of found AP
 */
void wifiDrvGetScanResult( uint16_t* ap_count );

/**
 * @brief   Get RSSI
 * @return  true RSSI
 */
int wifiDrvGetRssi( void );

/**
 * @brief   Check if in init state read wifi configuration from NVS
 * @return  true if read, otherwise false
 */
bool wifiDrvIsReadData( void );

/**
 * @brief   Check if driver in idle state
 * @return  true if in idle, otherwise false
 */
bool wifiDrvIsIdle( void );

/**
 * @brief   Check if driver is connected to AP
 * @return  true if connected, otherwise false
 */
bool wifiDrvIsConnected( void );

/**
 * @brief   Check if driver is ready to scan
 * @return  true if ready, otherwise false
 */
bool wifiDrvIsReadyToScan( void );

/**
 * @brief   Not implemented
 */
void wifiDrvPowerSave( bool state );

/**
 * @brief   Register connect callback
 */
void wifiDrvRegisterConnectCb( wifi_drv_callback cb );

/**
 * @brief   Register disconnect callback
 */
void wifiDrvRegisterDisconnectCb( wifi_drv_callback cb );

/**
 * @brief   Get client count connected to AP
 */
uint32_t wifiDrvGetClientCount( void );

/**
 * @brief   Get STA ip address
 * @param   [out] ip - buffer where copy string ip address
 * @param   [in] len - buffer size
 * @return  true if success, otherwise false
 */
bool wifiDrvGetIpAddr( char* ip, size_t len );

/**
 * @brief   Lock json buffer for not overwriting by wifi task
 * @note    Use before call @c wifiDrvGetAccessPointsListJson 
 *          or @c wifiDrvGetIpInfoJson
 * @return  true if success otherwise false
 */
bool wifiDrvLockJsonBuffer( size_t ms );

/**
 * @brief   Unlock json buffer for not overwriting by wifi task
 */
void wifiDrvUnlockJsonBuffer( void );

/**
 * @brief   Get access points list json
 * @note    Before uses this function should be locked mutex
 *          by calling @c wifiDrvLockJsonBuffer
 * @return  json string
 */
const char* wifiDrvGetAccessPointsListJson( void );

/**
 * @brief   Get ip info json
 * @note    Before uses this function should be locked mutex
 *          by calling @c wifiDrvLockJsonBuffer
 * @return  ip info string
 */
const char* wifiDrvGetIpInfoJson( void );

#endif