#include "wifi_hal_driver.h"

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "osal_log.h"
#include <string.h>

typedef struct
{
  bool initialized;
  bool started;
  wifi_hal_event_cb_t cb;
  void* cb_user_data;
  esp_netif_t* netif_sta;
  esp_netif_t* netif_ap;
  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t ap_cfg;
  uint32_t client_count;
} wifi_hal_ctx_t;

static wifi_hal_ctx_t g_wifi_hal_ctx = { 0 };

static osal_status_t _esp_to_status( esp_err_t err )
{
  if ( err == ESP_OK )
  {
    return OSAL_SUCCESS;
  }

  return OSAL_ERROR;
}

static void _emit_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  if ( g_wifi_hal_ctx.cb )
  {
    g_wifi_hal_ctx.cb( event, data, g_wifi_hal_ctx.cb_user_data );
  }
}

static void _wifi_event_handler( void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data )
{
  (void) arg;

  if ( event_base == WIFI_EVENT )
  {
    if ( event_id == WIFI_EVENT_STA_DISCONNECTED )
    {
      wifi_hal_event_data_t event = { 0 };
      wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
      event.disconnect_reason = disconnected->reason;
      _emit_event( WIFI_HAL_EVT_STA_DISCONNECTED, &event );
      return;
    }

    if ( event_id == WIFI_EVENT_SCAN_DONE )
    {
      _emit_event( WIFI_HAL_EVT_SCAN_DONE, NULL );
      return;
    }

    if ( event_id == WIFI_EVENT_AP_STACONNECTED )
    {
      g_wifi_hal_ctx.client_count++;
      _emit_event( WIFI_HAL_EVT_AP_CLIENT_CONNECTED, NULL );
      return;
    }

    if ( event_id == WIFI_EVENT_AP_STADISCONNECTED )
    {
      if ( g_wifi_hal_ctx.client_count > 0 )
      {
        g_wifi_hal_ctx.client_count--;
      }
      _emit_event( WIFI_HAL_EVT_AP_CLIENT_DISCONNECTED, NULL );
      return;
    }
  }

  if ( event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP )
  {
    wifi_hal_event_data_t event = { 0 };
    ip_event_got_ip_t* got_ip = (ip_event_got_ip_t*) event_data;
    esp_ip4addr_ntoa( &got_ip->ip_info.ip, event.ip_info.ip, sizeof( event.ip_info.ip ) );
    esp_ip4addr_ntoa( &got_ip->ip_info.netmask, event.ip_info.netmask, sizeof( event.ip_info.netmask ) );
    esp_ip4addr_ntoa( &got_ip->ip_info.gw, event.ip_info.gw, sizeof( event.ip_info.gw ) );
    _emit_event( WIFI_HAL_EVT_STA_GOT_IP, &event );
  }
}

static void _copy_sta_config( wifi_config_t* out, const wifi_hal_sta_config_t* in )
{
  memset( out, 0, sizeof( *out ) );
  strncpy( (char*) out->sta.ssid, in->ssid, sizeof( out->sta.ssid ) - 1 );
  strncpy( (char*) out->sta.password, in->password, sizeof( out->sta.password ) - 1 );
  out->sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  out->sta.pmf_cfg.capable = true;
}

static void _copy_ap_config( wifi_config_t* out, const wifi_hal_ap_config_t* in )
{
  memset( out, 0, sizeof( *out ) );
  strncpy( (char*) out->ap.ssid, in->ssid, sizeof( out->ap.ssid ) - 1 );
  strncpy( (char*) out->ap.password, in->password, sizeof( out->ap.password ) - 1 );
  out->ap.ssid_len = strlen( in->ssid );
  out->ap.max_connection = in->max_connection;
  out->ap.authmode = (wifi_auth_mode_t) in->authmode;
}

osal_status_t wifi_hal_init( const wifi_hal_init_t* init )
{
  if ( !init )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( g_wifi_hal_ctx.initialized )
  {
    return OSAL_SUCCESS;
  }

  g_wifi_hal_ctx.cb = init->event_cb;
  g_wifi_hal_ctx.cb_user_data = init->user_data;

  esp_err_t err = esp_netif_init();
  if ( err != ESP_OK && err != ESP_ERR_INVALID_STATE )
  {
    return _esp_to_status( err );
  }

  err = esp_event_loop_create_default();
  if ( err != ESP_OK && err != ESP_ERR_INVALID_STATE )
  {
    return _esp_to_status( err );
  }

  g_wifi_hal_ctx.netif_ap = esp_netif_create_default_wifi_ap();
  g_wifi_hal_ctx.netif_sta = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = false;
  err = esp_wifi_init( &cfg );
  if ( err != ESP_OK )
  {
    return _esp_to_status( err );
  }

  if ( init->ap_ip && init->ap_gateway && init->ap_netmask )
  {
    (void) esp_netif_dhcps_stop( g_wifi_hal_ctx.netif_ap );

    esp_netif_ip_info_t ap_ip_info = { 0 };
    inet_pton( AF_INET, init->ap_ip, &ap_ip_info.ip );
    inet_pton( AF_INET, init->ap_gateway, &ap_ip_info.gw );
    inet_pton( AF_INET, init->ap_netmask, &ap_ip_info.netmask );

    (void) esp_netif_set_ip_info( g_wifi_hal_ctx.netif_ap, &ap_ip_info );
    (void) esp_netif_dhcps_start( g_wifi_hal_ctx.netif_ap );
  }

  (void) esp_event_handler_register( WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL );
  (void) esp_event_handler_register( IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL );

  g_wifi_hal_ctx.initialized = true;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit( void )
{
  if ( !g_wifi_hal_ctx.initialized )
  {
    return OSAL_SUCCESS;
  }

  (void) esp_event_handler_unregister( WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler );
  (void) esp_event_handler_unregister( IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler );
  (void) esp_wifi_stop();
  (void) esp_wifi_deinit();

  memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  wifi_mode_t esp_mode = WIFI_MODE_STA;

  if ( mode == WIFI_HAL_MODE_AP )
  {
    esp_mode = WIFI_MODE_AP;
  }
  else if ( mode == WIFI_HAL_MODE_APSTA )
  {
    esp_mode = WIFI_MODE_APSTA;
  }

  if ( esp_wifi_set_mode( esp_mode ) != ESP_OK )
  {
    return OSAL_ERROR;
  }

  if ( mode == WIFI_HAL_MODE_AP || mode == WIFI_HAL_MODE_APSTA )
  {
    wifi_config_t ap_cfg;
    _copy_ap_config( &ap_cfg, &g_wifi_hal_ctx.ap_cfg );
    if ( esp_wifi_set_config( WIFI_IF_AP, &ap_cfg ) != ESP_OK )
    {
      return OSAL_ERROR;
    }
  }

  if ( mode == WIFI_HAL_MODE_STA || mode == WIFI_HAL_MODE_APSTA )
  {
    wifi_config_t sta_cfg;
    _copy_sta_config( &sta_cfg, &g_wifi_hal_ctx.sta_cfg );
    if ( esp_wifi_set_config( WIFI_IF_STA, &sta_cfg ) != ESP_OK )
    {
      return OSAL_ERROR;
    }
  }

  if ( esp_wifi_start() != ESP_OK )
  {
    return OSAL_ERROR;
  }

  g_wifi_hal_ctx.started = true;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop( void )
{
  if ( !g_wifi_hal_ctx.started )
  {
    return OSAL_SUCCESS;
  }

  if ( esp_wifi_stop() != ESP_OK )
  {
    return OSAL_ERROR;
  }

  g_wifi_hal_ctx.started = false;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  memset( &g_wifi_hal_ctx.sta_cfg, 0, sizeof( g_wifi_hal_ctx.sta_cfg ) );
  strncpy( g_wifi_hal_ctx.sta_cfg.ssid, config->ssid, WIFI_HAL_SSID_MAX_LEN );
  strncpy( g_wifi_hal_ctx.sta_cfg.password, config->password, WIFI_HAL_PASSWORD_MAX_LEN );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  memset( &g_wifi_hal_ctx.ap_cfg, 0, sizeof( g_wifi_hal_ctx.ap_cfg ) );
  strncpy( g_wifi_hal_ctx.ap_cfg.ssid, config->ssid, WIFI_HAL_SSID_MAX_LEN );
  strncpy( g_wifi_hal_ctx.ap_cfg.password, config->password, WIFI_HAL_PASSWORD_MAX_LEN );
  g_wifi_hal_ctx.ap_cfg.max_connection = config->max_connection;
  g_wifi_hal_ctx.ap_cfg.authmode = config->authmode;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  return _esp_to_status( esp_wifi_connect() );
}

osal_status_t wifi_hal_disconnect( void )
{
  return _esp_to_status( esp_wifi_disconnect() );
}

osal_status_t wifi_hal_start_scan( bool block )
{
  wifi_scan_config_t config = { 0 };
  return _esp_to_status( esp_wifi_scan_start( &config, block ) );
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !records || !in_out_count )
  {
    return OSAL_INVALID_POINTER;
  }

  wifi_ap_record_t ap_records[64] = { 0 };
  uint16_t count = *in_out_count;
  if ( count > 64 )
  {
    count = 64;
  }

  esp_err_t err = esp_wifi_scan_get_ap_records( &count, ap_records );
  if ( err != ESP_OK )
  {
    return OSAL_ERROR;
  }

  for ( uint16_t i = 0; i < count; ++i )
  {
    memset( &records[i], 0, sizeof( records[i] ) );
    strncpy( records[i].ssid, (const char*) ap_records[i].ssid, WIFI_HAL_SSID_MAX_LEN );
    records[i].channel = ap_records[i].primary;
    records[i].rssi = ap_records[i].rssi;
    records[i].authmode = ap_records[i].authmode;
  }

  *in_out_count = count;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }

  esp_netif_ip_info_t ip_info = { 0 };
  esp_err_t err = esp_netif_get_ip_info( g_wifi_hal_ctx.netif_sta, &ip_info );
  if ( err != ESP_OK )
  {
    return OSAL_ERROR;
  }

  esp_ip4addr_ntoa( &ip_info.ip, out_info->ip, sizeof( out_info->ip ) );
  esp_ip4addr_ntoa( &ip_info.netmask, out_info->netmask, sizeof( out_info->netmask ) );
  esp_ip4addr_ntoa( &ip_info.gw, out_info->gw, sizeof( out_info->gw ) );

  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_rssi( int* out_rssi )
{
  if ( !out_rssi )
  {
    return OSAL_INVALID_POINTER;
  }

  wifi_ap_record_t ap_info = { 0 };
  esp_err_t err = esp_wifi_sta_get_ap_info( &ap_info );
  if ( err != ESP_OK )
  {
    return OSAL_ERROR;
  }

  *out_rssi = ap_info.rssi;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  return _esp_to_status( esp_wifi_set_ps( enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE ) );
}

osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] )
{
  if ( !mac )
  {
    return OSAL_INVALID_POINTER;
  }

  return _esp_to_status( esp_efuse_mac_get_default( mac ) );
}

osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count )
{
  if ( !out_client_count )
  {
    return OSAL_INVALID_POINTER;
  }

  *out_client_count = g_wifi_hal_ctx.client_count;
  return OSAL_SUCCESS;
}
