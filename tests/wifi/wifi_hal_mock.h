/*
 * Wi-Fi HAL Mock — public control interface for unit tests
 */

#ifndef WIFI_HAL_MOCK_H
#define WIFI_HAL_MOCK_H

#include "wifi_hal_driver.h"

#define WIFI_HAL_MOCK_MAX_AP 32

typedef struct
{
  bool                  initialized;
  bool                  started;
  bool                  connected;
  bool                  power_save;
  wifi_hal_mode_t       mode;

  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t  ap_cfg;

  wifi_hal_ap_record_t  scan_list[WIFI_HAL_MOCK_MAX_AP];
  uint16_t              scan_count;

  wifi_hal_ip_info_t    ip_info;

  osal_status_t         connect_result;

  wifi_hal_event_cb_t   event_cb;
  void*                 user_data;
} wifi_hal_mock_state_t;

/* Reset all mock state to defaults. */
void wifi_hal_mock_reset( void );

/* Configure the return value of wifi_hal_connect(). */
void wifi_hal_mock_set_connect_result( osal_status_t result );

/* Set the predefined AP list returned by wifi_hal_get_scanned_ap(). */
void wifi_hal_mock_set_scan_list( const wifi_hal_ap_record_t* list, uint16_t count );

/* Set the IP info returned by wifi_hal_get_sta_ip_info(). */
void wifi_hal_mock_set_ip_info( const wifi_hal_ip_info_t* info );

/* Fire an event through the registered callback (simulates HAL events). */
void wifi_hal_mock_inject_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data );

/* Get read-only pointer to internal mock state for assertions. */
const wifi_hal_mock_state_t* wifi_hal_mock_get_state( void );

#endif
