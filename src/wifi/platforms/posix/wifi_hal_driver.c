/**
 *******************************************************************************
 * @file    wifi_hal_driver.c (POSIX simulator)
 * @author  Dmytro Shevchenko
 * @brief   Simulated Wi-Fi HAL for POSIX targets
 *
 *          Provides a virtual Wi-Fi environment with predefined access points
 *          that exhibit different connection behaviours.  Useful for
 *          integration testing and development without real hardware.
 *
 *          Simulated APs:
 *            1. properly_ap       — stable connection, never drops
 *            2. disconnect_15_sec — connects, then drops after 15 s
 *            3. broken            — always rejects connections
 *            4. disconnect_1_min  — connects, then drops after 60 s
 *            5. slow_connect      — connects after a 3 s delay
 *            6. weak_signal       — stable but very low RSSI (-90 dBm)
 *            7. wrong_password    — rejects unless password is exact
 *******************************************************************************
 */

#include "wifi_hal_driver.h"

#include "osal_log.h"
#include "osal_task.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/*  Simulated AP definitions                                                  */
/* -------------------------------------------------------------------------- */

/** @brief Behaviour flags for simulated access points. */
typedef enum
{
  AP_BEHAV_NORMAL        = 0, /**< Stable connection.                      */
  AP_BEHAV_DISCONNECT,        /**< Disconnects after a configured delay.   */
  AP_BEHAV_REJECT,            /**< Always rejects connection attempts.     */
  AP_BEHAV_SLOW_CONNECT,      /**< Delays before GOT_IP event.            */
  AP_BEHAV_WRONG_PASSWORD,    /**< Rejects unless password matches exactly.*/
} sim_ap_behaviour_t;

/** @brief Descriptor for one simulated access point. */
typedef struct
{
  const char*         ssid;
  const char*         password;
  int                 channel;
  int                 rssi;
  int                 authmode;
  sim_ap_behaviour_t  behaviour;
  uint32_t            param_ms;   /**< Delay / disconnect time in ms. */
} sim_ap_t;

/** @brief Master list of simulated access points. */
static const sim_ap_t g_sim_aps[] = {
  /* 1. Normal, stable connection */
  {
    .ssid      = "properly_ap",
    .password  = "12345678",
    .channel   = 1,
    .rssi      = -10,
    .authmode  = 3, /* WPA2 */
    .behaviour = AP_BEHAV_NORMAL,
    .param_ms  = 0,
  },
  /* 2. Disconnects after 15 seconds */
  {
    .ssid      = "disconnect_15_sec",
    .password  = "12345678",
    .channel   = 6,
    .rssi      = -20,
    .authmode  = 3,
    .behaviour = AP_BEHAV_DISCONNECT,
    .param_ms  = 15000,
  },
  /* 3. Always broken — connection rejected */
  {
    .ssid      = "broken",
    .password  = "12345678",
    .channel   = 11,
    .rssi      = -50,
    .authmode  = 3,
    .behaviour = AP_BEHAV_REJECT,
    .param_ms  = 0,
  },
  /* 4. Disconnects after 1 minute */
  {
    .ssid      = "disconnect_1_min",
    .password  = "12345678",
    .channel   = 3,
    .rssi      = -30,
    .authmode  = 3,
    .behaviour = AP_BEHAV_DISCONNECT,
    .param_ms  = 60000,
  },
  /* 5. Slow connect — GOT_IP arrives after 3 s */
  {
    .ssid      = "slow_connect",
    .password  = "12345678",
    .channel   = 9,
    .rssi      = -45,
    .authmode  = 3,
    .behaviour = AP_BEHAV_SLOW_CONNECT,
    .param_ms  = 3000,
  },
  /* 6. Weak signal but stable */
  {
    .ssid      = "weak_signal",
    .password  = "12345678",
    .channel   = 4,
    .rssi      = -90,
    .authmode  = 3,
    .behaviour = AP_BEHAV_NORMAL,
    .param_ms  = 0,
  },
  /* 7. Wrong-password gate — rejects unless the password is exactly right */
  {
    .ssid      = "wrong_password",
    .password  = "correct_pw",
    .channel   = 7,
    .rssi      = -40,
    .authmode  = 4, /* WPA2-Enterprise */
    .behaviour = AP_BEHAV_WRONG_PASSWORD,
    .param_ms  = 0,
  },
};

#define SIM_AP_COUNT ( sizeof( g_sim_aps ) / sizeof( g_sim_aps[0] ) )

/* Shared network parameters for all simulated APs. */
#define SIM_IP       "192.168.1.100"
#define SIM_NETMASK  "255.255.255.0"
#define SIM_GATEWAY  "192.168.1.1"

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */

typedef struct
{
  bool                  initialized;
  bool                  started;
  bool                  connected;
  bool                  power_save;
  wifi_hal_mode_t       mode;

  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t  ap_cfg;
  wifi_hal_event_cb_t   event_cb;
  void*                 user_data;

  const sim_ap_t*       active_ap;      /**< AP we're connected to (or NULL). */

  /* Disconnect timer thread */
  pthread_t             disc_thread;
  bool                  disc_thread_active;
  bool                  disc_cancel;
  pthread_mutex_t       disc_mutex;
  pthread_cond_t        disc_cond;
  uint32_t              disc_delay_ms;

  /* Slow-connect thread */
  pthread_t             conn_thread;
  bool                  conn_thread_active;
  bool                  conn_cancel;
  pthread_mutex_t       conn_mutex;
  pthread_cond_t        conn_cond;
  uint32_t              conn_delay_ms;

  /* AP client count (soft-AP simulation) */
  uint32_t              client_cnt;
} sim_state_t;

static sim_state_t g_sim = { 0 };

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

static const sim_ap_t* _find_ap( const char* ssid )
{
  for ( size_t i = 0; i < SIM_AP_COUNT; ++i )
  {
    if ( strncmp( g_sim_aps[i].ssid, ssid, WIFI_HAL_SSID_MAX_LEN ) == 0 )
    {
      return &g_sim_aps[i];
    }
  }
  return NULL;
}

static void _fire_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  if ( g_sim.event_cb )
  {
    g_sim.event_cb( event, data, g_sim.user_data );
  }
}

/* Forward declarations. */
static void _stop_disconnect_timer_if_active( void );
static void _stop_connect_timer_if_active( void );

/* Thread: fires STA_DISCONNECTED after a configurable delay.
   Can be cancelled via disc_cancel + cond signal. */
static void* _disconnect_thread( void* arg )
{
  (void) arg;

  pthread_mutex_lock( &g_sim.disc_mutex );

  struct timespec deadline;
  clock_gettime( CLOCK_REALTIME, &deadline );
  uint32_t ms = g_sim.disc_delay_ms;
  deadline.tv_sec  += (time_t)( ms / 1000U );
  deadline.tv_nsec += (long)( ( ms % 1000U ) * 1000000L );
  if ( deadline.tv_nsec >= 1000000000L )
  {
    deadline.tv_sec  += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  while ( !g_sim.disc_cancel )
  {
    int rc = pthread_cond_timedwait( &g_sim.disc_cond, &g_sim.disc_mutex, &deadline );
    if ( rc != 0 ) /* ETIMEDOUT or error */
    {
      break;
    }
  }

  bool should_fire = !g_sim.disc_cancel && g_sim.connected;
  pthread_mutex_unlock( &g_sim.disc_mutex );

  if ( should_fire )
  {
    osal_log_info( "[wifi-sim] timed disconnect after %u ms", (unsigned) ms );
    g_sim.connected = false;
    g_sim.active_ap = NULL;

    wifi_hal_event_data_t evt = { .disconnect_reason = 8 /* ASSOC_LEAVE */ };
    _fire_event( WIFI_HAL_EVT_STA_DISCONNECTED, &evt );
  }

  g_sim.disc_thread_active = false;
  return NULL;
}

static void _stop_disconnect_timer_if_active( void )
{
  if ( !g_sim.disc_thread_active )
  {
    return;
  }

  pthread_mutex_lock( &g_sim.disc_mutex );
  g_sim.disc_cancel = true;
  pthread_cond_signal( &g_sim.disc_cond );
  pthread_mutex_unlock( &g_sim.disc_mutex );
  pthread_join( g_sim.disc_thread, NULL );
  g_sim.disc_thread_active = false;
}

static void _start_disconnect_timer( uint32_t delay_ms )
{
  _stop_disconnect_timer_if_active();

  pthread_mutex_lock( &g_sim.disc_mutex );
  g_sim.disc_cancel       = false;
  g_sim.disc_delay_ms     = delay_ms;
  g_sim.disc_thread_active = true;
  pthread_mutex_unlock( &g_sim.disc_mutex );

  pthread_create( &g_sim.disc_thread, NULL, _disconnect_thread, NULL );
}

/* Thread: delays, then fires STA_GOT_IP (slow-connect simulation). */
static void* _slow_connect_thread( void* arg )
{
  (void) arg;

  pthread_mutex_lock( &g_sim.conn_mutex );

  struct timespec deadline;
  clock_gettime( CLOCK_REALTIME, &deadline );
  uint32_t ms = g_sim.conn_delay_ms;
  deadline.tv_sec  += (time_t)( ms / 1000U );
  deadline.tv_nsec += (long)( ( ms % 1000U ) * 1000000L );
  if ( deadline.tv_nsec >= 1000000000L )
  {
    deadline.tv_sec  += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  while ( !g_sim.conn_cancel )
  {
    int rc = pthread_cond_timedwait( &g_sim.conn_cond, &g_sim.conn_mutex, &deadline );
    if ( rc != 0 )
    {
      break;
    }
  }

  bool should_fire = !g_sim.conn_cancel;
  pthread_mutex_unlock( &g_sim.conn_mutex );

  if ( should_fire )
  {
    osal_log_info( "[wifi-sim] slow connect completed after %u ms", (unsigned) ms );
    g_sim.connected = true;

    wifi_hal_event_data_t evt = { 0 };
    strncpy( evt.ip_info.ip,      SIM_IP,      sizeof( evt.ip_info.ip ) - 1 );
    strncpy( evt.ip_info.netmask, SIM_NETMASK, sizeof( evt.ip_info.netmask ) - 1 );
    strncpy( evt.ip_info.gw,      SIM_GATEWAY, sizeof( evt.ip_info.gw ) - 1 );
    _fire_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  }

  g_sim.conn_thread_active = false;
  return NULL;
}

static void _stop_connect_timer_if_active( void )
{
  if ( !g_sim.conn_thread_active )
  {
    return;
  }

  pthread_mutex_lock( &g_sim.conn_mutex );
  g_sim.conn_cancel = true;
  pthread_cond_signal( &g_sim.conn_cond );
  pthread_mutex_unlock( &g_sim.conn_mutex );
  pthread_join( g_sim.conn_thread, NULL );
  g_sim.conn_thread_active = false;
}

static void _start_slow_connect( uint32_t delay_ms )
{
  _stop_connect_timer_if_active();

  pthread_mutex_lock( &g_sim.conn_mutex );
  g_sim.conn_cancel        = false;
  g_sim.conn_delay_ms      = delay_ms;
  g_sim.conn_thread_active = true;
  pthread_mutex_unlock( &g_sim.conn_mutex );

  pthread_create( &g_sim.conn_thread, NULL, _slow_connect_thread, NULL );
}

/* -------------------------------------------------------------------------- */
/*  HAL implementation                                                        */
/* -------------------------------------------------------------------------- */

osal_status_t wifi_hal_init( const wifi_hal_init_t* init )
{
  if ( !init || !init->event_cb )
  {
    return OSAL_INVALID_POINTER;
  }

  memset( &g_sim, 0, sizeof( g_sim ) );
  pthread_mutex_init( &g_sim.disc_mutex, NULL );
  pthread_cond_init( &g_sim.disc_cond, NULL );
  pthread_mutex_init( &g_sim.conn_mutex, NULL );
  pthread_cond_init( &g_sim.conn_cond, NULL );

  g_sim.event_cb    = init->event_cb;
  g_sim.user_data   = init->user_data;
  g_sim.initialized = true;

  osal_log_info( "[wifi-sim] HAL initialized (%zu simulated APs)", SIM_AP_COUNT );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit( void )
{
  _stop_disconnect_timer_if_active();
  _stop_connect_timer_if_active();

  pthread_mutex_destroy( &g_sim.disc_mutex );
  pthread_cond_destroy( &g_sim.disc_cond );
  pthread_mutex_destroy( &g_sim.conn_mutex );
  pthread_cond_destroy( &g_sim.conn_cond );

  g_sim.initialized = false;
  g_sim.started     = false;
  g_sim.connected   = false;
  g_sim.active_ap   = NULL;

  osal_log_info( "[wifi-sim] HAL deinitialized" );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  if ( !g_sim.initialized )
  {
    return OSAL_ERROR;
  }

  g_sim.mode    = mode;
  g_sim.started = true;
  osal_log_info( "[wifi-sim] started, mode=%d", (int) mode );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop( void )
{
  _stop_disconnect_timer_if_active();
  _stop_connect_timer_if_active();

  g_sim.started   = false;
  g_sim.connected = false;
  g_sim.active_ap = NULL;
  osal_log_info( "[wifi-sim] stopped" );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  g_sim.sta_cfg = *config;
  osal_log_debug( "[wifi-sim] STA config: ssid=\"%s\"", config->ssid );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  g_sim.ap_cfg = *config;
  osal_log_debug( "[wifi-sim] AP config: ssid=\"%s\"", config->ssid );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  if ( !g_sim.started )
  {
    osal_log_warning( "[wifi-sim] connect called but not started" );
    return OSAL_ERROR;
  }

  const sim_ap_t* ap = _find_ap( g_sim.sta_cfg.ssid );
  if ( !ap )
  {
    osal_log_warning( "[wifi-sim] SSID \"%s\" not found in simulated environment",
                      g_sim.sta_cfg.ssid );
    return OSAL_ERROR;
  }

  osal_log_info( "[wifi-sim] connecting to \"%s\" (behaviour=%d) ...",
                 ap->ssid, (int) ap->behaviour );

  switch ( ap->behaviour )
  {
    case AP_BEHAV_REJECT:
      osal_log_info( "[wifi-sim] \"%s\" — connection REJECTED", ap->ssid );
      return OSAL_ERROR;

    case AP_BEHAV_WRONG_PASSWORD:
      if ( strncmp( g_sim.sta_cfg.password, ap->password,
                    WIFI_HAL_PASSWORD_MAX_LEN ) != 0 )
      {
        osal_log_info( "[wifi-sim] \"%s\" — wrong password", ap->ssid );
        return OSAL_ERROR;
      }
      /* Correct password — fall through to normal connect. */
      /* fallthrough */

    case AP_BEHAV_NORMAL:
    {
      g_sim.connected = true;
      g_sim.active_ap = ap;

      wifi_hal_event_data_t evt = { 0 };
      strncpy( evt.ip_info.ip,      SIM_IP,      sizeof( evt.ip_info.ip ) - 1 );
      strncpy( evt.ip_info.netmask, SIM_NETMASK, sizeof( evt.ip_info.netmask ) - 1 );
      strncpy( evt.ip_info.gw,      SIM_GATEWAY, sizeof( evt.ip_info.gw ) - 1 );

      osal_log_info( "[wifi-sim] \"%s\" — connected, IP=%s", ap->ssid, SIM_IP );
      _fire_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
      return OSAL_SUCCESS;
    }

    case AP_BEHAV_DISCONNECT:
    {
      g_sim.connected = true;
      g_sim.active_ap = ap;

      wifi_hal_event_data_t evt = { 0 };
      strncpy( evt.ip_info.ip,      SIM_IP,      sizeof( evt.ip_info.ip ) - 1 );
      strncpy( evt.ip_info.netmask, SIM_NETMASK, sizeof( evt.ip_info.netmask ) - 1 );
      strncpy( evt.ip_info.gw,      SIM_GATEWAY, sizeof( evt.ip_info.gw ) - 1 );

      osal_log_info( "[wifi-sim] \"%s\" — connected, will disconnect in %u ms",
                     ap->ssid, (unsigned) ap->param_ms );
      _fire_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );

      /* Start background thread that will fire DISCONNECTED. */
      _start_disconnect_timer( ap->param_ms );
      return OSAL_SUCCESS;
    }

    case AP_BEHAV_SLOW_CONNECT:
    {
      g_sim.active_ap = ap;

      osal_log_info( "[wifi-sim] \"%s\" — slow connect, IP in %u ms",
                     ap->ssid, (unsigned) ap->param_ms );

      /* GOT_IP will be fired asynchronously after the delay. */
      _start_slow_connect( ap->param_ms );
      return OSAL_SUCCESS;
    }
  }

  return OSAL_ERROR;
}

osal_status_t wifi_hal_disconnect( void )
{
  _stop_disconnect_timer_if_active();
  _stop_connect_timer_if_active();

  if ( g_sim.connected )
  {
    osal_log_info( "[wifi-sim] disconnected from \"%s\"",
                   g_sim.active_ap ? g_sim.active_ap->ssid : "?" );
  }

  g_sim.connected = false;
  g_sim.active_ap = NULL;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start_scan( bool block )
{
  osal_log_debug( "[wifi-sim] scan started (block=%d)", (int) block );

  /* Notify management that scan is complete — results ready immediately. */
  _fire_event( WIFI_HAL_EVT_SCAN_DONE, NULL );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !records || !in_out_count )
  {
    return OSAL_INVALID_POINTER;
  }

  uint16_t capacity = *in_out_count;
  uint16_t to_copy  = ( SIM_AP_COUNT > capacity ) ? capacity : (uint16_t) SIM_AP_COUNT;

  for ( uint16_t i = 0; i < to_copy; ++i )
  {
    memset( &records[i], 0, sizeof( records[i] ) );
    strncpy( records[i].ssid, g_sim_aps[i].ssid, WIFI_HAL_SSID_MAX_LEN );
    records[i].channel  = g_sim_aps[i].channel;
    records[i].rssi     = g_sim_aps[i].rssi;
    records[i].authmode = g_sim_aps[i].authmode;
  }

  *in_out_count = to_copy;
  osal_log_debug( "[wifi-sim] scan returned %u APs", (unsigned) to_copy );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( !g_sim.connected )
  {
    memset( out_info, 0, sizeof( *out_info ) );
    return OSAL_ERROR;
  }

  strncpy( out_info->ip,      SIM_IP,      sizeof( out_info->ip ) - 1 );
  strncpy( out_info->netmask, SIM_NETMASK, sizeof( out_info->netmask ) - 1 );
  strncpy( out_info->gw,      SIM_GATEWAY, sizeof( out_info->gw ) - 1 );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_rssi( int* out_rssi )
{
  if ( !out_rssi )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( !g_sim.connected || !g_sim.active_ap )
  {
    *out_rssi = 0;
    return OSAL_ERROR;
  }

  *out_rssi = g_sim.active_ap->rssi;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  g_sim.power_save = enabled;
  osal_log_debug( "[wifi-sim] power save %s", enabled ? "ON" : "OFF" );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] )
{
  if ( !mac )
  {
    return OSAL_INVALID_POINTER;
  }

  /* Deterministic simulated MAC address. */
  mac[0] = 0xDE;
  mac[1] = 0xAD;
  mac[2] = 0xBE;
  mac[3] = 0xEF;
  mac[4] = 0xCA;
  mac[5] = 0xFE;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count )
{
  if ( !out_client_count )
  {
    return OSAL_INVALID_POINTER;
  }

  *out_client_count = g_sim.client_cnt;
  return OSAL_SUCCESS;
}
