#include "wifi_managment.h"

#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "wifi_config.h"
#include "wifi_hal_driver.h"
#include <stdio.h>
#include <string.h>

#define CALLBACKS_LIST_SIZE        8
#define DEFAULT_SCAN_LIST_SIZE     WIFI_DRV_MAX_SCAN_AP
/* Blocking scan timeout (ms) when callers expect results synchronously. */
#define WIFI_SCAN_TIMEOUT_MS 10000

/* Maximum number of typed event subscriptions held at once. */
#define EVENT_SUBSCRIPTIONS_SIZE 32

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
  wifi_mgmt_event_t    event;
  wifi_mgmt_event_cb_t cb;
  void*                user_data;
} event_subscription_t;

typedef struct
{
  event_subscription_t items[EVENT_SUBSCRIPTIONS_SIZE];
  size_t               size;
} event_sub_list_t;

typedef struct
{
  wifi_app_status_t state;
  bool              is_started;
  bool              is_power_save;
  bool              connected;
  bool              disconnect_req;
  bool              connect_req;
  wifi_type_t       mode_req; /**< 0 = none, otherwise a pending async mode.   */
  bool              read_wifi_data;
  bool              scan_in_progress;
  uint32_t          scan_generation;
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
  osal_bin_sem_id_t scan_sem;

  callback_list_t on_connect_cb;
  callback_list_t on_disconnect_cb;
  event_sub_list_t event_subs;
  osal_mutex_id_t  event_mutex;

  /* Guards scan records, scan state, IP info and connection state. Callers
   * must not hold this mutex while dispatching events or running callbacks. */
  osal_mutex_id_t  state_mutex;

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

static bool _event_subscribe( wifi_mgmt_event_t    event,
                              wifi_mgmt_event_cb_t cb,
                              void*                user_data )
{
  if ( !cb )
  {
    return false;
  }

  (void) osal_mutex_take( g_ctx.event_mutex );

  /* Reject duplicate registrations: same event, callback and context. */
  for ( size_t i = 0; i < g_ctx.event_subs.size; ++i )
  {
    event_subscription_t* s = &g_ctx.event_subs.items[i];
    if ( s->event == event && s->cb == cb && s->user_data == user_data )
    {
      (void) osal_mutex_give( g_ctx.event_mutex );
      return false;
    }
  }

  if ( g_ctx.event_subs.size >= EVENT_SUBSCRIPTIONS_SIZE )
  {
    (void) osal_mutex_give( g_ctx.event_mutex );
    return false;
  }

  event_subscription_t* s = &g_ctx.event_subs.items[g_ctx.event_subs.size++];
  s->event     = event;
  s->cb        = cb;
  s->user_data = user_data;

  (void) osal_mutex_give( g_ctx.event_mutex );
  return true;
}

static bool _event_unsubscribe( wifi_mgmt_event_t    event,
                               wifi_mgmt_event_cb_t cb,
                               void*                user_data )
{
  if ( !cb )
  {
    return false;
  }

  (void) osal_mutex_take( g_ctx.event_mutex );

  for ( size_t i = 0; i < g_ctx.event_subs.size; ++i )
  {
    event_subscription_t* s = &g_ctx.event_subs.items[i];
    if ( s->event == event && s->cb == cb && s->user_data == user_data )
    {
      /* Shift remaining subscriptions down to fill the gap. */
      for ( size_t j = i + 1; j < g_ctx.event_subs.size; ++j )
      {
        g_ctx.event_subs.items[j - 1] = g_ctx.event_subs.items[j];
      }
      g_ctx.event_subs.size--;
      (void) osal_mutex_give( g_ctx.event_mutex );
      return true;
    }
  }

  (void) osal_mutex_give( g_ctx.event_mutex );
  return false;
}

static void _event_dispatch( wifi_mgmt_event_t event )
{
  event_subscription_t snapshot[EVENT_SUBSCRIPTIONS_SIZE];
  size_t               count     = 0;

  (void) osal_mutex_take( g_ctx.event_mutex );
  for ( size_t i = 0; i < g_ctx.event_subs.size; ++i )
  {
    if ( g_ctx.event_subs.items[i].event == event )
    {
      snapshot[count++] = g_ctx.event_subs.items[i];
    }
  }
  (void) osal_mutex_give( g_ctx.event_mutex );

  /* Invoke callbacks outside the lock so handlers may subscribe/unsubscribe. */
  for ( size_t i = 0; i < count; ++i )
  {
    snapshot[i].cb( event, snapshot[i].user_data );
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

static void _lock_state( void )
{
  (void) osal_mutex_take( g_ctx.state_mutex );
}

static void _unlock_state( void )
{
  (void) osal_mutex_give( g_ctx.state_mutex );
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
  _lock_state();
  memset( &g_ctx.ip_info, 0, sizeof( g_ctx.ip_info ) );
  strncpy( g_ctx.ip_info.ssid, g_ctx.sta_cfg.ssid, sizeof( g_ctx.ip_info.ssid ) - 1 );
  g_ctx.ip_info.urc = (int) reason;

  if ( reason != UPDATE_CONNECTION_OK )
  {
    _unlock_state();
    return;
  }

  wifi_hal_ip_info_t hal_ip = { 0 };
  if ( wifi_hal_get_sta_ip_info( &hal_ip ) != OSAL_SUCCESS )
  {
    _unlock_state();
    return;
  }

  strncpy( g_ctx.ip_info.ip, hal_ip.ip, sizeof( g_ctx.ip_info.ip ) - 1 );
  strncpy( g_ctx.ip_info.netmask, hal_ip.netmask, sizeof( g_ctx.ip_info.netmask ) - 1 );
  strncpy( g_ctx.ip_info.gw, hal_ip.gw, sizeof( g_ctx.ip_info.gw ) - 1 );
  _unlock_state();
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
    _lock_state();
    const wifi_app_status_t old_state = g_ctx.state;
    g_ctx.state = new_state;
    _unlock_state();
    osal_log_debug( "[wifi] state %s -> %s", _state_name( old_state ), _state_name( new_state ) );
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
      _lock_state();
      g_ctx.connected        = false;
      g_ctx.reason_disconnect = data ? data->disconnect_reason : 0;
      _unlock_state();
      _safe_update_sta_ip_string( "0.0.0.0" );
      break;

    case WIFI_HAL_EVT_STA_GOT_IP:
      _lock_state();
      g_ctx.connected = true;
      _unlock_state();
      if ( data )
      {
        _safe_update_sta_ip_string( data->ip_info.ip );
      }
      break;

    case WIFI_HAL_EVT_SCAN_DONE:
      {
        /* Publish the fresh scan snapshot under the state lock so concurrent
         * readers see either the previous snapshot or the completed one, never
         * a partially written list. Events are dispatched only after the lock
         * is released so handlers may re-enter the getter API without deadlock. */
        _lock_state();
        uint16_t ap_num = DEFAULT_SCAN_LIST_SIZE;
        if ( wifi_hal_get_scanned_ap( g_ctx.scan_list, &ap_num ) == OSAL_SUCCESS )
        {
          g_ctx.scanned_ap_num = ap_num;
        }
        g_ctx.scan_in_progress = false;
        g_ctx.scan_generation++;
        _unlock_state();
        /* Notify any waiter that scan completed. */
        (void) osal_bin_sem_give( g_ctx.scan_sem );
        _event_dispatch( WIFI_MGMT_EVENT_SCAN_COMPLETED );
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
  g_ctx.scan_in_progress = false;

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

  _event_dispatch( WIFI_MGMT_EVENT_MODE_CHANGED );
  _update_ip_info( UPDATE_LOST_CONNECTION );
  _change_state( WIFI_APP_IDLE );
}

static void _request_mode_change( void );

static void _state_idle( void )
{
  _lock_state();
  const bool mode_pending = g_ctx.mode_req != (wifi_type_t) 0;
  _unlock_state();

  if ( mode_pending )
  {
    _request_mode_change();
    return;
  }

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
      _event_dispatch( WIFI_MGMT_EVENT_CONNECT_FAILED );
      _change_state( WIFI_APP_STOP );
      return;
    }

    _change_state( WIFI_APP_IDLE );
  }
}

static void _state_wait_connect( void )
{
  _lock_state();
  const bool is_conn = g_ctx.connected;
  if ( is_conn )
  {
    g_ctx.disconnect_req    = false;
    g_ctx.connect_req       = false;
    g_ctx.connect_attempts  = 0;
  }
  _unlock_state();

  if ( is_conn )
  {
    _save_current_sta_config();
    _change_state( WIFI_APP_START );
    return;
  }

  if ( g_ctx.connect_attempts > 30 )
  {
    _try_next_credential();
    g_ctx.connect_req      = false;
    g_ctx.connect_attempts = 0;
    _event_dispatch( WIFI_MGMT_EVENT_CONNECT_FAILED );
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
  _event_dispatch( WIFI_MGMT_EVENT_CONNECTED );
  _change_state( WIFI_APP_READY );
}

/* Apply a pending asynchronous mode change (always running in the worker task).
 *
 * The HAL is stopped and restarted only when the requested type differs from
 * the current one, station credentials are deliberately left intact so an
 * AP+STA -> STA (or any) transition never drops the saved network, and
 * WIFI_MGMT_EVENT_MODE_CHANGED is emitted only after the HAL restart succeeds.
 * On failure the pending request is cleared and the machine is left in a
 * defined recoverable state (HAL stopped, current mode unchanged) from which
 * the caller may safely retry. */
static void _request_mode_change( void )
{
  _lock_state();
  const wifi_type_t requested = g_ctx.mode_req;
  g_ctx.mode_req = (wifi_type_t) 0;
  g_ctx.connected   = false;
  g_ctx.is_started  = false;
  _unlock_state();

  if ( requested != T_WIFI_TYPE_SERVER &&
       requested != T_WIFI_TYPE_CLIENT &&
       requested != T_WIFI_TYPE_CLI_SER )
  {
    return;
  }

  wifi_hal_mode_t hal_mode = WIFI_HAL_MODE_STA;
  if ( requested == T_WIFI_TYPE_SERVER )
  {
    hal_mode = WIFI_HAL_MODE_AP;
  }
  else if ( requested == T_WIFI_TYPE_CLIENT )
  {
    hal_mode = WIFI_HAL_MODE_STA;
  }
  else
  {
    hal_mode = WIFI_HAL_MODE_APSTA;
  }

  /* Always cycle the HAL when a distinct mode is requested. It was either still
   * running in the old mode (stop + restart) or already stopped (restart). */
  (void) wifi_hal_stop();

  if ( _start_mode( hal_mode ) != OSAL_SUCCESS )
  {
    /* Start failed: keep current mode, HAL stopped, IDLE/READY drive a retry. */
    osal_log_error( "[wifi] mode change to %u failed, HAL left stopped", (unsigned) requested );
    return;
  }

  _lock_state();
  g_wifi_type = requested;
  _unlock_state();
  _update_ip_info( UPDATE_LOST_CONNECTION );
  _event_dispatch( WIFI_MGMT_EVENT_MODE_CHANGED );
}

static void _state_stop( void )
{
  osal_log_debug( "[wifi] _state_stop enter" );

  /* Snapshot the connection flags atomically and decide which IP-reason code
   * to publish. The state lock is released before _update_ip_info (which takes
   * it internally) and before any callback/event dispatch, so subscribers may
   * re-enter the getter API without deadlocking. */
  bool do_failed = false;
  bool do_user   = false;
  bool do_lost   = false;
  _lock_state();
  const bool was_connected = g_ctx.connected;
  if ( g_ctx.disconnect_req )
  {
    g_ctx.disconnect_req = false;
    if ( g_ctx.reason_disconnect != 0 )
    {
      do_failed   = true;
      g_ctx.reason_disconnect = 0;
    }
    else
    {
      do_user = true;
    }
  }
  else if ( !was_connected )
  {
    do_lost = true;
  }
  _unlock_state();

  /* Publish the updated IP snapshot before notifying subscribers so a handler
   * reading management state at event time observes the post-disconnect
   * snapshot (is_connected() false, ip_info reason set). */
  if ( do_failed )
  {
    _update_ip_info( UPDATE_FAILED_ATTEMPT );
  }
  else if ( do_user )
  {
    _update_ip_info( UPDATE_USER_DISCONNECT );
  }
  else if ( do_lost )
  {
    _update_ip_info( UPDATE_LOST_CONNECTION );
  }

  (void) wifi_hal_disconnect();
  _lock_state();
  g_ctx.connected = false;
  _unlock_state();

  /* Notify subscribers only after the state transition is final. The event
   * handlers take an internal snapshot under the event mutex and invoke each
   * callback after releasing it, so no subscriber runs under a WiFi lock. */
  (void) _run_from_cb_list( &g_ctx.on_disconnect_cb );
  if ( was_connected )
  {
    _event_dispatch( WIFI_MGMT_EVENT_DISCONNECTED );
  }

  _change_state( WIFI_APP_IDLE );
}

static void _state_ready( void )
{
  _lock_state();
  const bool mode_pending = g_ctx.mode_req != (wifi_type_t) 0;
  const bool stop_req     = g_ctx.disconnect_req || !g_ctx.connected || g_ctx.connect_req;
  _unlock_state();
  if ( mode_pending )
  {
    _request_mode_change();
    return;
  }
  if ( stop_req )
  {
    _change_state( WIFI_APP_STOP );
    return;
  }

  (void) wifi_hal_get_sta_rssi( &g_ctx.rssi );
  (void) osal_task_delay_ms( 200 );
}

static void _state_deinit( void )
{
  _lock_state();
  g_ctx.is_started        = false;
  g_ctx.is_power_save     = false;
  g_ctx.connected         = false;
  g_ctx.disconnect_req    = false;
  g_ctx.connect_req       = false;
  g_ctx.mode_req          = (wifi_type_t) 0;
  g_ctx.connect_attempts  = 0;
  g_ctx.reason_disconnect = 0;
  g_ctx.client_cnt        = 0;
  g_ctx.scanned_ap_num    = 0;
  g_ctx.scan_in_progress  = false;
  _unlock_state();

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
  memset( &g_ctx.event_subs, 0, sizeof( g_ctx.event_subs ) );
  (void) osal_mutex_create( &g_ctx.event_mutex, "wifi_evt" );
  (void) osal_mutex_create( &g_ctx.state_mutex, "wifi_state" );

  (void) osal_bin_sem_create( &g_ctx.ip_sem, "wifi_ip", OSAL_SEM_FULL );
  (void) osal_bin_sem_create( &g_ctx.scan_sem, "wifi_scan", OSAL_SEM_EMPTY );

  _load_saved_config();
  _update_ip_info( UPDATE_LOST_CONNECTION );
  _safe_update_sta_ip_string( "0.0.0.0" );

  if ( g_ctx.config_loaded && g_ctx.sta_cfg.ssid[0] != '\0' )
  {
    osal_log_debug( "[wifi] saved credentials found, auto-connect enabled" );
    g_ctx.connect_req = true;
  }

  osal_task_attr_t attr;
  (void) osal_task_attributes_init( &attr );
  size_t stack_size = OSAL_TASK_MIN_STACK_SIZE * 4;
  osal_status_t task_rc = osal_task_create( &g_wifi_task_id,
                           "wifi_task",
                           _wifi_event_task,
                           NULL,
                           NULL,
                           stack_size,
                           NORMALPRIO,
                           &attr );
  osal_log_info( "[wifi] task create rc=%d, stack=%zu, prio=%u",
                 (int) task_rc, stack_size, (unsigned) NORMALPRIO );
  if ( task_rc != OSAL_SUCCESS )
  {
    osal_log_error( "[wifi] FAILED to create wifi_task (rc=%d)", (int) task_rc );
  }
}

void wifi_mgmt_set_wifi_type( wifi_type_t type )
{
  if ( type == g_wifi_type )
  {
    return;
  }

  g_wifi_type = type;

  /* Only notify once the module is live (mutex created). */
  if ( g_ctx.state != WIFI_APP_DISABLE )
  {
    _event_dispatch( WIFI_MGMT_EVENT_MODE_CHANGED );
  }
}

bool wifi_mgmt_request_mode( wifi_type_t type )
{
  if ( type != T_WIFI_TYPE_SERVER &&
       type != T_WIFI_TYPE_CLIENT &&
       type != T_WIFI_TYPE_CLI_SER )
  {
    return false;
  }

  _lock_state();
  if ( type == g_wifi_type )
  {
    /* Repeated request for the current mode: harmless no-op. */
    g_ctx.mode_req = (wifi_type_t) 0;
    _unlock_state();
    return true;
  }

  /* Queue the transition. The Wi-Fi worker task applies it and emits
   * MODE_CHANGED only after the HAL restart succeeds. */
  g_ctx.mode_req = type;
  _unlock_state();
  return true;
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
  _lock_state();
  if ( g_wifi_type == T_WIFI_TYPE_SERVER || g_ctx.scan_in_progress )
  {
    _unlock_state();
    return false;
  }

  const bool state_ok = g_ctx.state == WIFI_APP_IDLE || g_ctx.state == WIFI_APP_READY;
  if ( !state_ok )
  {
    _unlock_state();
    return false;
  }
  g_ctx.scan_in_progress = true;
  _unlock_state();

  /* Clear stale SCAN_DONE semaphore state from previous scans.
   * This prevents a new blocking scan from returning immediately. */
  while ( osal_bin_sem_timed_wait( g_ctx.scan_sem, 0 ) == OSAL_SUCCESS )
  {
  }

  if ( wifi_hal_start_scan( block ) != OSAL_SUCCESS )
  {
    _lock_state();
    g_ctx.scan_in_progress = false;
    _unlock_state();
    return false;
  }

  if ( block )
  {
    /* Wait for the SCAN_DONE callback to signal completion. */
    if ( osal_bin_sem_timed_wait( g_ctx.scan_sem, WIFI_SCAN_TIMEOUT_MS ) != OSAL_SUCCESS )
    {
      /* Timed out waiting for results; clear flag and report failure. */
      _lock_state();
      g_ctx.scan_in_progress = false;
      _unlock_state();
      return false;
    }
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
  if ( !ap_count )
  {
    return;
  }

  _lock_state();
  *ap_count = g_ctx.scanned_ap_num;
  _unlock_state();
}

bool wifi_mgmt_is_scan_active( void )
{
  _lock_state();
  const bool active = g_ctx.scan_in_progress;
  _unlock_state();
  return active;
}

uint32_t wifi_mgmt_get_scan_generation( void )
{
  _lock_state();
  const uint32_t gen = g_ctx.scan_generation;
  _unlock_state();
  return gen;
}

bool wifi_mgmt_get_name_from_scanned_list( uint8_t number, char* name )
{
  if ( !name )
  {
    return false;
  }

  _lock_state();
  if ( number >= g_ctx.scanned_ap_num )
  {
    _unlock_state();
    return false;
  }

  strncpy( name, g_ctx.scan_list[number].ssid, MAX_SSID_SIZE );
  name[MAX_SSID_SIZE] = '\0';
  _unlock_state();
  return true;
}

bool wifi_mgmt_set_from_ap_list( uint8_t num )
{
  _lock_state();
  if ( num >= g_ctx.scanned_ap_num )
  {
    _unlock_state();
    return false;
  }

  memset( g_ctx.sta_cfg.ssid, 0, sizeof( g_ctx.sta_cfg.ssid ) );
  strncpy( g_ctx.sta_cfg.ssid, g_ctx.scan_list[num].ssid, sizeof( g_ctx.sta_cfg.ssid ) - 1 );
  _unlock_state();
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
  name[MAX_SSID_SIZE] = '\0';
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

  _lock_state();
  g_ctx.connect_req = true;
  _unlock_state();
  return true;
}

bool wifi_mgmt_disconnect( void )
{
  if ( g_wifi_type == T_WIFI_TYPE_SERVER )
  {
    return false;
  }

  _lock_state();
  g_ctx.disconnect_req = true;
  _unlock_state();
  return true;
}

bool wifi_mgmt_is_connected( void )
{
  _lock_state();
  const bool conn = g_ctx.connected && !g_ctx.connect_req;
  _unlock_state();
  return conn;
}

bool wifi_mgmt_is_ready_to_scan( void )
{
  _lock_state();
  const bool ok = g_ctx.state == WIFI_APP_IDLE || g_ctx.state == WIFI_APP_READY;
  _unlock_state();
  return ok;
}

bool wifi_mgmt_ready_to_connect( void )
{
  _lock_state();
  const bool ok = g_ctx.state == WIFI_APP_IDLE;
  _unlock_state();
  return ok;
}

bool wifi_mgmt_trying_connect( void )
{
  _lock_state();
  const bool ok = g_ctx.state == WIFI_APP_WAIT_CONNECT || g_ctx.state == WIFI_APP_CONNECT;
  _unlock_state();
  return ok;
}

bool wifi_mgmt_is_read_data( void )
{
  _lock_state();
  const bool ok = g_ctx.read_wifi_data;
  _unlock_state();
  return ok;
}

bool wifi_mgmt_is_idle( void )
{
  _lock_state();
  const bool ok = g_ctx.state == WIFI_APP_IDLE;
  _unlock_state();
  return ok;
}

bool wifi_mgmt_is_running( void )
{
  _lock_state();
  const bool running = g_ctx.state != WIFI_APP_DISABLE;
  _unlock_state();
  return running;
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

bool wifi_mgmt_subscribe( wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void* user_data )
{
  return _event_subscribe( event, cb, user_data );
}

bool wifi_mgmt_unsubscribe( wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void* user_data )
{
  return _event_unsubscribe( event, cb, user_data );
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

  _lock_state();
  *info = g_ctx.ip_info;
  _unlock_state();
  return true;
}

bool wifi_mgmt_get_access_points( wifi_mgmt_ap_list_t* list )
{
  if ( !list )
  {
    return false;
  }

  /* Copy the scan snapshot under the state lock so a concurrent SCAN_DONE
   * update can never leave count and items inconsistent. */
  _lock_state();
  memset( list, 0, sizeof( *list ) );
  uint16_t n = g_ctx.scanned_ap_num;
  if ( n > WIFI_DRV_MAX_SCAN_AP )
  {
    n = WIFI_DRV_MAX_SCAN_AP;
  }
  list->count = n;

  for ( uint16_t i = 0; i < n; ++i )
  {
    strncpy( list->items[i].ssid, g_ctx.scan_list[i].ssid, MAX_SSID_SIZE );
    list->items[i].chan = g_ctx.scan_list[i].channel;
    list->items[i].rssi = g_ctx.scan_list[i].rssi;
    list->items[i].auth = g_ctx.scan_list[i].authmode;
  }
  _unlock_state();

  return true;
}
