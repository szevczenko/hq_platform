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

  char                  ap_dns[16]; /**< DNS advertised by AP DHCP (empty when omitted). */
  bool                  ap_dns_set; /**< True when a non-empty ap_dns was configured.    */

  const sim_ap_t*       active_ap;      /**< AP we're connected to (or NULL). */

  /* Disconnect timer thread.  disc_thread_created is set only after a
   * successful pthread_create() and cleared only after pthread_join() returns,
   * so the stop path always joins a thread that was ever started — even one
   * that completed on its own — and never joins a handle from a failed
   * pthread_create().  disc_thread_active merely mirrors the running state. */
  pthread_t             disc_thread;
  bool                  disc_thread_active;
  bool                  disc_thread_created;
  bool                  disc_cancel;
  pthread_mutex_t       disc_mutex;
  pthread_cond_t        disc_cond;
  uint32_t              disc_delay_ms;

  /* Slow-connect thread (same created/join discipline as the disconnect timer). */
  pthread_t             conn_thread;
  bool                  conn_thread_active;
  bool                  conn_thread_created;
  bool                  conn_cancel;
  pthread_mutex_t       conn_mutex;
  pthread_cond_t        conn_cond;
  uint32_t              conn_delay_ms;

  /* AP client count (soft-AP simulation) */
  uint32_t              client_cnt;
  pthread_mutex_t       state_mutex; /* Protects shared state (connected, active_ap, flags) */

  /* Callback delivery gate.  Guards event_cb/user_data, the delivery-disabled
   * flag and the in-flight counter.  The callback itself is always invoked
   * outside every HAL lock so callback code may re-enter management APIs. */
  pthread_mutex_t       cb_mutex;
  pthread_cond_t        cb_cond;
  bool                  cb_disabled;  /**< Deinit disabled new deliveries.      */
  uint32_t              cb_in_flight; /**< Number of callbacks still running.   */

  /* Per-session primitive "still alive" flags.  A primitive is flagged as soon
   * as it is created and unset once it is destroyed.  This lets a
   * wifi_hal_deinit() that failed to release a primitive return an error while
   * a later retry never locks or destroys a primitive whose flag is already
   * clear (its thread-safe release was already performed). */
  bool                  prim_disc_mutex;
  bool                  prim_disc_cond;
  bool                  prim_conn_mutex;
  bool                  prim_conn_cond;
  bool                  prim_state_mutex;
  bool                  prim_cb_mutex;
  bool                  prim_cb_cond;
} sim_state_t;

static sim_state_t g_sim = { 0 };

/* Lifecycle serialization.  This lock and the outside-session flag live beyond
 * a single init/deinit session (the lock is statically initialized and never
 * destroyed), so init and deinit can be serialized even across a session
 * boundary and deinit can be recognized as a no-op before any per-session
 * primitive exists. */
static pthread_mutex_t g_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool            g_hal_inited      = false;

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

bool wifi_hal_is_valid_ipv4( const char* str )
{
  if ( !str || strlen( str ) == 0 || strlen( str ) > 15 )
  {
    return false;
  }

  int  octets = 0;
  int  value  = 0;
  int  digits = 0;
  for ( size_t i = 0; i < strlen( str ); ++i )
  {
    char c = str[i];
    if ( c >= '0' && c <= '9' )
    {
      value = value * 10 + ( c - '0' );
      if ( value > 255 )
      {
        return false;
      }
      digits++;
      if ( digits > 3 )
      {
        return false;
      }
    }
    else if ( c == '.' )
    {
      if ( digits == 0 )
      {
        return false; /* empty octet */
      }
      octets++;
      value  = 0;
      digits = 0;
    }
    else
    {
      return false; /* invalid character */
    }
  }

  if ( digits == 0 )
  {
    return false;
  }
  return octets == 3;
}

static void _fire_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  pthread_mutex_lock( &g_sim.cb_mutex );

  /* Deinit either already disabled delivery or will do so under this lock; a
   * delivery that starts after the disabled flag is seen drops the event. */
  if ( g_sim.cb_disabled || g_sim.event_cb == NULL )
  {
    pthread_mutex_unlock( &g_sim.cb_mutex );
    return;
  }

  wifi_hal_event_cb_t cb   = g_sim.event_cb;
  void*               user = g_sim.user_data;
  g_sim.cb_in_flight++;
  pthread_mutex_unlock( &g_sim.cb_mutex );

  /* Invoke the callback outside every HAL lock so callback code may re-enter
   * management APIs (including HAL calls that fire further events) without
   * deadlocking.  Deinit waits for cb_in_flight to drain, which makes every
   * already-started callback complete before deinit returns. */
  cb( event, data, user );

  pthread_mutex_lock( &g_sim.cb_mutex );
  g_sim.cb_in_flight--;
  if ( g_sim.cb_in_flight == 0 )
  {
    pthread_cond_signal( &g_sim.cb_cond );
  }
  pthread_mutex_unlock( &g_sim.cb_mutex );
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

  bool disc_cancelled = g_sim.disc_cancel;
  pthread_mutex_unlock( &g_sim.disc_mutex );

  pthread_mutex_lock( &g_sim.state_mutex );
  bool connected = g_sim.connected;
  pthread_mutex_unlock( &g_sim.state_mutex );

  bool should_fire = !disc_cancelled && connected;

  if ( should_fire )
  {
    osal_log_info( "[wifi-sim] timed disconnect after %u ms", (unsigned) ms );
    pthread_mutex_lock( &g_sim.state_mutex );
    g_sim.connected = false;
    g_sim.active_ap = NULL;
    pthread_mutex_unlock( &g_sim.state_mutex );

    wifi_hal_event_data_t evt = { .disconnect_reason = 8 /* ASSOC_LEAVE */ };
    _fire_event( WIFI_HAL_EVT_STA_DISCONNECTED, &evt );
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.disc_thread_active = false;
  pthread_mutex_unlock( &g_sim.state_mutex );
  return NULL;
}

static void _stop_disconnect_timer_if_active( void )
{
  pthread_mutex_lock( &g_sim.state_mutex );
  bool created = g_sim.disc_thread_created;
  pthread_mutex_unlock( &g_sim.state_mutex );

  /* Join whenever a thread was ever started, even if it already completed on
   * its own: the completed thread is still joinable and its resources are only
   * reclaimed by pthread_join().  Never join a handle that was never created
   * (pthread_create() failed or no timer was started). */
  if ( !created )
  {
    return;
  }

  pthread_mutex_lock( &g_sim.disc_mutex );
  g_sim.disc_cancel = true;
  pthread_cond_signal( &g_sim.disc_cond );
  pthread_mutex_unlock( &g_sim.disc_mutex );
  pthread_join( g_sim.disc_thread, NULL );

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.disc_thread_active  = false;
  g_sim.disc_thread_created = false;
  pthread_mutex_unlock( &g_sim.state_mutex );
}

static void _start_disconnect_timer( uint32_t delay_ms )
{
  _stop_disconnect_timer_if_active();

  pthread_mutex_lock( &g_sim.disc_mutex );
  g_sim.disc_cancel       = false;
  g_sim.disc_delay_ms     = delay_ms;
  pthread_mutex_unlock( &g_sim.disc_mutex );

  /* Flag the thread only after a successful create; a failed create must not
   * leave a handle that a later stop/join would try to join. */
  if ( pthread_create( &g_sim.disc_thread, NULL, _disconnect_thread, NULL ) != 0 )
  {
    osal_log_warning( "[wifi-sim] failed to create disconnect timer thread" );
    return;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.disc_thread_active  = true;
  g_sim.disc_thread_created = true;
  pthread_mutex_unlock( &g_sim.state_mutex );
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
    pthread_mutex_lock( &g_sim.state_mutex );
    g_sim.connected = true;
    pthread_mutex_unlock( &g_sim.state_mutex );

    wifi_hal_event_data_t evt = { 0 };
    strncpy( evt.ip_info.ip,      SIM_IP,      sizeof( evt.ip_info.ip ) - 1 );
    strncpy( evt.ip_info.netmask, SIM_NETMASK, sizeof( evt.ip_info.netmask ) - 1 );
    strncpy( evt.ip_info.gw,      SIM_GATEWAY, sizeof( evt.ip_info.gw ) - 1 );
    _fire_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.conn_thread_active = false;
  pthread_mutex_unlock( &g_sim.state_mutex );
  return NULL;
}

static void _stop_connect_timer_if_active( void )
{
  pthread_mutex_lock( &g_sim.state_mutex );
  bool created = g_sim.conn_thread_created;
  pthread_mutex_unlock( &g_sim.state_mutex );

  /* Join whenever a thread was ever started, even if it already completed on
   * its own: the completed thread is still joinable and its resources are only
   * reclaimed by pthread_join().  Never join a handle that was never created. */
  if ( !created )
  {
    return;
  }

  pthread_mutex_lock( &g_sim.conn_mutex );
  g_sim.conn_cancel = true;
  pthread_cond_signal( &g_sim.conn_cond );
  pthread_mutex_unlock( &g_sim.conn_mutex );
  pthread_join( g_sim.conn_thread, NULL );

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.conn_thread_active  = false;
  g_sim.conn_thread_created = false;
  pthread_mutex_unlock( &g_sim.state_mutex );
}

static void _start_slow_connect( uint32_t delay_ms )
{
  _stop_connect_timer_if_active();

  pthread_mutex_lock( &g_sim.conn_mutex );
  g_sim.conn_cancel        = false;
  g_sim.conn_delay_ms      = delay_ms;
  pthread_mutex_unlock( &g_sim.conn_mutex );

  /* Flag the thread only after a successful create. */
  if ( pthread_create( &g_sim.conn_thread, NULL, _slow_connect_thread, NULL ) != 0 )
  {
    osal_log_warning( "[wifi-sim] failed to create slow-connect thread" );
    return;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.conn_thread_active  = true;
  g_sim.conn_thread_created = true;
  pthread_mutex_unlock( &g_sim.state_mutex );
}

/* -------------------------------------------------------------------------- */
/*  Session lifecycle helpers                                                  */
/* -------------------------------------------------------------------------- */

/* Destroy every per-session synchronization primitive that is still flagged as
 * alive, clearing the flag when the release succeeds.  Used by both the
 * partial-init unwind and deinit.  Returns false if any release failed — the
 * flag of that primitive is kept so a later retry can attempt it again without
 * ever touching a primitive that is already gone. */
static bool _session_primitives_destroy( void )
{
  bool all_destroyed = true;

  if ( g_sim.prim_cb_cond )
  {
    if ( pthread_cond_destroy( &g_sim.cb_cond ) == 0 )
    {
      g_sim.prim_cb_cond = false;
    }
    else
    {
      all_destroyed = false;
    }
  }
  if ( g_sim.prim_cb_mutex )
  {
    if ( pthread_mutex_destroy( &g_sim.cb_mutex ) == 0 )
    {
      g_sim.prim_cb_mutex = false;
    }
    else
    {
      all_destroyed = false;
    }
  }
  if ( g_sim.prim_state_mutex )
  {
    if ( pthread_mutex_destroy( &g_sim.state_mutex ) == 0 )
    {
      g_sim.prim_state_mutex = false;
    }
    else
    {
      all_destroyed = false;
    }
  }
  if ( g_sim.prim_conn_cond )
  {
    if ( pthread_cond_destroy( &g_sim.conn_cond ) == 0 )
    {
      g_sim.prim_conn_cond = false;
    }
    else
    {
      all_destroyed = false;
    }
  }
  if ( g_sim.prim_conn_mutex )
  {
    if ( pthread_mutex_destroy( &g_sim.conn_mutex ) == 0 )
    {
      g_sim.prim_conn_mutex = false;
    }
    else
    {
      all_destroyed = false;
    }
  }
  if ( g_sim.prim_disc_cond )
  {
    if ( pthread_cond_destroy( &g_sim.disc_cond ) == 0 )
    {
      g_sim.prim_disc_cond = false;
    }
    else
    {
      all_destroyed = false;
    }
  }
  if ( g_sim.prim_disc_mutex )
  {
    if ( pthread_mutex_destroy( &g_sim.disc_mutex ) == 0 )
    {
      g_sim.prim_disc_mutex = false;
    }
    else
    {
      all_destroyed = false;
    }
  }

  return all_destroyed;
}

/* Result of creating the per-session synchronization objects.  The distinction
 * between a cleanup that succeeded and one that left objects behind matters for
 * the safe-retry contract: if a primitive cannot be destroyed the failing init
 * must keep the session flag set so a later wifi_hal_deinit() re-attempts the
 * release (see wifi_hal_init). */
typedef enum
{
  SESSION_PRIMITIVES_OK = 0,   /**< All primitives created and alive.      */
  SESSION_PRIMITIVES_CLEANED,  /**< Creation failed, everything released.  */
  SESSION_PRIMITIVES_PARTIAL,  /**< Creation failed, some primitives left. */
} session_primitives_rc_t;

/* Create every per-session synchronization object, flagging each one as alive
 * immediately after it is created.  On failure the objects created so far are
 * destroyed (via the flag-driven destroyer).  If every primitive was released
 * the session is left without live primitives and SESSION_PRIMITIVES_CLEANED is
 * returned; if a destroy failed the flags that were not cleared stay set so the
 * caller can keep lifecycle state for a safe retry. */
static session_primitives_rc_t _session_primitives_init( void )
{
  if ( pthread_mutex_init( &g_sim.disc_mutex, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_disc_mutex = true;

  if ( pthread_cond_init( &g_sim.disc_cond, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_disc_cond = true;

  if ( pthread_mutex_init( &g_sim.conn_mutex, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_conn_mutex = true;

  if ( pthread_cond_init( &g_sim.conn_cond, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_conn_cond = true;

  if ( pthread_mutex_init( &g_sim.state_mutex, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_state_mutex = true;

  if ( pthread_mutex_init( &g_sim.cb_mutex, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_cb_mutex = true;

  if ( pthread_cond_init( &g_sim.cb_cond, NULL ) != 0 )
  {
    return _session_primitives_destroy() ? SESSION_PRIMITIVES_CLEANED
                                         : SESSION_PRIMITIVES_PARTIAL;
  }
  g_sim.prim_cb_cond = true;

  return SESSION_PRIMITIVES_OK;
}

/* Disable callback delivery, wait for every in-flight callback to return and
 * clear the callback/user-data pointers.  Must be called while the per-session
 * primitives (in particular state_mutex and cb_mutex) are still alive, and
 * after all timer threads have been stopped and joined. */
static void _quiesce_callbacks( void )
{
  pthread_mutex_lock( &g_sim.cb_mutex );
  g_sim.cb_disabled = true;
  while ( g_sim.cb_in_flight > 0 )
  {
    pthread_cond_wait( &g_sim.cb_cond, &g_sim.cb_mutex );
  }
  /* Clear callback state before the per-session state mutex is destroyed. */
  g_sim.event_cb  = NULL;
  g_sim.user_data = NULL;
  pthread_mutex_unlock( &g_sim.cb_mutex );
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

  pthread_mutex_lock( &g_lifecycle_mutex );

  /* Idempotent: a second init of an already initialized session must not
   * memset or re-create live synchronization objects. */
  if ( g_hal_inited )
  {
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_SUCCESS;
  }

  memset( &g_sim, 0, sizeof( g_sim ) );

  /* Allocate the per-session synchronization objects first.  If any step
   * below fails the session is unwound back to uninitialized so a later deinit
   * (or a retried init after deinit) never sees live resources.  If a
   * primitive cannot even be destroyed the session flag is kept so a later
   * wifi_hal_deinit() re-attempts the release (safe-retry contract). */
  if ( _session_primitives_init() != SESSION_PRIMITIVES_OK )
  {
    if ( _session_primitives_destroy() == false )
    {
      /* Some primitive could not be released: keep the lifecycle state so a
       * later wifi_hal_deinit() can retry the destruction.  No callback can be
       * delivered (event_cb is NULL) and no thread is running, so the state is
       * quiescent even though the release is incomplete. */
      g_hal_inited = true;
      osal_log_error( "[wifi-sim] init unwind incomplete, retry via deinit" );
    }
    else
    {
      memset( &g_sim, 0, sizeof( g_sim ) );
    }
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_ERROR;
  }

  /* Optional AP DNS override: validate it when supplied, otherwise store as
   * omitted (non-captive DHCP keeps the platform default DNS). */
  g_sim.ap_dns_set = ( init->ap_dns && init->ap_dns[0] != '\0' );
  if ( g_sim.ap_dns_set )
  {
    if ( !wifi_hal_is_valid_ipv4( init->ap_dns ) )
    {
      osal_log_error( "[wifi-sim] invalid AP DNS \"%s\"", init->ap_dns );
      /* Partial init: release the primitives allocated above.  When a release
       * fails the session flag is kept so a later wifi_hal_deinit() retries;
       * when every primitive is released the session is fully uninitialized
       * and deinit remains a safe no-op. */
      if ( !_session_primitives_destroy() )
      {
        g_hal_inited = true;
        osal_log_error( "[wifi-sim] init unwind incomplete, retry via deinit" );
        pthread_mutex_unlock( &g_lifecycle_mutex );
        return OSAL_ERR_INVALID_ARGUMENT;
      }
      memset( &g_sim, 0, sizeof( g_sim ) );
      pthread_mutex_unlock( &g_lifecycle_mutex );
      return OSAL_ERR_INVALID_ARGUMENT;
    }
    strncpy( g_sim.ap_dns, init->ap_dns, sizeof( g_sim.ap_dns ) - 1 );
  }
  else
  {
    g_sim.ap_dns[0] = '\0';
  }

  pthread_mutex_lock( &g_sim.cb_mutex );
  g_sim.event_cb     = init->event_cb;
  g_sim.user_data    = init->user_data;
  g_sim.cb_disabled  = false;
  g_sim.cb_in_flight = 0;
  pthread_mutex_unlock( &g_sim.cb_mutex );

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.initialized = true;
  pthread_mutex_unlock( &g_sim.state_mutex );

  g_hal_inited = true;
  pthread_mutex_unlock( &g_lifecycle_mutex );

  osal_log_info( "[wifi-sim] HAL initialized (%zu simulated APs)", SIM_AP_COUNT );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit( void )
{
  pthread_mutex_lock( &g_lifecycle_mutex );

  /* Uninitialized and repeated deinit are successful no-ops: there are no
   * per-session primitives to touch, so nothing is locked or destroyed. */
  if ( !g_hal_inited )
  {
    pthread_mutex_unlock( &g_lifecycle_mutex );
    osal_log_info( "[wifi-sim] HAL deinit: not initialized (no-op)" );
    return OSAL_SUCCESS;
  }

  /* 1. Stop and join both timer threads while their per-session mutexes and
   *    conditions are still alive.  After this no timer-driven callback can
   *    begin and any callback the timer threads had started has returned.  On a
   *    retry after a partial resource release the threads were already stopped
   *    in the first pass and some primitives may already be gone, so only run
   *    while the primitives the stop path needs still exist. */
  if ( g_sim.prim_state_mutex && g_sim.prim_disc_mutex && g_sim.prim_disc_cond &&
       g_sim.prim_conn_mutex && g_sim.prim_conn_cond )
  {
    _stop_disconnect_timer_if_active();
    _stop_connect_timer_if_active();
  }

  /* 2. Disable callback delivery, wait for every in-flight callback to return
   *    and clear the callback/user-data pointers — before the per-session state
   *    mutex below is destroyed.  Once the callback gate was released (it was,
   *    on any retry that reached step 3 before) delivery is already disabled,
   *    drained and cleared, so a retry must not touch the destroyed gate. */
  if ( g_sim.prim_cb_mutex && g_sim.prim_cb_cond )
  {
    _quiesce_callbacks();
  }

  /* 3. Clear the remaining connection state while it is still protected. */
  if ( g_sim.prim_state_mutex )
  {
    pthread_mutex_lock( &g_sim.state_mutex );
    g_sim.initialized = false;
    g_sim.started     = false;
    g_sim.connected   = false;
    g_sim.active_ap   = NULL;
    pthread_mutex_unlock( &g_sim.state_mutex );
  }

  /* 4. Destroy the per-session synchronization objects.  No thread references
   *    them anymore: timer threads were stopped and joined, and delivery is
   *    quiescent.  If a release fails, the primitive stays flagged as alive and
   *    the session flag is kept so a later retry of wifi_hal_deinit() can
   *    attempt it again; callback delivery remains disabled throughout. */
  if ( !_session_primitives_destroy() )
  {
    osal_log_error( "[wifi-sim] HAL deinit: failed to destroy a primitive" );
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_ERROR;
  }

  memset( &g_sim, 0, sizeof( g_sim ) );
  g_hal_inited = false;
  pthread_mutex_unlock( &g_lifecycle_mutex );

  osal_log_info( "[wifi-sim] HAL deinitialized" );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  if ( !g_sim.initialized )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.mode    = mode;
  g_sim.started = true;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_info( "[wifi-sim] started, mode=%d", (int) mode );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop( void )
{
  _stop_disconnect_timer_if_active();
  _stop_connect_timer_if_active();

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.started   = false;
  g_sim.connected = false;
  g_sim.active_ap = NULL;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_info( "[wifi-sim] stopped" );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.sta_cfg = *config;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_debug( "[wifi-sim] STA config: ssid=\"%s\"", config->ssid );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.ap_cfg = *config;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_debug( "[wifi-sim] AP config: ssid=\"%s\"", config->ssid );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  bool started;
  char ssid_local[WIFI_HAL_SSID_MAX_LEN] = {0};
  char pass_local[WIFI_HAL_PASSWORD_MAX_LEN] = {0};

  pthread_mutex_lock( &g_sim.state_mutex );
  started = g_sim.started;
  strncpy( ssid_local, g_sim.sta_cfg.ssid, sizeof( ssid_local ) - 1 );
  strncpy( pass_local, g_sim.sta_cfg.password, sizeof( pass_local ) - 1 );
  pthread_mutex_unlock( &g_sim.state_mutex );

  if ( !started )
  {
    osal_log_warning( "[wifi-sim] connect called but not started" );
    return OSAL_ERROR;
  }

  const sim_ap_t* ap = _find_ap( ssid_local );
  if ( !ap )
  {
    osal_log_warning( "[wifi-sim] SSID \"%s\" not found in simulated environment",
                      ssid_local );
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
      if ( strncmp( pass_local, ap->password,
                    WIFI_HAL_PASSWORD_MAX_LEN ) != 0 )
      {
        osal_log_info( "[wifi-sim] \"%s\" — wrong password", ap->ssid );
        return OSAL_ERROR;
      }
      /* Correct password — fall through to normal connect. */
      /* fallthrough */

    case AP_BEHAV_NORMAL:
    {
      pthread_mutex_lock( &g_sim.state_mutex );
      g_sim.connected = true;
      g_sim.active_ap = ap;
      pthread_mutex_unlock( &g_sim.state_mutex );

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
      pthread_mutex_lock( &g_sim.state_mutex );
      g_sim.connected = true;
      g_sim.active_ap = ap;
      pthread_mutex_unlock( &g_sim.state_mutex );

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
      pthread_mutex_lock( &g_sim.state_mutex );
      g_sim.active_ap = ap;
      pthread_mutex_unlock( &g_sim.state_mutex );

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
  pthread_mutex_lock( &g_sim.state_mutex );
  bool was_connected = g_sim.connected;
  const sim_ap_t* was_ap = g_sim.active_ap;
  if ( was_connected )
  {
    osal_log_info( "[wifi-sim] disconnected from \"%s\"",
                   was_ap ? was_ap->ssid : "?" );
  }

  g_sim.connected = false;
  g_sim.active_ap = NULL;
  pthread_mutex_unlock( &g_sim.state_mutex );
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

  pthread_mutex_lock( &g_sim.state_mutex );
  bool connected = g_sim.connected;
  pthread_mutex_unlock( &g_sim.state_mutex );

  if ( !connected )
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
  pthread_mutex_lock( &g_sim.state_mutex );
  bool connected = g_sim.connected;
  const sim_ap_t* ap = g_sim.active_ap;
  pthread_mutex_unlock( &g_sim.state_mutex );

  if ( !connected || !ap )
  {
    *out_rssi = 0;
    return OSAL_ERROR;
  }

  *out_rssi = ap->rssi;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.power_save = enabled;
  pthread_mutex_unlock( &g_sim.state_mutex );
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
  pthread_mutex_lock( &g_sim.state_mutex );
  *out_client_count = g_sim.client_cnt;
  pthread_mutex_unlock( &g_sim.state_mutex );
  return OSAL_SUCCESS;
}
