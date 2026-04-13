/*
 * Wi-Fi HAL Mock Driver for Unit Tests
 *
 * Provides controllable mock implementations of all wifi_hal_driver.h
 * functions.  Test code can configure predefined scan results, control
 * connect/disconnect outcomes, and inject events via the public helpers
 * declared in wifi_hal_mock.h.
 */

#include "wifi_hal_mock.h"

#include <stdbool.h>
#include <string.h>

/* ---- internal state ------------------------------------------------------ */

static wifi_hal_mock_state_t g_mock = { 0 };

/* ---- mock control API ---------------------------------------------------- */

void wifi_hal_mock_reset( void )
{
  memset( &g_mock, 0, sizeof( g_mock ) );
}

void wifi_hal_mock_set_connect_result( osal_status_t result )
{
  g_mock.connect_result = result;
}

void wifi_hal_mock_set_scan_list( const wifi_hal_ap_record_t* list, uint16_t count )
{
  if ( count > WIFI_HAL_MOCK_MAX_AP )
  {
    count = WIFI_HAL_MOCK_MAX_AP;
  }

  g_mock.scan_count = count;
  if ( list && count > 0 )
  {
    memcpy( g_mock.scan_list, list, count * sizeof( wifi_hal_ap_record_t ) );
  }
}

void wifi_hal_mock_set_ip_info( const wifi_hal_ip_info_t* info )
{
  if ( info )
  {
    g_mock.ip_info = *info;
  }
}

void wifi_hal_mock_inject_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  if ( g_mock.event_cb )
  {
    g_mock.event_cb( event, data, g_mock.user_data );
  }
}

const wifi_hal_mock_state_t* wifi_hal_mock_get_state( void )
{
  return &g_mock;
}

/* ---- HAL implementation -------------------------------------------------- */

osal_status_t wifi_hal_init( const wifi_hal_init_t* init )
{
  if ( !init )
  {
    return OSAL_INVALID_POINTER;
  }

  g_mock.initialized = true;
  g_mock.event_cb    = init->event_cb;
  g_mock.user_data   = init->user_data;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit( void )
{
  g_mock.initialized = false;
  g_mock.started     = false;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  g_mock.mode    = mode;
  g_mock.started = true;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop( void )
{
  g_mock.started   = false;
  g_mock.connected = false;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  g_mock.sta_cfg = *config;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  g_mock.ap_cfg = *config;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  if ( g_mock.connect_result == OSAL_SUCCESS )
  {
    g_mock.connected = true;
  }
  return g_mock.connect_result;
}

osal_status_t wifi_hal_disconnect( void )
{
  g_mock.connected = false;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start_scan( bool block )
{
  (void) block;

  /* If a callback is registered, fire SCAN_DONE so the management layer
     picks up the results. */
  if ( g_mock.event_cb )
  {
    g_mock.event_cb( WIFI_HAL_EVT_SCAN_DONE, NULL, g_mock.user_data );
  }

  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !records || !in_out_count )
  {
    return OSAL_INVALID_POINTER;
  }

  uint16_t to_copy = g_mock.scan_count;
  if ( to_copy > *in_out_count )
  {
    to_copy = *in_out_count;
  }

  memcpy( records, g_mock.scan_list, to_copy * sizeof( wifi_hal_ap_record_t ) );
  *in_out_count = to_copy;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }

  *out_info = g_mock.ip_info;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_rssi( int* out_rssi )
{
  if ( !out_rssi )
  {
    return OSAL_INVALID_POINTER;
  }

  *out_rssi = -50;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  g_mock.power_save = enabled;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] )
{
  if ( !mac )
  {
    return OSAL_INVALID_POINTER;
  }

  /* Return a deterministic MAC for tests. */
  mac[0] = 0xAA;
  mac[1] = 0xBB;
  mac[2] = 0xCC;
  mac[3] = 0xDD;
  mac[4] = 0xEE;
  mac[5] = 0xFF;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count )
{
  if ( !out_client_count )
  {
    return OSAL_INVALID_POINTER;
  }

  *out_client_count = 0;
  return OSAL_SUCCESS;
}
