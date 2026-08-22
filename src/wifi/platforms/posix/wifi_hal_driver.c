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
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/*  Test-only failure injection (WIFI_HAL_POSIX_TESTING)                       */
/* -------------------------------------------------------------------------- */

#ifdef WIFI_HAL_POSIX_TESTING

/* The real pthread entry points are captured BEFORE the names below are
 * redirected, so the wrappers can call through to the actual APIs.  Production
 * builds (without WIFI_HAL_POSIX_TESTING) never see this section and call the
 * real pthread APIs directly. */
static int ( *g_real_pthread_create )( pthread_t*, const pthread_attr_t*, void* ( * )( void* ), void* ) = pthread_create;
static int ( *g_real_pthread_join )( pthread_t, void** )                                                = pthread_join;
static int ( *g_real_pthread_mutex_init )( pthread_mutex_t*, const pthread_mutexattr_t* )               = pthread_mutex_init;
static int ( *g_real_pthread_cond_init )( pthread_cond_t*, const pthread_condattr_t* )                  = pthread_cond_init;
static int ( *g_real_pthread_mutex_destroy )( pthread_mutex_t* )                                        = pthread_mutex_destroy;
static int ( *g_real_pthread_cond_destroy )( pthread_cond_t* )                                          = pthread_cond_destroy;

/* Countdown of successful calls before the next injected failure (-1 = off). */
static int32_t g_fail_create_after        = -1;
static int32_t g_fail_join_after          = -1;
static int32_t g_fail_mutex_init_after    = -1;
static int32_t g_fail_cond_init_after     = -1;
static int32_t g_fail_mutex_destroy_after = -1;
static int32_t g_fail_cond_destroy_after  = -1;

static bool _test_inject( int32_t* counter )
{
  if ( *counter < 0 )
  {
    return false;
  }
  if ( *counter == 0 )
  {
    *counter = -1;
    return true;
  }
  ( *counter )--;
  return false;
}

static int _wifi_hal_test_pthread_create( pthread_t*                       thread,
                                          const pthread_attr_t*           attr,
                                          void* ( *start )( void* ),
                                          void*                           arg )
{
  if ( _test_inject( &g_fail_create_after ) )
  {
    return EAGAIN;
  }
  return g_real_pthread_create( thread, attr, start, arg );
}

static int _wifi_hal_test_pthread_join( pthread_t thread, void** retval )
{
  if ( _test_inject( &g_fail_join_after ) )
  {
    return EPERM;
  }
  return g_real_pthread_join( thread, retval );
}

static int _wifi_hal_test_pthread_mutex_init( pthread_mutex_t* mutex, const pthread_mutexattr_t* attr )
{
  if ( _test_inject( &g_fail_mutex_init_after ) )
  {
    return EAGAIN;
  }
  return g_real_pthread_mutex_init( mutex, attr );
}

static int _wifi_hal_test_pthread_cond_init( pthread_cond_t* cond, const pthread_condattr_t* attr )
{
  if ( _test_inject( &g_fail_cond_init_after ) )
  {
    return EAGAIN;
  }
  return g_real_pthread_cond_init( cond, attr );
}

static int _wifi_hal_test_pthread_mutex_destroy( pthread_mutex_t* mutex )
{
  if ( _test_inject( &g_fail_mutex_destroy_after ) )
  {
    return EBUSY;
  }
  return g_real_pthread_mutex_destroy( mutex );
}

static int _wifi_hal_test_pthread_cond_destroy( pthread_cond_t* cond )
{
  if ( _test_inject( &g_fail_cond_destroy_after ) )
  {
    return EBUSY;
  }
  return g_real_pthread_cond_destroy( cond );
}

/* Test API (declared extern by the regression tests). */
void wifi_hal_testing_fail_pthread_create_after( int32_t successes )
{
  g_fail_create_after = successes;
}

void wifi_hal_testing_fail_pthread_join_after( int32_t successes )
{
  g_fail_join_after = successes;
}

void wifi_hal_testing_fail_mutex_init_after( int32_t successes )
{
  g_fail_mutex_init_after = successes;
}

void wifi_hal_testing_fail_cond_init_after( int32_t successes )
{
  g_fail_cond_init_after = successes;
}

void wifi_hal_testing_fail_mutex_destroy_after( int32_t successes )
{
  g_fail_mutex_destroy_after = successes;
}

void wifi_hal_testing_fail_cond_destroy_after( int32_t successes )
{
  g_fail_cond_destroy_after = successes;
}

void wifi_hal_testing_reset( void )
{
  g_fail_create_after        = -1;
  g_fail_join_after          = -1;
  g_fail_mutex_init_after    = -1;
  g_fail_cond_init_after     = -1;
  g_fail_mutex_destroy_after = -1;
  g_fail_cond_destroy_after  = -1;
}

#define pthread_create        _wifi_hal_test_pthread_create
#define pthread_join          _wifi_hal_test_pthread_join
#define pthread_mutex_init    _wifi_hal_test_pthread_mutex_init
#define pthread_cond_init     _wifi_hal_test_pthread_cond_init
#define pthread_mutex_destroy _wifi_hal_test_pthread_mutex_destroy
#define pthread_cond_destroy  _wifi_hal_test_pthread_cond_destroy

#endif /* WIFI_HAL_POSIX_TESTING */

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
  const char*        ssid;
  const char*        password;
  int                channel;
  int                rssi;
  int                authmode;
  sim_ap_behaviour_t behaviour;
  uint32_t           param_ms; /**< Delay / disconnect time in ms. */
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
#define SIM_IP      "192.168.1.100"
#define SIM_NETMASK "255.255.255.0"
#define SIM_GATEWAY "192.168.1.1"

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

  const sim_ap_t*       active_ap;  /**< AP we're connected to (or NULL). */

  /* Disconnect timer thread.  disc_thread_created is published under the
   * process-lifetime disconnect-timer gate while pthread_create() is still in
   * the same critical section (see _start_disconnect_timer), so no observer
   * can ever see a live thread with the flag still false — every successful
   * create is therefore guaranteed to be joined.  The flag is cleared only
   * after pthread_join() succeeds, so a failed join keeps the exact same live
   * handle for a later deinit retry. */
  pthread_t             disc_thread;
  bool                  disc_thread_created;
  bool                  disc_cancel;
  pthread_mutex_t       disc_mutex;
  pthread_cond_t        disc_cond;
  uint32_t              disc_delay_ms;

  /* Slow-connect thread (same created/join discipline as the disconnect timer). */
  pthread_t             conn_thread;
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

/* Timer lifecycle gates are process-lifetime objects.  They serialize the
 * complete stop/read/create/handle-publication sequence for each timer.  In
 * particular, two admitted operations cannot overwrite one joinable handle,
 * and deinit cannot inspect a timer between pthread_create() and publication
 * of its created flag. */
static pthread_mutex_t g_disc_timer_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_conn_timer_gate = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------- */
/*  Process-lifetime lifecycle state                                          */
/* -------------------------------------------------------------------------- */

typedef enum
{
  HAL_STATE_UNINITIALIZED   = 0, /**< No session; init may create one.       */
  HAL_STATE_ACTIVE,              /**< Session up; operation admission open.  */
  HAL_STATE_DEINITIALIZING,      /**< A deinit owns the transition; admission closed. */
  HAL_STATE_CLEANUP_REQUIRED,    /**< Teardown incomplete; admission closed; deinit retry owns cleanup. */
} hal_state_t;

/* One process-lifetime lifecycle mutex/condition pair: statically initialized,
 * never destroyed, so init and deinit can be serialized even across a session
 * boundary and deinit can be recognized as a no-op before any per-session
 * primitive exists.  g_lifecycle_state transitions are owned by exactly one
 * init/deinit caller at a time; g_admitted_ops counts public operations that
 * hold a session lease. */
static pthread_mutex_t g_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_lifecycle_cond  = PTHREAD_COND_INITIALIZER;
static hal_state_t     g_lifecycle_state = HAL_STATE_UNINITIALIZED;
static uint32_t        g_admitted_ops    = 0;

#ifdef WIFI_HAL_POSIX_TESTING
/* Deterministic test rendezvous for the real lifecycle transition.  This
 * waits until deinit has closed admission, rather than guessing from the
 * point at which a deinit worker was scheduled. */
void wifi_hal_testing_wait_for_deinit_started( void )
{
  pthread_mutex_lock( &g_lifecycle_mutex );
  while ( g_lifecycle_state != HAL_STATE_DEINITIALIZING )
  {
    pthread_cond_wait( &g_lifecycle_cond, &g_lifecycle_mutex );
  }
  pthread_mutex_unlock( &g_lifecycle_mutex );
}
#endif

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

/* Fire one event, invoking the management callback outside every HAL lock.
 * The callback gate counts an admitted delivery as in flight, copies the
 * function/user-data pair under the gate, invokes the callback unlocked, then
 * acknowledges completion under the gate. */
static void _fire_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  pthread_mutex_lock( &g_sim.cb_mutex );

  /* Deinit either disabled delivery or will do so under this lock; a delivery
   * that starts after the disabled flag is seen drops the event. */
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
   * management APIs without deadlocking.  Deinit waits for cb_in_flight to
   * drain, which makes every already-started callback complete before deinit
   * returns. */
  cb( event, data, user );

  pthread_mutex_lock( &g_sim.cb_mutex );
  g_sim.cb_in_flight--;
  if ( g_sim.cb_in_flight == 0 )
  {
    pthread_cond_signal( &g_sim.cb_cond );
  }
  pthread_mutex_unlock( &g_sim.cb_mutex );
}

/* ---------------------------------------------------------- */
/*  Operation admission (session lease)                        */
/* ---------------------------------------------------------- */

/* Grants a session lease to a public operation.  Returns true and counts the
 * operation when the HAL is ACTIVE; otherwise returns false WITHOUT touching
 * any per-session primitive (the caller must fail fast).  Every granted lease
 * must be released with _release_operation() on every return path, so deinit
 * can close admission and wait for the drain before destroying session-owned
 * primitives.  A callback that re-enters a HAL API while teardown is in
 * progress gets the rejection instead of deadlocking. */
static bool _admit_operation( void )
{
  pthread_mutex_lock( &g_lifecycle_mutex );
  bool admitted = ( g_lifecycle_state == HAL_STATE_ACTIVE );
  if ( admitted )
  {
    g_admitted_ops++;
  }
  pthread_mutex_unlock( &g_lifecycle_mutex );
  return admitted;
}

static void _release_operation( void )
{
  pthread_mutex_lock( &g_lifecycle_mutex );
  g_admitted_ops--;
  if ( g_admitted_ops == 0 )
  {
    /* A deinit waiting on the drain may now proceed. */
    pthread_cond_broadcast( &g_lifecycle_cond );
  }
  pthread_mutex_unlock( &g_lifecycle_mutex );
}

static void _disable_callback_delivery( void );

/* Mark the session as needing a teardown retry after a join/primitive failure
 * detected outside deinit (public stop/disconnect).  Admission and callback
 * delivery are both closed before the operation reports the failed cleanup;
 * the retained session is released only by a later deinit. */
static void _lifecycle_mark_cleanup_required( void )
{
  pthread_mutex_lock( &g_lifecycle_mutex );
  if ( g_lifecycle_state == HAL_STATE_ACTIVE )
  {
    g_lifecycle_state = HAL_STATE_CLEANUP_REQUIRED;
    pthread_cond_broadcast( &g_lifecycle_cond );
  }
  pthread_mutex_unlock( &g_lifecycle_mutex );

  _disable_callback_delivery();
}

/* -------------------------------------------------------------------------- */
/*  Timer threads                                                             */
/* -------------------------------------------------------------------------- */

static osal_status_t _stop_disconnect_timer( void );
static osal_status_t _stop_connect_timer( void );

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
  return NULL;
}

/* Stop and join the disconnect timer.  Joins whenever a thread was ever
 * started, even when it already completed on its own: a completed thread
 * remains joinable and its resources are only reclaimed by pthread_join().
 * The created flag is cleared only after a successful join; on a real join
 * failure the thread may still be live, so the flag (and per-session
 * primitives) are retained for a deinit retry. */
static osal_status_t _stop_disconnect_timer_locked( void )
{
  /* The caller owns g_disc_timer_gate.  The gate covers the flag read,
   * cancellation, join, and flag clear; it is process-lifetime precisely
   * because session primitives cannot be used as a gate during teardown or
   * partial-init unwind. */
  bool created = g_sim.disc_thread_created;
  if ( !created )
  {
    return OSAL_SUCCESS;
  }

  /* A thread can only exist in a fully initialized session.  If a prior
   * primitive release left state_mutex unavailable, retain the created flag
   * and report failure rather than locking an uninitialized object. */
  if ( !g_sim.prim_state_mutex || !g_sim.prim_disc_mutex || !g_sim.prim_disc_cond )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.disc_mutex );
  g_sim.disc_cancel = true;
  pthread_cond_signal( &g_sim.disc_cond );
  pthread_mutex_unlock( &g_sim.disc_mutex );

  int rc = pthread_join( g_sim.disc_thread, NULL );

  /* Any non-zero result is a failed join.  The handle may still identify a
   * live or joinable thread, so retain both it and the created flag for a
   * later deinit retry; never treat an error as proof that the resources were
   * reclaimed. */
  if ( rc != 0 )
  {
    osal_log_error( "[wifi-sim] disconnect timer join failed (%d)", rc );
    return OSAL_ERROR;
  }

  /* Clear the created flag only after pthread_join() succeeds. */
  g_sim.disc_thread_created = false;
  return OSAL_SUCCESS;
}

static osal_status_t _stop_disconnect_timer( void )
{
  pthread_mutex_lock( &g_disc_timer_gate );
  osal_status_t rc = _stop_disconnect_timer_locked();
  pthread_mutex_unlock( &g_disc_timer_gate );
  return rc;
}

static osal_status_t _start_disconnect_timer( uint32_t delay_ms )
{
  /* Keep the gate across stop, configuration, create, and publication. */
  pthread_mutex_lock( &g_disc_timer_gate );
  if ( _stop_disconnect_timer_locked() != OSAL_SUCCESS )
  {
    pthread_mutex_unlock( &g_disc_timer_gate );
    /* This path is also used by an admitted public connect operation, not
     * only by deinit.  A failed join therefore closes admission immediately
     * and leaves the session for deinit retry. */
    _lifecycle_mark_cleanup_required();
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.disc_mutex );
  g_sim.disc_cancel   = false;
  g_sim.disc_delay_ms = delay_ms;
  pthread_mutex_unlock( &g_sim.disc_mutex );

  /* Publish the handle state before releasing the process-lifetime gate.
   * Teardown waits for the admitting operation, so it cannot destroy the
   * session while this sequence owns the gate. */
  g_sim.disc_thread_created = false;
  int rc = pthread_create( &g_sim.disc_thread, NULL, _disconnect_thread, NULL );
  if ( rc == 0 )
  {
    g_sim.disc_thread_created = true;
  }
  pthread_mutex_unlock( &g_disc_timer_gate );

  if ( rc != 0 )
  {
    osal_log_warning( "[wifi-sim] failed to create disconnect timer thread" );
    return OSAL_ERROR;
  }
  return OSAL_SUCCESS;
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
  return NULL;
}

static osal_status_t _stop_connect_timer_locked( void )
{
  /* Caller owns g_conn_timer_gate; this serializes every access to the
   * joinable handle and created flag with timer creation. */
  bool created = g_sim.conn_thread_created;
  if ( !created )
  {
    return OSAL_SUCCESS;
  }

  /* Never lock a session object that was not successfully initialized or was
   * already released by an earlier cleanup attempt. */
  if ( !g_sim.prim_state_mutex || !g_sim.prim_conn_mutex || !g_sim.prim_conn_cond )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.conn_mutex );
  g_sim.conn_cancel = true;
  pthread_cond_signal( &g_sim.conn_cond );
  pthread_mutex_unlock( &g_sim.conn_mutex );

  int rc = pthread_join( g_sim.conn_thread, NULL );

  /* Clear the created flag only after pthread_join() reports success.  On any
   * error retain the handle and all session primitives for a safe deinit
   * retry. */
  if ( rc != 0 )
  {
    osal_log_error( "[wifi-sim] slow-connect timer join failed (%d)", rc );
    return OSAL_ERROR;
  }

  g_sim.conn_thread_created = false;
  return OSAL_SUCCESS;
}

static osal_status_t _stop_connect_timer( void )
{
  pthread_mutex_lock( &g_conn_timer_gate );
  osal_status_t rc = _stop_connect_timer_locked();
  pthread_mutex_unlock( &g_conn_timer_gate );
  return rc;
}

static osal_status_t _start_slow_connect( uint32_t delay_ms )
{
  pthread_mutex_lock( &g_conn_timer_gate );
  if ( _stop_connect_timer_locked() != OSAL_SUCCESS )
  {
    pthread_mutex_unlock( &g_conn_timer_gate );
    _lifecycle_mark_cleanup_required();
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.conn_mutex );
  g_sim.conn_cancel   = false;
  g_sim.conn_delay_ms = delay_ms;
  pthread_mutex_unlock( &g_sim.conn_mutex );

  /* Same atomic create/publish discipline as the disconnect timer.  The
   * per-timer gate remains held from stop through this publication. */
  g_sim.conn_thread_created = false;
  int rc = pthread_create( &g_sim.conn_thread, NULL, _slow_connect_thread, NULL );
  if ( rc == 0 )
  {
    g_sim.conn_thread_created = true;
  }
  pthread_mutex_unlock( &g_conn_timer_gate );

  if ( rc != 0 )
  {
    osal_log_warning( "[wifi-sim] failed to create slow-connect thread" );
    return OSAL_ERROR;
  }
  return OSAL_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/*  Session lifecycle helpers                                                 */
/* -------------------------------------------------------------------------- */

/* Destroy every per-session synchronization primitive that is still flagged as
 * alive, clearing the flag when the release succeeds.  Used by the partial-init
 * unwind, by deinit and by a deinit retry.  Returns false if any release
 * failed — the flag of that primitive is kept so a later retry can attempt it
 * again without ever touching a primitive that is already gone.  Runs in the
 * exact reverse order of creation. */
static bool _session_primitives_destroy( void )
{
  bool all_destroyed = true;

  /* init order: disc_mutex, disc_cond, conn_mutex, conn_cond, state_mutex,
   *             cb_mutex, cb_cond
   * destroy order is the exact reverse:
   *             cb_cond, cb_mutex, state_mutex, conn_cond, conn_mutex,
   *             disc_cond, disc_mutex */
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
  SESSION_PRIMITIVES_OK      = 0, /**< All primitives created and alive.      */
  SESSION_PRIMITIVES_CLEANED,     /**< Creation failed, everything released.  */
  SESSION_PRIMITIVES_PARTIAL,     /**< Creation failed, some primitives left. */
} session_primitives_rc_t;

/* Create every per-session synchronization object, flagging each one as alive
 * immediately after it is created.  On failure the objects created so far are
 * destroyed (via the flag-driven destroyer, in exact reverse order).  If every
 * primitive was released SESSION_PRIMITIVES_CLEANED is returned; if a destroy
 * failed the flags that were not cleared stay set so the caller can keep
 * lifecycle state for a safe retry. */
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

/* Best-effort close of the callback gate used when teardown is aborted (e.g.
 * a join failed).  The gate primitives are still alive at that point, so new
 * deliveries are dropped; the already-running callback is left to drain. */
static void _disable_callback_delivery( void )
{
  if ( g_sim.prim_cb_mutex )
  {
    pthread_mutex_lock( &g_sim.cb_mutex );
    g_sim.cb_disabled = true;
    pthread_mutex_unlock( &g_sim.cb_mutex );
  }
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

  /* Wait for an in-progress teardown, then re-check the state.  A retained
   * CLEANUP_REQUIRED session is not replaceable: only a later deinit may own
   * and finish that cleanup, so init fails without touching its primitives. */
  while ( g_lifecycle_state == HAL_STATE_DEINITIALIZING )
  {
    pthread_cond_wait( &g_lifecycle_cond, &g_lifecycle_mutex );
  }

  if ( g_lifecycle_state == HAL_STATE_CLEANUP_REQUIRED )
  {
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_ERROR;
  }

  if ( g_lifecycle_state == HAL_STATE_ACTIVE )
  {
    /* Idempotent: a second init of an already initialized session must not
     * memset or re-create live synchronization objects. */
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_SUCCESS;
  }

  /* UNINITIALIZED: this caller owns the transition to ACTIVE.  No live
   * resources exist, so a clean memset is safe. */
  memset( &g_sim, 0, sizeof( g_sim ) );

  /* Allocate the per-session synchronization objects first.  If any step fails
   * the objects created so far are released in exact reverse order.  When the
   * unwind itself fails the validity flags that were not cleared stay set and
   * the lifecycle state is kept (CLEANUP_REQUIRED) so a later deinit retries
   * the release; a successful unwind leaves the session fully uninitialized. */
  session_primitives_rc_t prim_rc = _session_primitives_init();
  if ( prim_rc == SESSION_PRIMITIVES_PARTIAL )
  {
    osal_log_error( "[wifi-sim] init unwind incomplete, retry via deinit" );
    g_lifecycle_state = HAL_STATE_CLEANUP_REQUIRED;
    pthread_cond_broadcast( &g_lifecycle_cond );
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_ERROR;
  }
  if ( prim_rc == SESSION_PRIMITIVES_CLEANED )
  {
    memset( &g_sim, 0, sizeof( g_sim ) );
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_ERROR;
  }

  /* Optional AP DNS override: validate it when supplied, otherwise store as
   * omitted (non-captive DHCP keeps the platform default DNS). */
  g_sim.ap_dns_set = ( init->ap_dns && init->ap_dns[0] != '\0' );
  if ( g_sim.ap_dns_set && !wifi_hal_is_valid_ipv4( init->ap_dns ) )
  {
    osal_log_error( "[wifi-sim] invalid AP DNS \"%s\"", init->ap_dns );
    if ( !_session_primitives_destroy() )
    {
      osal_log_error( "[wifi-sim] init unwind incomplete, retry via deinit" );
      g_lifecycle_state = HAL_STATE_CLEANUP_REQUIRED;
      pthread_cond_broadcast( &g_lifecycle_cond );
      pthread_mutex_unlock( &g_lifecycle_mutex );
      return OSAL_ERR_INVALID_ARGUMENT;
    }
    memset( &g_sim, 0, sizeof( g_sim ) );
    pthread_mutex_unlock( &g_lifecycle_mutex );
    return OSAL_ERR_INVALID_ARGUMENT;
  }
  if ( g_sim.ap_dns_set )
  {
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

  /* Publish the session only after every owned resource is allocated. */
  g_lifecycle_state = HAL_STATE_ACTIVE;
  pthread_cond_broadcast( &g_lifecycle_cond );
  pthread_mutex_unlock( &g_lifecycle_mutex );

  osal_log_info( "[wifi-sim] HAL initialized (%zu simulated APs)", SIM_AP_COUNT );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit( void )
{
  pthread_mutex_lock( &g_lifecycle_mutex );

  /* Only one init/deinit caller owns a lifecycle transition.  Uninitialized
   * and already-finished deinit are successful no-ops. */
  for ( ;; )
  {
    if ( g_lifecycle_state == HAL_STATE_UNINITIALIZED )
    {
      pthread_mutex_unlock( &g_lifecycle_mutex );
      osal_log_info( "[wifi-sim] HAL deinit: not initialized (no-op)" );
      return OSAL_SUCCESS;
    }
    if ( g_lifecycle_state == HAL_STATE_DEINITIALIZING )
    {
      /* Another deinit owns the teardown: wait and re-check. */
      pthread_cond_wait( &g_lifecycle_cond, &g_lifecycle_mutex );
      continue;
    }
    /* ACTIVE or CLEANUP_REQUIRED: this caller owns the transition.  Close
     * admission immediately so no new operation can touch session resources. */
    g_lifecycle_state = HAL_STATE_DEINITIALIZING;
    pthread_cond_broadcast( &g_lifecycle_cond );
    break;
  }

  /* Wait for every admitted operation to finish before destroying
   * session-owned primitives.  The lifecycle mutex is released while waiting
   * (pthread_cond_wait), so an operation that arrives now just fails fast in
   * _admit_operation() and an init caller waits on the same condition. */
  while ( g_admitted_ops > 0 )
  {
    pthread_cond_wait( &g_lifecycle_cond, &g_lifecycle_mutex );
  }

  pthread_mutex_unlock( &g_lifecycle_mutex );

  /* ---- Teardown runs without the process-lifetime lock ----.
   * From here no operation is admitted and no new event source can start, so
   * only this deinit touches the session until the state is published again. */
  bool ok = true;

  /* 1. Stop and join both timer threads while their per-session mutexes and
   *    conditions are still alive (a naturally completed thread stays joinable
   *    and is joined here too).  A failed join means the thread may still be
   *    running, so NOTHING below may be destroyed: the created flag and the
   *    primitives are retained, callback admission is already closed and a
   *    later wifi_hal_deinit retries the exact same join. */
  /* The stop helpers perform the created-flag read under their per-timer
   * lifecycle gates.  They also fail closed if a retained created timer lacks
   * an object it might still access. */
  if ( _stop_disconnect_timer() != OSAL_SUCCESS )
  {
    ok = false;
  }
  if ( ok && _stop_connect_timer() != OSAL_SUCCESS )
  {
    ok = false;
  }

  if ( ok )
  {
    /* 2. Disable callback delivery, wait for every in-flight callback to
     *    return and clear the callback/user-data pointers — before the
     *    per-session state mutex below is destroyed. */
    if ( g_sim.prim_cb_mutex && g_sim.prim_cb_cond )
    {
      _quiesce_callbacks();
    }

    /* 3. Clear the remaining session state while it is still protected. */
    if ( g_sim.prim_state_mutex )
    {
      pthread_mutex_lock( &g_sim.state_mutex );
      g_sim.initialized = false;
      g_sim.started     = false;
      g_sim.connected   = false;
      g_sim.active_ap   = NULL;
      g_sim.power_save  = false;
      g_sim.client_cnt  = 0;
      pthread_mutex_unlock( &g_sim.state_mutex );
    }

    /* 4. Destroy the per-session synchronization objects (exact reverse order
     *    of creation).  A failed release keeps that primitive's validity flag
     *    so a later retry re-attempts only it. */
    if ( !_session_primitives_destroy() )
    {
      ok = false;
      osal_log_error( "[wifi-sim] HAL deinit: failed to destroy a primitive" );
    }
  }
  else
  {
    /* A join failed: the thread may still touch its primitives, so destroy
     * nothing; just close the callback gate so no new delivery can start. */
    _disable_callback_delivery();
  }

  pthread_mutex_lock( &g_lifecycle_mutex );
  if ( ok )
  {
    /* All owned resources are released; only now publish UNINITIALIZED. */
    memset( &g_sim, 0, sizeof( g_sim ) );
    g_lifecycle_state = HAL_STATE_UNINITIALIZED;
    osal_log_info( "[wifi-sim] HAL deinitialized" );
  }
  else
  {
    g_lifecycle_state = HAL_STATE_CLEANUP_REQUIRED;
    osal_log_error( "[wifi-sim] HAL deinit failed; call deinit again to finish cleanup" );
  }
  pthread_cond_broadcast( &g_lifecycle_cond );
  pthread_mutex_unlock( &g_lifecycle_mutex );

  return ok ? OSAL_SUCCESS : OSAL_ERROR;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.mode    = mode;
  g_sim.started = true;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_info( "[wifi-sim] started, mode=%d", (int) mode );

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop( void )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t rc = OSAL_SUCCESS;
  if ( _stop_disconnect_timer() != OSAL_SUCCESS || _stop_connect_timer() != OSAL_SUCCESS )
  {
    rc = OSAL_ERROR;
    _lifecycle_mark_cleanup_required();
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.started   = false;
  g_sim.connected = false;
  g_sim.active_ap = NULL;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_info( "[wifi-sim] stopped" );

  _release_operation();
  return rc;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.sta_cfg = *config;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_debug( "[wifi-sim] STA config: ssid=\"%s\"", config->ssid );

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.ap_cfg = *config;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_debug( "[wifi-sim] AP config: ssid=\"%s\"", config->ssid );

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t rc = OSAL_ERROR;
  bool          started;
  char          ssid_local[WIFI_HAL_SSID_MAX_LEN]        = { 0 };
  char          pass_local[WIFI_HAL_PASSWORD_MAX_LEN]    = { 0 };

  pthread_mutex_lock( &g_sim.state_mutex );
  started = g_sim.started;
  strncpy( ssid_local, g_sim.sta_cfg.ssid, sizeof( ssid_local ) - 1 );
  strncpy( pass_local, g_sim.sta_cfg.password, sizeof( pass_local ) - 1 );
  pthread_mutex_unlock( &g_sim.state_mutex );

  if ( !started )
  {
    osal_log_warning( "[wifi-sim] connect called but not started" );
    goto out;
  }

  const sim_ap_t* ap = _find_ap( ssid_local );
  if ( !ap )
  {
    osal_log_warning( "[wifi-sim] SSID \"%s\" not found in simulated environment",
                      ssid_local );
    goto out;
  }

  osal_log_info( "[wifi-sim] connecting to \"%s\" (behaviour=%d) ...",
                 ap->ssid, (int) ap->behaviour );

  switch ( ap->behaviour )
  {
    case AP_BEHAV_REJECT:
      osal_log_info( "[wifi-sim] \"%s\" — connection REJECTED", ap->ssid );
      break;

    case AP_BEHAV_WRONG_PASSWORD:
      if ( strncmp( pass_local, ap->password, WIFI_HAL_PASSWORD_MAX_LEN ) != 0 )
      {
        osal_log_info( "[wifi-sim] \"%s\" — wrong password", ap->ssid );
        break;
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
      rc = OSAL_SUCCESS;
      break;
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
      rc = _start_disconnect_timer( ap->param_ms );
      break;
    }

    case AP_BEHAV_SLOW_CONNECT:
    {
      pthread_mutex_lock( &g_sim.state_mutex );
      g_sim.active_ap = ap;
      pthread_mutex_unlock( &g_sim.state_mutex );

      osal_log_info( "[wifi-sim] \"%s\" — slow connect, IP in %u ms",
                     ap->ssid, (unsigned) ap->param_ms );

      /* GOT_IP will be fired asynchronously after the delay. */
      rc = _start_slow_connect( ap->param_ms );
      break;
    }
  }

out:
  _release_operation();
  return rc;
}

osal_status_t wifi_hal_disconnect( void )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t rc = OSAL_SUCCESS;
  if ( _stop_disconnect_timer() != OSAL_SUCCESS || _stop_connect_timer() != OSAL_SUCCESS )
  {
    rc = OSAL_ERROR;
    _lifecycle_mark_cleanup_required();
  }

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

  _release_operation();
  return rc;
}

osal_status_t wifi_hal_start_scan( bool block )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_log_debug( "[wifi-sim] scan started (block=%d)", (int) block );

  /* Notify management that scan is complete — results ready immediately. */
  _fire_event( WIFI_HAL_EVT_SCAN_DONE, NULL );

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !records || !in_out_count )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
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

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  bool connected = g_sim.connected;
  pthread_mutex_unlock( &g_sim.state_mutex );

  if ( !connected )
  {
    memset( out_info, 0, sizeof( *out_info ) );
    _release_operation();
    return OSAL_ERROR;
  }

  strncpy( out_info->ip,      SIM_IP,      sizeof( out_info->ip ) - 1 );
  strncpy( out_info->netmask, SIM_NETMASK, sizeof( out_info->netmask ) - 1 );
  strncpy( out_info->gw,      SIM_GATEWAY, sizeof( out_info->gw ) - 1 );

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_rssi( int* out_rssi )
{
  if ( !out_rssi )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  bool connected = g_sim.connected;
  const sim_ap_t* ap = g_sim.active_ap;
  pthread_mutex_unlock( &g_sim.state_mutex );

  if ( !connected || !ap )
  {
    *out_rssi = 0;
    _release_operation();
    return OSAL_ERROR;
  }

  *out_rssi = ap->rssi;

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  g_sim.power_save = enabled;
  pthread_mutex_unlock( &g_sim.state_mutex );
  osal_log_debug( "[wifi-sim] power save %s", enabled ? "ON" : "OFF" );

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] )
{
  if ( !mac )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  /* Deterministic simulated MAC address. */
  mac[0] = 0xDE;
  mac[1] = 0xAD;
  mac[2] = 0xBE;
  mac[3] = 0xEF;
  mac[4] = 0xCA;
  mac[5] = 0xFE;

  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count )
{
  if ( !out_client_count )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  pthread_mutex_lock( &g_sim.state_mutex );
  *out_client_count = g_sim.client_cnt;
  pthread_mutex_unlock( &g_sim.state_mutex );

  _release_operation();
  return OSAL_SUCCESS;
}
