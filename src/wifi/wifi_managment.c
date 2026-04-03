#include "wifi_managment.h"

#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_task.h"
#include "wifi_config.h"
#include "wifi_hal_driver.h"
#include <stdio.h>
#include <string.h>

#define CALLBACKS_LIST_SIZE        8
#define DEFAULT_SCAN_LIST_SIZE     WIFI_DRV_MAX_SCAN_AP

#ifndef NORMALPRIO
#define NORMALPRIO 1u
#endif

typedef enum
{
  WIFI_APP_DISABLE = 0,
  WIFI_APP_INIT,
  WIFI_APP_IDLE,
  WIFI_APP_CONNECT,
  WIFI_APP_WAIT_CONNECT,
  WIFI_APP_START,
  WIFI_APP_STOP,
  WIFI_APP_READY,
  WIFI_APP_DEINIT,
  WIFI_APP_TOP,
} wifi_app_status_t;

typedef enum
{
  UPDATE_CONNECTION_OK    = 0,
  UPDATE_FAILED_ATTEMPT   = 1,
  UPDATE_USER_DISCONNECT  = 2,
  UPDATE_LOST_CONNECTION  = 3,
} update_reason_code_t;

typedef struct
{
  wifi_mgmt_callback_t callbacks[CALLBACKS_LIST_SIZE];
  size_t               size;
} callback_list_t;

typedef struct
{
  wifi_app_status_t state;
  bool              is_started;
  bool              is_power_save;
  bool              connected;
  bool              disconnect_req;
  bool              connect_req;
  bool              read_wifi_data;
  bool              is_scanned;
  uint32_t          connect_attempts;
  uint32_t          reason_disconnect;
  uint32_t          client_cnt;

  wifi_hal_ap_record_t  scan_list[DEFAULT_SCAN_LIST_SIZE];
  uint16_t              scanned_ap_num;
  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t  ap_cfg;
  wifi_mgmt_con_data_t  saved_data;
  wifi_mgmt_ip_info_t   ip_info;
  int                   rssi;
  char                  ip_addr[16];

  osal_bin_sem_id_t ip_sem;

  callback_list_t on_connect_cb;
  callback_list_t on_disconnect_cb;

  /* Multi-credential support */
  wifi_config_list_t  config_list;
  uint8_t             current_cred_nb;
  bool                config_loaded;
} wifi_ctx_t;

static wifi_ctx_t g_ctx = {
  .ap_cfg = {
    .ssid           = WIFI_AP_NAME,
    .password       = WIFI_AP_PASSWORD,
    .max_connection = 2,
    .authmode       = 4,
  },
};

static uint8_t       g_wifi_type  = T_WIFI_TYPE_CLIENT;
static osal_task_id_t g_wifi_task_id;

static void _init_list( callback_list_t* list )
{
  memset( list, 0, sizeof( *list ) );
}

static void _add_to_list( callback_list_t* list, wifi_mgmt_callback_t cb )
{
  if ( !list || !cb || list->size >= CALLBACKS_LIST_SIZE )
  {
    return;
  }

  list->callbacks[list->size] = cb;
  list->size++;
}

static void _run_from_cb_list( callback_list_t* list )
{
  if ( !list )
  {
    return;
  }

  for ( size_t i = 0; i < list->size; ++i )
  {
    list->callbacks[i]();
  }
}

static bool _lock_ip( uint32_t timeout_ms )
{
  return osal_bin_sem_timed_wait( g_ctx.ip_sem, timeout_ms ) == OSAL_SUCCESS;
}

static void _unlock_ip( void )
{
  (void) osal_bin_sem_give( g_ctx.ip_sem );
}

static void _safe_update_sta_ip_string( const char* ip )
{
  if ( !ip )
  {
    return;
  }

  if ( _lock_ip( OSAL_MAX_DELAY ) )
  {
    strncpy( g_ctx.ip_addr, ip, sizeof( g_ctx.ip_addr ) - 1 );
    _unlock_ip();
  }
}

static void _update_ip_info( update_reason_code_t reason )
{
  memset( &g_ctx.ip_info, 0, sizeof( g_ctx.ip_info ) );
  strncpy( g_ctx.ip_info.ssid, g_ctx.sta_cfg.ssid, sizeof( g_ctx.ip_info.ssid ) - 1 );
  g_ctx.ip_info.urc = (int) reason;

  if ( reason != UPDATE_CONNECTION_OK )
  {
    return;
  }

  wifi_hal_ip_info_t hal_ip = { 0 };
  if ( wifi_hal_get_sta_ip_info( &hal_ip ) != OSAL_SUCCESS )
  {
    return;
  }

  strncpy( g_ctx.ip_info.ip, hal_ip.ip, sizeof( g_ctx.ip_info.ip ) - 1 );
  strncpy( g_ctx.ip_info.netmask, hal_ip.netmask, sizeof( g_ctx.ip_info.netmask ) - 1 );
  strncpy( g_ctx.ip_info.gw, hal_ip.gw, sizeof( g_ctx.ip_info.gw ) - 1 );
}

static const char* _state_name( wifi_app_status_t s )
{
  static const char* names[] = {
    "DISABLE", "INIT", "IDLE", "CONNECT", "WAIT_CONNECT",
    "START", "STOP", "READY", "DEINIT"
  };
  return ( s < WIFI_APP_TOP ) ? names[s] : "UNKNOWN";
}

static void _change_state( wifi_app_status_t new_state )
{
  if ( new_state < WIFI_APP_TOP )
  {
    osal_log_debug( "[wifi] state %s -> %s", _state_name( g_ctx.state ), _state_name( new_state ) );
    g_ctx.state = new_state;
  }
}

static void _hal_event_cb( wifi_hal_event_t              event,
                           const wifi_hal_event_data_t*  data,
                           void*                         user_data )
{
  (void) user_data;

  switch ( event )
  {
    case WIFI_HAL_EVT_STA_DISCONNECTED:
      g_ctx.connected        = false;
      g_ctx.reason_disconnect = data ? data->disconnect_reason : 0;
      _safe_update_sta_ip_string( "0.0.0.0" );
      break;

    case WIFI_HAL_EVT_STA_GOT_IP:
      g_ctx.connected = true;
      if ( data )
      {
        _safe_update_sta_ip_string( data->ip_info.ip );
      }
      break;

    case WIFI_HAL_EVT_SCAN_DONE:
      {
        uint16_t ap_num = DEFAULT_SCAN_LIST_SIZE;
        if ( wifi_hal_get_scanned_ap( g_ctx.scan_list, &ap_num ) == OSAL_SUCCESS )
        {
          g_ctx.scanned_ap_num = ap_num;
        }
        g_ctx.is_scanned = false;
      }
      break;

    case WIFI_HAL_EVT_AP_CLIENT_CONNECTED:
    case WIFI_HAL_EVT_AP_CLIENT_DISCONNECTED:
      {
        uint32_t clients = 0;
        if ( wifi_hal_get_client_count( &clients ) == OSAL_SUCCESS )
        {
          g_ctx.client_cnt = clients;
        }
      }
      break;
  }
}

static void _load_saved_config( void )
{
  memset( &g_ctx.config_list, 0, sizeof( g_ctx.config_list ) );
  g_ctx.config_loaded = false;

  if ( wifi_config_load( &g_ctx.config_list ) != OSAL_SUCCESS )
  {
    return;
  }

  if ( g_ctx.config_list.count == 0 )
  {
    return;
  }

  g_ctx.config_loaded = true;

  /* Pick the last_use credential. */
  wifi_config_entry_t entry = { 0 };
  if ( !wifi_config_get_by_nb( &g_ctx.config_list, g_ctx.config_list.last_use, &entry ) )
  {
    /* last_use nb not found — fall back to first entry. */
    entry = g_ctx.config_list.entries[0];
  }

  g_ctx.current_cred_nb = entry.nb;
  strncpy( (char*) g_ctx.sta_cfg.ssid,    entry.ssid,     sizeof( g_ctx.sta_cfg.ssid ) - 1 );
  strncpy( (char*) g_ctx.sta_cfg.password, entry.password, sizeof( g_ctx.sta_cfg.password ) - 1 );
  strncpy( g_ctx.saved_data.ssid,          entry.ssid,     sizeof( g_ctx.saved_data.ssid ) - 1 );
  strncpy( g_ctx.saved_data.password,      entry.password, sizeof( g_ctx.saved_data.password ) - 1 );
  g_ctx.read_wifi_data = true;
}

static void _save_current_sta_config( void )
{
  const char* ssid = (const char*) g_ctx.sta_cfg.ssid;
  const char* pass = (const char*) g_ctx.sta_cfg.password;

  if ( wifi_config_add_credential( &g_ctx.config_list, ssid, pass ) != OSAL_SUCCESS )
  {
    return;
  }

  g_ctx.current_cred_nb  = g_ctx.config_list.last_use;
  g_ctx.config_loaded    = true;

  if ( wifi_config_save( &g_ctx.config_list ) == OSAL_SUCCESS )
  {
    strncpy( g_ctx.saved_data.ssid,     ssid,  sizeof( g_ctx.saved_data.ssid ) - 1 );
    strncpy( g_ctx.saved_data.password, pass,  sizeof( g_ctx.saved_data.password ) - 1 );
  }
}

static void _setup_ap_name_with_mac( void )
{
  uint8_t mac[6] = { 0 };
  if ( wifi_hal_get_default_mac( mac ) != OSAL_SUCCESS )
  {
    return;
  }

  char ssid[sizeof( g_ctx.ap_cfg.ssid )] = { 0 };
  snprintf( ssid,
            sizeof( ssid ),
            "%s:%02x:%02x:%02x:%02x:%02x:%02x",
            WIFI_AP_NAME,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] );

  strncpy( g_ctx.ap_cfg.ssid, ssid, sizeof( g_ctx.ap_cfg.ssid ) - 1 );
}

static osal_status_t _start_mode( wifi_hal_mode_t mode )
{
  if ( g_ctx.is_started )
  {
    return OSAL_SUCCESS;
  }

  osal_status_t st = wifi_hal_set_ap_config( &g_ctx.ap_cfg );
  if ( st != OSAL_SUCCESS )
  {
    return st;
  }

  st = wifi_hal_set_sta_config( &g_ctx.sta_cfg );
  if ( st != OSAL_SUCCESS )
  {
    return st;
  }

  st = wifi_hal_start( mode );
  if ( st == OSAL_SUCCESS )
  {
    g_ctx.is_started = true;
    (void) wifi_hal_set_power_save( false );
  }

  return st;
}

static void _state_init( void )
{
  osal_log_debug( "[wifi] _state_init enter" );
  g_ctx.is_scanned = false;

  wifi_hal_init_t init = {
    .ap_ip      = DEFAULT_AP_IP,
    .ap_gateway = DEFAULT_AP_GATEWAY,
    .ap_netmask = DEFAULT_AP_NETMASK,
    .event_cb   = _hal_event_cb,
    .user_data  = NULL,
  };

  if ( wifi_hal_init( &init ) != OSAL_SUCCESS )
  {
    _change_state( WIFI_APP_DISABLE );
    return;
  }

  _setup_ap_name_with_mac();

  if ( g_wifi_type == T_WIFI_TYPE_SERVER )
  {
    (void) _start_mode( WIFI_HAL_MODE_AP );
  }
  else if ( g_wifi_type == T_WIFI_TYPE_CLIENT )
  {
    (void) _start_mode( WIFI_HAL_MODE_STA );
  }
  else
  {
    (void) _start_mode( WIFI_HAL_MODE_APSTA );
  }

  _update_ip_info( UPDATE_LOST_CONNECTION );
  _change_state( WIFI_APP_IDLE );
}

static void _state_idle( void )
{
  if ( g_wifi_type == T_WIFI_TYPE_SERVER )
  {
    g_ctx.connected = true;
    _change_state( WIFI_APP_START );
    return;
  }

  if ( g_ctx.connect_req )
  {
    _change_state( WIFI_APP_CONNECT );
  }
  else
  {
    (void) osal_task_delay_ms( 50 );
  }
}

static void _try_next_credential( void )
{
  if ( !g_ctx.config_loaded || g_ctx.config_list.count <= 1 )
  {
    return;
  }

  wifi_config_entry_t next = { 0 };
  if ( !wifi_config_get_next( &g_ctx.config_list, g_ctx.current_cred_nb, &next ) )
  {
    return;
  }

  g_ctx.current_cred_nb = next.nb;
  memset( g_ctx.sta_cfg.ssid, 0, sizeof( g_ctx.sta_cfg.ssid ) );
  memset( g_ctx.sta_cfg.password, 0, sizeof( g_ctx.sta_cfg.password ) );
  strncpy( (char*) g_ctx.sta_cfg.ssid,    next.ssid,     sizeof( g_ctx.sta_cfg.ssid ) - 1 );
  strncpy( (char*) g_ctx.sta_cfg.password, next.password, sizeof( g_ctx.sta_cfg.password ) - 1 );
}

static void _state_connect( void )
{
  osal_log_debug( "[wifi] _state_connect enter, attempts=%u", g_ctx.connect_attempts );
  if ( !g_ctx.is_started )
  {
    if ( g_wifi_type == T_WIFI_TYPE_CLIENT )
    {
      (void) _start_mode( WIFI_HAL_MODE_STA );
    }
    else
    {
      (void) _start_mode( WIFI_HAL_MODE_APSTA );
    }
  }

  (void) wifi_hal_set_sta_config( &g_ctx.sta_cfg );

  if ( wifi_hal_connect() == OSAL_SUCCESS )
  {
    _change_state( WIFI_APP_WAIT_CONNECT );
  }
  else
  {
    g_ctx.connect_attempts++;
    if ( g_ctx.connect_attempts > 3 )
    {
      _try_next_credential();
      g_ctx.connect_attempts = 0;
      g_ctx.connect_req      = false;
      g_ctx.is_started       = false;
      (void) wifi_hal_stop();
      _change_state( WIFI_APP_STOP );
      return;
    }

    _change_state( WIFI_APP_IDLE );
  }
}

static void _state_wait_connect( void )
{
  if ( g_ctx.connected )
  {
    g_ctx.disconnect_req    = false;
    g_ctx.connect_req       = false;
    g_ctx.connect_attempts  = 0;
    _save_current_sta_config();
    _change_state( WIFI_APP_START );
    return;
  }

  if ( g_ctx.connect_attempts > 30 )
  {
    _try_next_credential();
    g_ctx.connect_req      = false;
    g_ctx.connect_attempts = 0;
    _change_state( WIFI_APP_STOP );
    return;
  }

  g_ctx.connect_attempts++;
  (void) osal_task_delay_ms( 250 );
}

static void _state_start( void )
{
  osal_log_debug( "[wifi] _state_start enter, connected=%d", g_ctx.connected );
  _update_ip_info( UPDATE_CONNECTION_OK );
  _run_from_cb_list( &g_ctx.on_connect_cb );
  _change_state( WIFI_APP_READY );
}

static void _state_stop( void )
{
  osal_log_debug( "[wifi] _state_stop enter" );
  _run_from_cb_list( &g_ctx.on_disconnect_cb );

  if ( g_ctx.disconnect_req )
  {
    g_ctx.disconnect_req = false;
    if ( g_ctx.reason_disconnect != 0 )
    {
      _update_ip_info( UPDATE_FAILED_ATTEMPT );
      g_ctx.reason_disconnect = 0;
    }
    else
    {
      _update_ip_info( UPDATE_USER_DISCONNECT );
    }
  }
  else if ( !g_ctx.connected )
  {
    _update_ip_info( UPDATE_LOST_CONNECTION );
  }

  (void) wifi_hal_disconnect();
  g_ctx.connected = false;
  _change_state( WIFI_APP_IDLE );
}

static void _state_ready( void )
{
  if ( g_ctx.disconnect_req || !g_ctx.connected || g_ctx.connect_req )
  {
    _change_state( WIFI_APP_STOP );
    return;
  }

  (void) wifi_hal_get_sta_rssi( &g_ctx.rssi );
  (void) osal_task_delay_ms( 200 );
}

static void _state_deinit( void )
{
  g_ctx.is_started        = false;
  g_ctx.is_power_save     = false;
  g_ctx.connected         = false;
  g_ctx.disconnect_req    = false;
  g_ctx.connect_req       = false;
  g_ctx.connect_attempts  = 0;
  g_ctx.reason_disconnect = 0;
  g_ctx.client_cnt        = 0;
  g_ctx.scanned_ap_num    = 0;
  g_ctx.is_scanned        = false;

  _update_ip_info( UPDATE_LOST_CONNECTION );

  (void) wifi_hal_stop();
  (void) wifi_hal_deinit();
  _change_state( WIFI_APP_DISABLE );
}

static void _wifi_event_task( void* arg )
{
  (void) arg;
  osal_log_debug( "[wifi] event task started" );

  while ( true )
  {
    switch ( g_ctx.state )
    {
      case WIFI_APP_INIT:
        _state_init();
        break;

      case WIFI_APP_IDLE:
        _state_idle();
        break;

      case WIFI_APP_CONNECT:
        _state_connect();
        break;

      case WIFI_APP_WAIT_CONNECT:
        _state_wait_connect();
        break;

      case WIFI_APP_START:
        _state_start();
        break;

      case WIFI_APP_STOP:
        _state_stop();
        break;

      case WIFI_APP_READY:
        _state_ready();
        break;

      case WIFI_APP_DEINIT:
        _state_deinit();
        break;

      case WIFI_APP_DISABLE:
      default:
        (void) osal_task_delay_ms( 100 );
        break;
    }
  }
}

void wifi_mgmt_init( void )
{
  _init_list( &g_ctx.on_connect_cb );
  _init_list( &g_ctx.on_disconnect_cb );

  (void) osal_bin_sem_create( &g_ctx.ip_sem, "wifi_ip", OSAL_SEM_FULL );

  _load_saved_config();
  _update_ip_info( UPDATE_LOST_CONNECTION );
  _safe_update_sta_ip_string( "0.0.0.0" );

  osal_task_attr_t attr;
  (void) osal_task_attributes_init( &attr );
  osal_status_t task_rc = osal_task_create( &g_wifi_task_id,
                           "wifi_task",
                           _wifi_event_task,
                           NULL,
                           NULL,
                           OSAL_TASK_MIN_STACK_SIZE,
                           NORMALPRIO,
                           &attr );
  osal_log_info( "[wifi] task create rc=%d, stack=%u, prio=%u",
                 (int) task_rc, (unsigned) OSAL_TASK_MIN_STACK_SIZE, (unsigned) NORMALPRIO );
  if ( task_rc != OSAL_SUCCESS )
  {
    osal_log_error( "[wifi] FAILED to create wifi_task (rc=%d)", (int) task_rc );
  }
}

void wifi_mgmt_set_wifi_type( wifi_type_t type )
{
  g_wifi_type = type;
}

void wifi_mgmt_stop( void )
{
  if ( g_ctx.state != WIFI_APP_DISABLE )
  {
    _change_state( WIFI_APP_DEINIT );
  }

  for ( int i = 0; i < 20 && g_ctx.state != WIFI_APP_DISABLE; ++i )
  {
    (void) osal_task_delay_ms( 100 );
  }
}

void wifi_mgmt_start( void )
{
  osal_log_debug( "[wifi] wifi_mgmt_start called, current state=%s", _state_name( g_ctx.state ) );
  if ( g_ctx.state == WIFI_APP_DISABLE )
  {
    _change_state( WIFI_APP_INIT );
  }
}

static bool _scan( bool block )
{
  if ( g_wifi_type == T_WIFI_TYPE_SERVER || g_ctx.is_scanned )
  {
    return false;
  }

  if ( g_ctx.state != WIFI_APP_IDLE && g_ctx.state != WIFI_APP_READY )
  {
    return false;
  }

  g_ctx.is_scanned = true;
  if ( wifi_hal_start_scan( block ) != OSAL_SUCCESS )
  {
    g_ctx.is_scanned = false;
    return false;
  }

  return true;
}

bool wifi_mgmt_start_scan( void )
{
  return _scan( true );
}

bool wifi_mgmt_start_scan_no_block( void )
{
  return _scan( false );
}

void wifi_mgmt_get_scan_result( uint16_t* ap_count )
{
  if ( ap_count )
  {
    *ap_count = g_ctx.scanned_ap_num;
  }
}

bool wifi_mgmt_get_name_from_scanned_list( uint8_t number, char* name )
{
  if ( !name || number >= g_ctx.scanned_ap_num )
  {
    return false;
  }

  strncpy( name, g_ctx.scan_list[number].ssid, MAX_SSID_SIZE );
  return true;
}

bool wifi_mgmt_set_from_ap_list( uint8_t num )
{
  if ( num >= g_ctx.scanned_ap_num )
  {
    return false;
  }

  memset( g_ctx.sta_cfg.ssid, 0, sizeof( g_ctx.sta_cfg.ssid ) );
  strncpy( g_ctx.sta_cfg.ssid, g_ctx.scan_list[num].ssid, sizeof( g_ctx.sta_cfg.ssid ) - 1 );
  return true;
}

bool wifi_mgmt_set_ap_name( const char* name, size_t len )
{
  if ( !name || len == 0 || len >= sizeof( g_ctx.sta_cfg.ssid ) )
  {
    return false;
  }

  memset( g_ctx.sta_cfg.ssid, 0, sizeof( g_ctx.sta_cfg.ssid ) );
  memcpy( g_ctx.sta_cfg.ssid, name, len );
  return true;
}

bool wifi_mgmt_get_ap_name( char* name )
{
  if ( !name )
  {
    return false;
  }

  strncpy( name, g_ctx.sta_cfg.ssid, MAX_SSID_SIZE );
  return true;
}

bool wifi_mgmt_set_password( const char* passwd, size_t len )
{
  if ( !passwd || len >= sizeof( g_ctx.sta_cfg.password ) )
  {
    return false;
  }

  memset( g_ctx.sta_cfg.password, 0, sizeof( g_ctx.sta_cfg.password ) );
  memcpy( g_ctx.sta_cfg.password, passwd, len );
  return true;
}

bool wifi_mgmt_connect( void )
{
  if ( g_wifi_type == T_WIFI_TYPE_SERVER )
  {
    return false;
  }

  g_ctx.connect_req = true;
  return true;
}

bool wifi_mgmt_disconnect( void )
{
  if ( g_wifi_type == T_WIFI_TYPE_SERVER )
  {
    return false;
  }

  g_ctx.disconnect_req = true;
  return true;
}

bool wifi_mgmt_is_connected( void )
{
  return g_ctx.connected && !g_ctx.connect_req;
}

bool wifi_mgmt_is_ready_to_scan( void )
{
  return g_ctx.state == WIFI_APP_IDLE || g_ctx.state == WIFI_APP_READY;
}

bool wifi_mgmt_ready_to_connect( void )
{
  return g_ctx.state == WIFI_APP_IDLE;
}

bool wifi_mgmt_trying_connect( void )
{
  return g_ctx.state == WIFI_APP_WAIT_CONNECT || g_ctx.state == WIFI_APP_CONNECT;
}

bool wifi_mgmt_is_read_data( void )
{
  return g_ctx.read_wifi_data;
}

bool wifi_mgmt_is_idle( void )
{
  return g_ctx.state == WIFI_APP_IDLE;
}

int wifi_mgmt_get_rssi( void )
{
  return g_ctx.rssi;
}

void wifi_mgmt_power_save( bool state )
{
  g_ctx.is_power_save = state;
  (void) wifi_hal_set_power_save( state );
}

void wifi_mgmt_register_connect_cb( wifi_mgmt_callback_t cb )
{
  _add_to_list( &g_ctx.on_connect_cb, cb );
}

void wifi_mgmt_register_disconnect_cb( wifi_mgmt_callback_t cb )
{
  _add_to_list( &g_ctx.on_disconnect_cb, cb );
}

uint32_t wifi_mgmt_get_client_count( void )
{
  uint32_t cnt = g_ctx.client_cnt;
  (void) wifi_hal_get_client_count( &cnt );
  g_ctx.client_cnt = cnt;
  return g_ctx.client_cnt;
}

bool wifi_mgmt_get_ip_addr( char* ip, size_t len )
{
  if ( !ip || len == 0 )
  {
    return false;
  }

  if ( !_lock_ip( OSAL_MAX_DELAY ) )
  {
    return false;
  }

  size_t needed = strlen( g_ctx.ip_addr ) + 1;
  if ( len < needed )
  {
    _unlock_ip();
    return false;
  }

  memset( ip, 0, len );
  strncpy( ip, g_ctx.ip_addr, len - 1 );
  _unlock_ip();
  return true;
}

bool wifi_mgmt_get_ip_info( wifi_mgmt_ip_info_t* info )
{
  if ( !info )
  {
    return false;
  }

  *info = g_ctx.ip_info;
  return true;
}

bool wifi_mgmt_get_access_points( wifi_mgmt_ap_list_t* list )
{
  if ( !list )
  {
    return false;
  }

  memset( list, 0, sizeof( *list ) );
  list->count = g_ctx.scanned_ap_num;
  if ( list->count > WIFI_DRV_MAX_SCAN_AP )
  {
    list->count = WIFI_DRV_MAX_SCAN_AP;
  }

  for ( uint16_t i = 0; i < list->count; ++i )
  {
    strncpy( list->items[i].ssid, g_ctx.scan_list[i].ssid, MAX_SSID_SIZE );
    list->items[i].chan = g_ctx.scan_list[i].channel;
    list->items[i].rssi = g_ctx.scan_list[i].rssi;
    list->items[i].auth = g_ctx.scan_list[i].authmode;
  }

  return true;
}
