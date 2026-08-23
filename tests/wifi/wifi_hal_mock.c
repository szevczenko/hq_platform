/*
 * Wi-Fi HAL Mock Driver for Unit Tests
 *
 * Provides controllable mock implementations of all wifi_hal_driver.h
 * functions.  Test code can configure predefined scan results, control
 * connect/disconnect outcomes, and inject events via the public helpers
 * declared in wifi_hal_mock.h.
 */

#include "wifi_hal_mock.h"

#include "osal_bin_sem.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include <stdbool.h>
#include <string.h>

/* ---- internal state ------------------------------------------------------ */

static wifi_hal_mock_state_t g_mock = { 0 };
static bool           g_hold_scan_done = false;
static bool           g_hold_init      = false;
static bool           g_hold_deinit    = false;
static osal_status_t  g_init_result    = OSAL_SUCCESS;
static osal_status_t  g_stop_result    = OSAL_SUCCESS;
static osal_status_t  g_deinit_result  = OSAL_SUCCESS;

/* Callback/user-data registration indicators exposed through the focused
 * lifecycle snapshot.  These are booleans only; callback pointer values are
 * never exposed through the snapshot API.  Protected by g_mock_mutex. */
static bool           g_event_cb_registered  = false;
static bool           g_user_data_registered = false;

/* These primitives have process lifetime.  They are created together by the
 * first reset, on the single-threaded test bootstrap path, and are never
 * lazily created or deleted while a worker can call the mock. */
static osal_mutex_id_t   g_mock_mutex           = NULL;
static osal_bin_sem_id_t g_init_entered_sem     = NULL;
static osal_bin_sem_id_t g_init_completed_sem   = NULL;
static osal_bin_sem_id_t g_init_release_sem     = NULL;
static osal_bin_sem_id_t g_stop_entered_sem     = NULL;
static osal_bin_sem_id_t g_stop_completed_sem   = NULL;
static osal_bin_sem_id_t g_deinit_entered_sem   = NULL;
static osal_bin_sem_id_t g_deinit_completed_sem = NULL;
static osal_bin_sem_id_t g_deinit_release_sem   = NULL;
static bool              g_mock_ready           = false;

/* Protected by g_mock_mutex.  An invocation is active from its locked entry
 * transition until its return.  parked/granted form the handshake that keeps a
 * release token tied to the invocation that requested it. */
static bool     g_init_active           = false;
static bool     g_init_parked           = false;
static bool     g_init_release_granted  = false;
static bool     g_stop_active           = false;
static bool     g_deinit_active         = false;
static bool     g_deinit_parked         = false;
static bool     g_deinit_release_granted = false;

/* Generation counters.  The entered generation is the corresponding attempt
 * counter held in g_mock (init_count/stop_count/deinit_count), incremented at
 * the same locked entry transition.  The completed counters are advanced once
 * an invocation's mock effects and configured result are final. */
static uint32_t g_init_completed_gen   = 0;
static uint32_t g_stop_completed_gen   = 0;
static uint32_t g_deinit_completed_gen = 0;

/* Number of HAL events that were actually delivered through a registered
 * callback.  Protected by g_mock_mutex; incremented synchronously by
 * inject_event on the caller's thread, so the value is observable immediately
 * after inject_event returns.  Exposed through wifi_hal_mock.h so the TASK-135C
 * lifecycle tests can prove that an event injected after deinit cannot reach
 * the management layer (the callback is dropped during HAL deinit). */
static uint32_t g_delivered_events = 0;

/* Connection-call channel (wifi_hal_connect).  The entered generation is the
 * attempt counter held in g_mock.connect_count; the completed generation is
 * advanced once the configured result and connected state are final.  The
 * hold/park handshake mirrors the init round so a reset can never clear or
 * drain the round belonging to a live connect call. */
static bool              g_hold_connect           = false;
static bool              g_connect_active         = false;
static bool              g_connect_parked         = false;
static bool              g_connect_release_granted = false;
static uint32_t          g_connect_completed_gen  = 0;
static osal_bin_sem_id_t g_connect_call_sem       = NULL;
static osal_bin_sem_id_t g_connect_completed_sem  = NULL;
static osal_bin_sem_id_t g_connect_release_sem    = NULL;

/* Mode-start channel (wifi_hal_start).  The entered generation is the attempt
 * counter held in g_mock.start_count; the completed generation is advanced once
 * the mode/result are final.  Hold/park handshake mirrors the init round. */
static bool              g_hold_start             = false;
static bool              g_start_active           = false;
static bool              g_start_parked           = false;
static bool              g_start_release_granted  = false;
static uint32_t          g_start_completed_gen    = 0;
static osal_bin_sem_id_t g_start_entered_sem      = NULL;
static osal_bin_sem_id_t g_start_completed_sem    = NULL;
static osal_bin_sem_id_t g_start_release_sem      = NULL;

/* GOT_IP and SCAN_DONE delivery channels.  The generation is advanced while
 * the delivery decision is final (under the mock mutex) whenever the event is
 * actually delivered through a registered callback.  g_hold_got_ip withholds
 * injected GOT_IP deliveries (mirrors the scan_done hold). */
static uint32_t          g_got_ip_delivered_gen   = 0;
static uint32_t          g_scan_done_delivered_gen = 0;
static bool              g_hold_got_ip            = false;
static osal_bin_sem_id_t g_got_ip_sem             = NULL;
static osal_bin_sem_id_t g_scan_done_sem          = NULL;

/* ---- mock control API ---------------------------------------------------- */

static bool _bootstrap_sem( osal_bin_sem_id_t* sem, const char* name )
{
  osal_status_t status;

  status = osal_bin_sem_create( sem, name, OSAL_SEM_EMPTY );
  if ( status != OSAL_SUCCESS || *sem == NULL )
  {
    *sem = NULL;
    return false;
  }
  return true;
}

static bool _mock_bootstrap( void )
{
  osal_status_t status;

  if ( g_mock_ready )
  {
    return true;
  }

  status = osal_mutex_create( &g_mock_mutex, "wifi_mock_mutex" );
  if ( status != OSAL_SUCCESS || g_mock_mutex == NULL )
  {
    g_mock_mutex = NULL;
    return false;
  }

  if ( !_bootstrap_sem( &g_init_entered_sem,     "wifi_mock_init_entered" ) ||
       !_bootstrap_sem( &g_init_completed_sem,   "wifi_mock_init_completed" ) ||
       !_bootstrap_sem( &g_init_release_sem,     "wifi_mock_init_release" ) ||
       !_bootstrap_sem( &g_stop_entered_sem,     "wifi_mock_stop_entered" ) ||
       !_bootstrap_sem( &g_stop_completed_sem,   "wifi_mock_stop_completed" ) ||
       !_bootstrap_sem( &g_deinit_entered_sem,   "wifi_mock_deinit_entered" ) ||
       !_bootstrap_sem( &g_deinit_completed_sem, "wifi_mock_deinit_completed" ) ||
       !_bootstrap_sem( &g_deinit_release_sem,   "wifi_mock_deinit_release" ) ||
       !_bootstrap_sem( &g_connect_call_sem,     "wifi_mock_connect_call" ) ||
       !_bootstrap_sem( &g_connect_completed_sem, "wifi_mock_connect_completed" ) ||
       !_bootstrap_sem( &g_connect_release_sem,  "wifi_mock_connect_release" ) ||
       !_bootstrap_sem( &g_start_entered_sem,    "wifi_mock_start_entered" ) ||
       !_bootstrap_sem( &g_start_completed_sem,  "wifi_mock_start_completed" ) ||
       !_bootstrap_sem( &g_start_release_sem,    "wifi_mock_start_release" ) ||
       !_bootstrap_sem( &g_got_ip_sem,           "wifi_mock_got_ip" ) ||
       !_bootstrap_sem( &g_scan_done_sem,        "wifi_mock_scan_done" ) )
  {
    (void) osal_bin_sem_delete( g_init_entered_sem );
    (void) osal_bin_sem_delete( g_init_completed_sem );
    (void) osal_bin_sem_delete( g_init_release_sem );
    (void) osal_bin_sem_delete( g_stop_entered_sem );
    (void) osal_bin_sem_delete( g_stop_completed_sem );
    (void) osal_bin_sem_delete( g_deinit_entered_sem );
    (void) osal_bin_sem_delete( g_deinit_completed_sem );
    (void) osal_bin_sem_delete( g_deinit_release_sem );
    (void) osal_bin_sem_delete( g_connect_call_sem );
    (void) osal_bin_sem_delete( g_connect_completed_sem );
    (void) osal_bin_sem_delete( g_connect_release_sem );
    (void) osal_bin_sem_delete( g_start_entered_sem );
    (void) osal_bin_sem_delete( g_start_completed_sem );
    (void) osal_bin_sem_delete( g_start_release_sem );
    (void) osal_bin_sem_delete( g_got_ip_sem );
    (void) osal_bin_sem_delete( g_scan_done_sem );
    g_init_entered_sem = NULL;
    g_init_completed_sem = NULL;
    g_init_release_sem = NULL;
    g_stop_entered_sem = NULL;
    g_stop_completed_sem = NULL;
    g_deinit_entered_sem = NULL;
    g_deinit_completed_sem = NULL;
    g_deinit_release_sem = NULL;
    g_connect_call_sem = NULL;
    g_connect_completed_sem = NULL;
    g_connect_release_sem = NULL;
    g_start_entered_sem = NULL;
    g_start_completed_sem = NULL;
    g_start_release_sem = NULL;
    g_got_ip_sem = NULL;
    g_scan_done_sem = NULL;
    (void) osal_mutex_delete( g_mock_mutex );
    g_mock_mutex = NULL;
    return false;
  }

  g_mock_ready = true;
  return true;
}

static bool _mock_lock( void )
{
  return g_mock_ready && osal_mutex_take( g_mock_mutex ) == OSAL_SUCCESS;
}

static void _mock_unlock( void )
{
  (void) osal_mutex_give( g_mock_mutex );
}

static void _drain( osal_bin_sem_id_t sem )
{
  while ( osal_bin_sem_timed_wait( sem, 0 ) == OSAL_SUCCESS )
  {
  }
}

bool wifi_hal_mock_reset( void )
{
  /* Bootstrap is performed before any worker is created by the fixture. */
  if ( !_mock_bootstrap() || !_mock_lock() )
  {
    return false;
  }

  /* Reset is rejected while any lifecycle invocation or held connect/start
   * round is active or parked, so it can never clear or drain the round
   * belonging to a live call. */
  if ( g_init_active || g_stop_active || g_deinit_active ||
       g_connect_active || g_start_active )
  {
    _mock_unlock();
    return false;
  }

  memset( &g_mock, 0, sizeof( g_mock ) );
  g_hold_scan_done         = false;
  g_hold_init              = false;
  g_hold_deinit            = false;
  g_hold_connect           = false;
  g_hold_start             = false;
  g_hold_got_ip            = false;
  g_init_result            = OSAL_SUCCESS;
  g_stop_result            = OSAL_SUCCESS;
  g_deinit_result          = OSAL_SUCCESS;
  g_event_cb_registered    = false;
  g_user_data_registered   = false;
  g_delivered_events       = 0;
  g_init_completed_gen     = 0;
  g_stop_completed_gen     = 0;
  g_deinit_completed_gen   = 0;
  g_connect_completed_gen  = 0;
  g_start_completed_gen    = 0;
  g_got_ip_delivered_gen   = 0;
  g_scan_done_delivered_gen = 0;
  g_init_parked            = false;
  g_init_release_granted   = false;
  g_deinit_parked          = false;
  g_deinit_release_granted = false;
  g_connect_parked         = false;
  g_connect_release_granted = false;
  g_start_parked           = false;
  g_start_release_granted  = false;

  /* Keep the mutex through all drains.  A later lifecycle invocation therefore
   * cannot publish a token until every token from this round has been removed. */
  _drain( g_init_entered_sem );
  _drain( g_init_completed_sem );
  _drain( g_init_release_sem );
  _drain( g_stop_entered_sem );
  _drain( g_stop_completed_sem );
  _drain( g_deinit_entered_sem );
  _drain( g_deinit_completed_sem );
  _drain( g_deinit_release_sem );
  _drain( g_connect_call_sem );
  _drain( g_connect_completed_sem );
  _drain( g_connect_release_sem );
  _drain( g_start_entered_sem );
  _drain( g_start_completed_sem );
  _drain( g_start_release_sem );
  _drain( g_got_ip_sem );
  _drain( g_scan_done_sem );
  _mock_unlock();
  return true;
}

void wifi_hal_mock_set_connect_result( osal_status_t result )
{
  if ( _mock_lock() )
  {
    g_mock.connect_result = result;
    _mock_unlock();
  }
}

void wifi_hal_mock_set_start_result( osal_status_t result )
{
  if ( _mock_lock() )
  {
    g_mock.start_result = result;
    _mock_unlock();
  }
}

void wifi_hal_mock_set_init_result( osal_status_t result )
{
  if ( _mock_lock() )
  {
    g_init_result = result;
    _mock_unlock();
  }
}

void wifi_hal_mock_set_stop_result( osal_status_t result )
{
  if ( _mock_lock() )
  {
    g_stop_result = result;
    _mock_unlock();
  }
}

void wifi_hal_mock_set_deinit_result( osal_status_t result )
{
  if ( _mock_lock() )
  {
    g_deinit_result = result;
    _mock_unlock();
  }
}

void wifi_hal_mock_set_scan_list( const wifi_hal_ap_record_t* list, uint16_t count )
{
  if ( count > WIFI_HAL_MOCK_MAX_AP )
  {
    count = WIFI_HAL_MOCK_MAX_AP;
  }

  if ( !_mock_lock() )
  {
    return;
  }
  g_mock.scan_count = count;
  if ( list && count > 0 )
  {
    memcpy( g_mock.scan_list, list, count * sizeof( wifi_hal_ap_record_t ) );
  }
  _mock_unlock();
}

void wifi_hal_mock_set_ip_info( const wifi_hal_ip_info_t* info )
{
  if ( !info || !_mock_lock() )
  {
    return;
  }
  g_mock.ip_info = *info;
  _mock_unlock();
}

void wifi_hal_mock_inject_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  wifi_hal_event_cb_t cb;
  void*               user_data;
  bool                deliver = false;

  /* Copy the callback/user data under the mock mutex, release the mutex, then
   * invoke so no mock lock is held during delivery (a re-entrant callback that
   * re-enters a mock control API therefore cannot deadlock).  The delivery
   * counter is updated under the same lock, so it is observable immediately
   * after inject_event returns and proves whether a callback was reached.  The
   * GOT_IP/SCAN_DONE delivery generations are advanced in the same locked
   * transaction, so a delivery waiter can never observe a generation without
   * the matching delivery. */
  if ( !_mock_lock() )
  {
    return;
  }
  cb        = g_mock.event_cb;
  user_data = g_mock.user_data;
  if ( cb != NULL )
  {
    /* The GOT_IP hold withholds injected GOT_IP deliveries (mirrors the scan
     * done hold): the callback is not invoked and no delivery generation is
     * advanced while held.  Clearing the hold does not retroactively deliver. */
    if ( event == WIFI_HAL_EVT_STA_GOT_IP && g_hold_got_ip )
    {
      cb = NULL;
    }
    else
    {
      ++g_delivered_events;
      deliver = true;
      if ( event == WIFI_HAL_EVT_STA_GOT_IP )
      {
        ++g_got_ip_delivered_gen;
        (void) osal_bin_sem_give( g_got_ip_sem );
      }
      else if ( event == WIFI_HAL_EVT_SCAN_DONE )
      {
        ++g_scan_done_delivered_gen;
        (void) osal_bin_sem_give( g_scan_done_sem );
      }
    }
  }
  _mock_unlock();

  if ( deliver && cb != NULL )
  {
    cb( event, data, user_data );
  }
}

void wifi_hal_mock_set_scan_done_hold( bool hold )
{
  if ( _mock_lock() )
  {
    g_hold_scan_done = hold;
    _mock_unlock();
  }
}

void wifi_hal_mock_set_init_hold( bool hold )
{
  if ( !_mock_lock() )
  {
    return;
  }

  g_hold_init = hold;
  if ( !hold && g_init_parked && !g_init_release_granted )
  {
    /*
     * The grant and its wakeup are one locked transaction.  The parked init
     * may consume the semaphore without taking this mutex, so giving it after
     * unlocking would allow this control call to observe the old parked state
     * and queue a second release before the init clears that state.
     */
    g_init_release_granted = true;
    if ( osal_bin_sem_give( g_init_release_sem ) != OSAL_SUCCESS )
    {
      /* No token was queued: leave the grant retryable and the init parked. */
      g_init_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_release_init_hold( void )
{
  if ( !_mock_lock() )
  {
    return;
  }

  /* Release exactly one already-parked invocation without disabling the
   * hold.  A release made before an invocation parks leaves no credit, so it
   * can never release a later round. */
  if ( g_init_parked && !g_init_release_granted )
  {
    g_init_release_granted = true;
    if ( osal_bin_sem_give( g_init_release_sem ) != OSAL_SUCCESS )
    {
      g_init_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_set_deinit_hold( bool hold )
{
  if ( !_mock_lock() )
  {
    return;
  }

  g_hold_deinit = hold;
  if ( !hold && g_deinit_parked && !g_deinit_release_granted )
  {
    g_deinit_release_granted = true;
    if ( osal_bin_sem_give( g_deinit_release_sem ) != OSAL_SUCCESS )
    {
      g_deinit_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_release_deinit_hold( void )
{
  if ( !_mock_lock() )
  {
    return;
  }

  if ( g_deinit_parked && !g_deinit_release_granted )
  {
    g_deinit_release_granted = true;
    if ( osal_bin_sem_give( g_deinit_release_sem ) != OSAL_SUCCESS )
    {
      g_deinit_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_set_connect_hold( bool hold )
{
  if ( !_mock_lock() )
  {
    return;
  }

  g_hold_connect = hold;
  if ( !hold && g_connect_parked && !g_connect_release_granted )
  {
    g_connect_release_granted = true;
    if ( osal_bin_sem_give( g_connect_release_sem ) != OSAL_SUCCESS )
    {
      g_connect_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_release_connect_hold( void )
{
  if ( !_mock_lock() )
  {
    return;
  }

  if ( g_connect_parked && !g_connect_release_granted )
  {
    g_connect_release_granted = true;
    if ( osal_bin_sem_give( g_connect_release_sem ) != OSAL_SUCCESS )
    {
      g_connect_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_set_start_hold( bool hold )
{
  if ( !_mock_lock() )
  {
    return;
  }

  g_hold_start = hold;
  if ( !hold && g_start_parked && !g_start_release_granted )
  {
    g_start_release_granted = true;
    if ( osal_bin_sem_give( g_start_release_sem ) != OSAL_SUCCESS )
    {
      g_start_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_release_start_hold( void )
{
  if ( !_mock_lock() )
  {
    return;
  }

  if ( g_start_parked && !g_start_release_granted )
  {
    g_start_release_granted = true;
    if ( osal_bin_sem_give( g_start_release_sem ) != OSAL_SUCCESS )
    {
      g_start_release_granted = false;
    }
  }
  _mock_unlock();
}

void wifi_hal_mock_set_got_ip_hold( bool hold )
{
  if ( _mock_lock() )
  {
    g_hold_got_ip = hold;
    _mock_unlock();
  }
}

bool wifi_hal_mock_wait_init_entered( uint32_t timeout_ms )
{
  if ( !g_mock_ready )
  {
    return false;
  }
  return osal_bin_sem_timed_wait( g_init_entered_sem, timeout_ms ) == OSAL_SUCCESS;
}

/* ---- lifecycle generation helpers ---------------------------------------- */

/* Wrap-safe wall-clock delta in milliseconds.  This works for any interval
 * shorter than half of the 32-bit timer range (about 24.8 days). */
static uint32_t _ms_delta( uint32_t start_ms, uint32_t now_ms )
{
  if ( now_ms >= start_ms )
  {
    return now_ms - start_ms;
  }
  return ( UINT32_MAX - start_ms ) + now_ms + 1U;
}

/* Generation-checked wait: first inspect the counter under the mutex, then use
 * the semaphore only as a wake hint, re-checking the counter after every
 * wake and charging the elapsed time against one wrap-safe budget. */
static bool _wait_generation( uint32_t* generation, osal_bin_sem_id_t hint,
                              uint32_t level, uint32_t timeout_ms )
{
  bool    satisfied;
  uint32_t elapsed;
  uint32_t start_ms;

  if ( !g_mock_ready )
  {
    return false;
  }

  start_ms = osal_task_get_time_ms();

  for ( ;; )
  {
    if ( _mock_lock() )
    {
      satisfied = ( *generation >= level );
      _mock_unlock();
    }
    else
    {
      return false;
    }

    if ( satisfied )
    {
      return true;
    }

    if ( timeout_ms == OSAL_MAX_DELAY )
    {
      (void) osal_bin_sem_timed_wait( hint, OSAL_MAX_DELAY );
      continue;
    }

    elapsed = _ms_delta( start_ms, osal_task_get_time_ms() );
    if ( elapsed >= timeout_ms )
    {
      return false;
    }

    /* Charge the remaining budget against the hint wait. */
    (void) osal_bin_sem_timed_wait( hint, timeout_ms - elapsed );

    /* Loop: re-check the generation and recompute the elapsed time. */
  }
}

/* ---- lifecycle counters and waits ----------------------------------------- */

/* Copy one generation counter under the mock mutex so a concurrent getter
 * never reads a partially-updated snapshot produced by a lifecycle worker
 * invocation.  The mock is bootstrapped before any worker is created, so a
 * lock failure is not expected in normal test flow; on failure the empty (0)
 * sentinel is returned, consistent with the API treating an unavailable
 * synchronized counter as if no attempt were yet observed. */
static uint32_t _read_counter( uint32_t* counter )
{
  uint32_t value;

  if ( !_mock_lock() )
  {
    return 0;
  }
  value = *counter;
  _mock_unlock();
  return value;
}

uint32_t wifi_hal_mock_get_init_entered_count( void )
{
  return _read_counter( &g_mock.init_count );
}

uint32_t wifi_hal_mock_get_init_completed_count( void )
{
  return _read_counter( &g_init_completed_gen );
}

uint32_t wifi_hal_mock_get_stop_entered_count( void )
{
  return _read_counter( &g_mock.stop_count );
}

uint32_t wifi_hal_mock_get_stop_completed_count( void )
{
  return _read_counter( &g_stop_completed_gen );
}

uint32_t wifi_hal_mock_get_deinit_entered_count( void )
{
  return _read_counter( &g_mock.deinit_count );
}

uint32_t wifi_hal_mock_get_deinit_completed_count( void )
{
  return _read_counter( &g_deinit_completed_gen );
}

bool wifi_hal_mock_wait_init_entered_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_mock.init_count, g_init_entered_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_init_completed_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_init_completed_gen, g_init_completed_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_stop_entered_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_mock.stop_count, g_stop_entered_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_stop_completed_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_stop_completed_gen, g_stop_completed_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_deinit_entered_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_mock.deinit_count, g_deinit_entered_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_deinit_completed_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_deinit_completed_gen, g_deinit_completed_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_connect_call_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_mock.connect_count, g_connect_call_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_connect_completed_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_connect_completed_gen, g_connect_completed_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_start_entered_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_mock.start_count, g_start_entered_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_start_completed_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_start_completed_gen, g_start_completed_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_got_ip_delivered_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_got_ip_delivered_gen, g_got_ip_sem, level, timeout_ms );
}

bool wifi_hal_mock_wait_scan_done_delivered_level( uint32_t level, uint32_t timeout_ms )
{
  return _wait_generation( &g_scan_done_delivered_gen, g_scan_done_sem, level, timeout_ms );
}

bool wifi_hal_mock_get_lifecycle( wifi_hal_mock_lifecycle_t* out )
{
  if ( out != NULL )
  {
    /* Zero the whole destination before taking the lock so a failed copy is
     * never mistaken for a valid snapshot and no stale field survives. */
    memset( out, 0, sizeof( *out ) );
  }

  if ( !_mock_lock() )
  {
    return false;
  }

  if ( out != NULL )
  {
    out->initialized          = g_mock.initialized;
    out->started              = g_mock.started;
    out->connected            = g_mock.connected;
    out->init_count           = g_mock.init_count;
    out->start_count          = g_mock.start_count;
    out->stop_count           = g_mock.stop_count;
    out->deinit_count         = g_mock.deinit_count;
    out->init_entered_gen     = g_mock.init_count;
    out->init_completed_gen   = g_init_completed_gen;
    out->stop_entered_gen     = g_mock.stop_count;
    out->stop_completed_gen   = g_stop_completed_gen;
    out->deinit_entered_gen   = g_mock.deinit_count;
    out->deinit_completed_gen = g_deinit_completed_gen;
    out->event_cb_registered  = g_event_cb_registered;
    out->user_data_registered = g_user_data_registered;
  }

  _mock_unlock();
  return true;
}

uint32_t wifi_hal_mock_get_delivered_event_count( void )
{
  uint32_t count = 0;
  if ( _mock_lock() )
  {
    count = g_delivered_events;
    _mock_unlock();
  }
  return count;
}

uint32_t wifi_hal_mock_get_connect_call_count( void )
{
  return _read_counter( &g_mock.connect_count );
}

uint32_t wifi_hal_mock_get_connect_completed_count( void )
{
  return _read_counter( &g_connect_completed_gen );
}

uint32_t wifi_hal_mock_get_start_entered_count( void )
{
  return _read_counter( &g_mock.start_count );
}

uint32_t wifi_hal_mock_get_start_completed_count( void )
{
  return _read_counter( &g_start_completed_gen );
}

uint32_t wifi_hal_mock_get_got_ip_delivered_count( void )
{
  return _read_counter( &g_got_ip_delivered_gen );
}

uint32_t wifi_hal_mock_get_scan_done_delivered_count( void )
{
  return _read_counter( &g_scan_done_delivered_gen );
}

bool wifi_hal_mock_get_state( wifi_hal_mock_state_t* out )
{
  if ( out != NULL )
  {
    /* Zero the whole destination before taking the lock so a failed copy is
     * never mistaken for a valid snapshot and no stale field survives. */
    memset( out, 0, sizeof( *out ) );
  }

  if ( !_mock_lock() )
  {
    return false;
  }

  if ( out != NULL )
  {
    *out = g_mock;
  }

  _mock_unlock();
  return true;
}

/* ---- HAL implementation -------------------------------------------------- */

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

osal_status_t wifi_hal_init( const wifi_hal_init_t* init )
{
  bool          hold;
  osal_status_t result;

  if ( !init )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !g_mock_ready || !_mock_lock() )
  {
    return OSAL_ERROR;
  }

  /* Validate before entering the synchronized lifecycle round. */
  if ( init->ap_dns && init->ap_dns[0] != '\0' &&
       !wifi_hal_is_valid_ipv4( init->ap_dns ) )
  {
    _mock_unlock();
    return OSAL_ERR_INVALID_ARGUMENT;
  }

  /* Entry, active state, visible attempt count, and acknowledgement are one
   * transaction.  In particular, reset cannot run between entry and give. */
  g_init_active = true;
  g_mock.init_count++;
  if ( init->ap_dns && init->ap_dns[0] != '\0' )
  {
    strncpy( g_mock.ap_dns, init->ap_dns, sizeof( g_mock.ap_dns ) - 1 );
    g_mock.ap_dns[sizeof( g_mock.ap_dns ) - 1] = '\0';
    g_mock.ap_dns_set = true;
  }
  else
  {
    g_mock.ap_dns[0]  = '\0';
    g_mock.ap_dns_set = false;
  }

  hold = g_hold_init;
  if ( hold )
  {
    /* Commit to parking before publishing the acknowledgement. */
    g_init_parked          = true;
    g_init_release_granted = false;
  }
  (void) osal_bin_sem_give( g_init_entered_sem );
  _mock_unlock();

  if ( hold )
  {
    /* The mutex is deliberately not held while the invocation blocks. */
    (void) osal_bin_sem_take( g_init_release_sem );

    if ( !_mock_lock() )
    {
      return OSAL_ERROR;
    }
    g_init_parked          = false;
    g_init_release_granted = false;
    _mock_unlock();
  }

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  result = g_init_result;
  if ( result == OSAL_SUCCESS )
  {
    g_mock.initialized        = true;
    g_mock.event_cb           = init->event_cb;
    g_mock.user_data          = init->user_data;
    g_event_cb_registered     = ( init->event_cb != NULL );
    g_user_data_registered    = ( init->user_data != NULL );
  }
  /* Publish the completed generation once the effects and result are final,
   * while the invocation is still marked active.  A completion waiter can
   * therefore never observe a generation without the matching final state. */
  g_init_completed_gen++;
  (void) osal_bin_sem_give( g_init_completed_sem );
  g_init_active = false;
  _mock_unlock();
  return result;
}

osal_status_t wifi_hal_deinit( void )
{
  bool          hold;
  osal_status_t result;

  if ( !g_mock_ready || !_mock_lock() )
  {
    return OSAL_ERROR;
  }

  /* Entry, active state, visible attempt counter, and entered acknowledgement
   * are one transaction, so an entered ack and its counter snapshot cannot
   * disagree and reset cannot slip between entry and publication. */
  g_deinit_active = true;
  g_mock.deinit_count++;
  hold = g_hold_deinit;
  if ( hold )
  {
    g_deinit_parked          = true;
    g_deinit_release_granted = false;
  }
  (void) osal_bin_sem_give( g_deinit_entered_sem );
  _mock_unlock();

  if ( hold )
  {
    /* The mutex is deliberately not held while the invocation blocks. */
    (void) osal_bin_sem_take( g_deinit_release_sem );

    if ( !_mock_lock() )
    {
      return OSAL_ERROR;
    }
    g_deinit_parked          = false;
    g_deinit_release_granted = false;
    _mock_unlock();
  }

  /* Read the configured result and apply the mock effects before publishing the
   * completed generation, while still marked active so a waiter can never
   * observe a stale partial generation. */
  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  result = g_deinit_result;
  if ( result == OSAL_SUCCESS )
  {
    g_mock.initialized        = false;
    g_mock.started            = false;
    g_mock.connected          = false;
    g_mock.event_cb           = NULL;
    g_mock.user_data          = NULL;
    g_event_cb_registered     = false;
    g_user_data_registered    = false;
  }
  g_deinit_completed_gen++;
  (void) osal_bin_sem_give( g_deinit_completed_sem );
  g_deinit_active = false;
  _mock_unlock();
  return result;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  bool          hold;
  osal_status_t result;

  if ( !g_mock_ready || !_mock_lock() )
  {
    return OSAL_ERROR;
  }

  /* Entry, active state, visible mode/attempt counter, and entered
   * acknowledgement are one transaction, exactly like the lifecycle rounds. */
  g_start_active = true;
  g_mock.mode       = mode;
  g_mock.start_count++;
  hold = g_hold_start;
  if ( hold )
  {
    g_start_parked          = true;
    g_start_release_granted = false;
  }
  (void) osal_bin_sem_give( g_start_entered_sem );
  _mock_unlock();

  if ( hold )
  {
    /* The mutex is deliberately not held while the invocation blocks. */
    (void) osal_bin_sem_take( g_start_release_sem );

    if ( !_mock_lock() )
    {
      return OSAL_ERROR;
    }
    g_start_parked          = false;
    g_start_release_granted = false;
    _mock_unlock();
  }

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  result         = g_mock.start_result;
  g_mock.started = ( result == OSAL_SUCCESS );
  g_start_completed_gen++;
  (void) osal_bin_sem_give( g_start_completed_sem );
  g_start_active = false;
  _mock_unlock();
  return result;
}

osal_status_t wifi_hal_stop( void )
{
  osal_status_t result;

  if ( !g_mock_ready || !_mock_lock() )
  {
    return OSAL_ERROR;
  }

  /* Entry, active state, attempt counter, and entered acknowledgement are one
   * transaction. */
  g_stop_active = true;
  g_mock.stop_count++;
  (void) osal_bin_sem_give( g_stop_entered_sem );
  _mock_unlock();

  /* Read the configured result and apply the mock effects before publishing the
   * completed generation, while still marked active so a waiter can never
   * observe a partial generation. */
  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  result = g_stop_result;
  if ( result == OSAL_SUCCESS )
  {
    g_mock.started   = false;
    g_mock.connected = false;
  }
  g_stop_completed_gen++;
  (void) osal_bin_sem_give( g_stop_completed_sem );
  g_stop_active = false;
  _mock_unlock();
  return result;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  g_mock.sta_cfg = *config;
  _mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  g_mock.ap_cfg = *config;
  _mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  bool          hold;
  osal_status_t result;

  if ( !g_mock_ready || !_mock_lock() )
  {
    return OSAL_ERROR;
  }

  /* Entry, active state, visible attempt counter, and the entered
   * acknowledgement are one transaction (mirrors the lifecycle rounds). */
  g_connect_active = true;
  g_mock.connect_count++;
  hold = g_hold_connect;
  if ( hold )
  {
    g_connect_parked          = true;
    g_connect_release_granted = false;
  }
  (void) osal_bin_sem_give( g_connect_call_sem );
  _mock_unlock();

  if ( hold )
  {
    /* The mutex is deliberately not held while the invocation blocks. */
    (void) osal_bin_sem_take( g_connect_release_sem );

    if ( !_mock_lock() )
    {
      return OSAL_ERROR;
    }
    g_connect_parked          = false;
    g_connect_release_granted = false;
    _mock_unlock();
  }

  /* Apply the configured result and publish the completed generation while
   * still marked active so a waiter can never observe a partial generation. */
  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  result = g_mock.connect_result;
  if ( result == OSAL_SUCCESS )
  {
    g_mock.connected = true;
  }
  g_connect_completed_gen++;
  (void) osal_bin_sem_give( g_connect_completed_sem );
  g_connect_active = false;
  _mock_unlock();
  return result;
}

osal_status_t wifi_hal_disconnect( void )
{
  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  g_mock.connected = false;
  _mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start_scan( bool block )
{
  wifi_hal_event_cb_t cb;
  void*               user_data;
  bool                fire_scan_done = false;

  (void) block;

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  g_mock.scan_start_count++;
  /* Copy the callback/user data under the mock mutex, then release before
     delivering so no mock lock is held during invocation. */
  cb                 = g_mock.event_cb;
  user_data          = g_mock.user_data;
  if ( cb != NULL && !g_hold_scan_done )
  {
    fire_scan_done = true;
    ++g_scan_done_delivered_gen;
    (void) osal_bin_sem_give( g_scan_done_sem );
  }
  _mock_unlock();

  if ( fire_scan_done )
  {
    cb( WIFI_HAL_EVT_SCAN_DONE, NULL, user_data );
  }

  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !records || !in_out_count )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }

  uint16_t to_copy = g_mock.scan_count;
  if ( to_copy > *in_out_count )
  {
    to_copy = *in_out_count;
  }

  memcpy( records, g_mock.scan_list, to_copy * sizeof( wifi_hal_ap_record_t ) );
  *in_out_count = to_copy;
  _mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  *out_info = g_mock.ip_info;
  _mock_unlock();
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
  if ( !_mock_lock() )
  {
    return OSAL_ERROR;
  }
  g_mock.power_save = enabled;
  _mock_unlock();
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
