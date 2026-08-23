/*
 * Wi-Fi HAL Mock Driver for Unit Tests
 *
 * Provides controllable mock implementations of all wifi_hal_driver.h
 * functions.  Test code can configure predefined scan results, control
 * connect/disconnect outcomes, and inject events via the public helpers
 * declared in wifi_hal_mock.h.
 *
 * Lifecycle synchronization (TASK-134B):
 *   - wifi_hal_init()/wifi_hal_deinit() can park the Wi-Fi worker on an
 *     acknowledged, per-invocation release barrier (no busy boolean loops).
 *     The test acknowledges arrival with the "entered" generation notification
 *     and releases either exactly one parked call (release_*_hold()) or every
 *     parked call (set_*_hold(false)).
 *   - every wifi_hal_init/deinit/stop invocation raises 1-based "entered" and
 *     "completed" generation notifications; generation-checked waits
 *     distinguish a later lifecycle round from a stale token.
 *   - lifecycle fields (counters, initialized/started flags, event callback)
 *     are written and read under the mock lock, so lifecycle snapshot/counter
 *     assertions never race the Wi-Fi worker.
 *   - callbacks (event delivery, scan-done) are copied under the mock lock and
 *     invoked only after the lock is released, so a concurrent deinit that
 *     clears the callback cannot race callback invocation.
 */

#include "wifi_hal_mock.h"

#include "osal_count_sem.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include <stdbool.h>
#include <string.h>

/* ---- internal state ------------------------------------------------------ */

#define WIFI_HAL_MOCK_SEM_MAX 128U

static wifi_hal_mock_state_t g_mock = { 0 };

/* Serializes access to lifecycle fields, control flags, generation counters and
 * barrier bookkeeping shared between the Wi-Fi worker and the test thread.
 *
 * The lock and every lifecycle semaphore are created ONCE, on the single-threaded
 * wifi_hal_mock_reset() bootstrap path: every test harness (and TLS unit runner)
 * resets the mock before spawning the Wi-Fi worker task, so no two threads can
 * ever race the build.  No other mock entry point creates primitives, so there
 * is exactly one creation site.  g_mock_ready is published true only after the
 * complete set is created and safely visible. */
static osal_mutex_id_t  g_lock       = NULL;
static bool             g_mock_ready = false;

static bool g_hold_scan_done = false;

/* Per-invocation release barrier shared between the Wi-Fi worker and the test
 * thread.
 *
 *   - "active"   counts calls currently inside the HAL lifecycle function, so
 *                reset never clears shared state mid-call;
 *   - "waiting"  counts calls that committed to the barrier and have not yet
 *                been released;
 *   - "released" counts release grants handed out but not yet consumed.
 *
 * The invariant released <= waiting always holds.  A parked call consumes
 * exactly one grant (released--, waiting--).  release_*_hold() grants at most
 * one, set_*_hold(false) grants every waiting call, and a grant can therefore
 * never outlive the call it was issued for.
 *
 * The counting semaphore is only an advisory wakeup: the authoritative signal
 * is "released", re-checked under the mock lock after every wakeup, so stale
 * wakeup tokens can never release a later lifecycle round. */
typedef struct
{
  bool                hold_set;
  uint32_t            active;
  uint32_t            waiting;
  uint32_t            released;
  osal_count_sem_id_t wake_sem;
} mock_barrier_t;

static mock_barrier_t   g_init_barrier;
static mock_barrier_t   g_deinit_barrier;

static osal_status_t    g_init_result   = OSAL_SUCCESS;
static osal_status_t    g_deinit_result = OSAL_SUCCESS;

/* 1-based lifecycle round generations. */
static uint32_t          g_init_entered_gen;
static uint32_t          g_init_completed_gen;
static uint32_t          g_stop_entered_gen;
static uint32_t          g_stop_completed_gen;
static uint32_t          g_deinit_entered_gen;
static uint32_t          g_deinit_completed_gen;
static uint32_t          g_stop_active;

static osal_count_sem_id_t g_init_entered_sem;
static osal_count_sem_id_t g_init_completed_sem;
static osal_count_sem_id_t g_stop_entered_sem;
static osal_count_sem_id_t g_stop_completed_sem;
static osal_count_sem_id_t g_deinit_entered_sem;
static osal_count_sem_id_t g_deinit_completed_sem;

/* ---- tiny synchronization helpers ---------------------------------------- */

static bool mock_ensure_created( void );

static bool mock_lock( void )
{
  if ( !g_mock_ready || g_lock == NULL )
  {
    return false;
  }

  return osal_mutex_take( g_lock ) == OSAL_SUCCESS;
}

static void mock_unlock( void )
{
  if ( g_mock_ready && g_lock != NULL )
  {
    (void) osal_mutex_give( g_lock );
  }
}

/* Drop every pending token; used only when no lifecycle call is parked on the
 * barrier owning the semaphore. */
static void sem_drain( osal_count_sem_id_t sem )
{
  if ( sem == NULL )
  {
    return;
  }

  while ( osal_count_sem_timed_wait( sem, 0 ) == OSAL_SUCCESS )
  {
  }
}

/* Wake a waiter; tokens are only advisory (generation counters / released
 * counts are the authoritative signals), so a failed give is harmless. */
static void sem_wake( osal_count_sem_id_t sem )
{
  if ( sem != NULL )
  {
    (void) osal_count_sem_give( sem );
  }
}

/* Wait until (*counter) >= gen.  The semaphore is only a wakeup token: after
 * any wakeup (or timeout) the generation counter is re-checked under the lock,
 * so a stale token from a previous round can never satisfy a later round. */
static bool gen_wait( uint32_t* counter, osal_count_sem_id_t sem, uint32_t gen, uint32_t timeout_ms )
{
  /* Use an absolute deadline.  Notification semaphores are deliberately only
   * wakeup channels: a later-round wait may consume stale tokens successfully,
   * and those successful waits must still consume real wall-clock budget. */
  const uint32_t deadline = osal_task_get_time_ms() + timeout_ms;

  if ( !mock_ensure_created() )
  {
    return false;
  }

  if ( gen == 0 )
  {
    return true;
  }

  for ( ;; )
  {
    if ( !mock_lock() )
    {
      return false;
    }
    bool reached = ( *counter >= gen );
    mock_unlock();
    if ( reached )
    {
      return true;
    }

    const uint32_t now = osal_task_get_time_ms();
    if ( (int32_t) ( now - deadline ) >= 0 )
    {
      return false;
    }

    const uint32_t remaining = deadline - now;
    const uint32_t step      = ( remaining < 50 ) ? remaining : 50;
    (void) osal_count_sem_timed_wait( sem, step );
  }
}

/* Raise an "entered" (or "completed") generation notification. */
static void notify_generation( uint32_t* gen, osal_count_sem_id_t sem )
{
  mock_lock();
  ( *gen )++;
  mock_unlock();
  sem_wake( sem );
}

/* ---- release barrier ------------------------------------------------------ */

/* Block the current call until it receives a release grant.  The loop re-checks
 * "released" under the lock after every wakeup (or timeout); the semaphore only
 * provides prompt wakeups, so a stale token can never release a call early. */
static void barrier_wait( mock_barrier_t* b )
{
  for ( ;; )
  {
    mock_lock();
    if ( b->released > 0 )
    {
      b->released--;
      b->waiting--;
      mock_unlock();
      return;
    }
    mock_unlock();

    /* Short timeout is only a liveness safety net; the grant check above is
     * authoritative. */
    (void) osal_count_sem_timed_wait( b->wake_sem, 100 );
  }
}

/* Release exactly one parked call while leaving the hold state untouched: the
 * next call parks again and needs its own grant.  Grants are bounded by the
 * number of parked calls, so no grant is ever left over for a later round. */
static void barrier_release_one( mock_barrier_t* b )
{
  bool granted = false;

  mock_lock();
  if ( b->released < b->waiting )
  {
    b->released++;
    granted = true;
  }
  mock_unlock();

  if ( granted )
  {
    sem_wake( b->wake_sem );
  }
}

/* Enable/disable the hold state.  Disabling releases every call currently
 * parked with its own grant (released = waiting); if nothing is parked no grant
 * is left behind. */
static void barrier_set_hold( mock_barrier_t* b, bool hold_on )
{
  uint32_t wake_count = 0;

  mock_lock();
  b->hold_set = hold_on;
  if ( !hold_on )
  {
    wake_count  = b->waiting - b->released;
    b->released = b->waiting;
  }
  mock_unlock();

  for ( uint32_t i = 0; i < wake_count; i++ )
  {
    sem_wake( b->wake_sem );
  }
}

/* Mark the current call as having left the HAL function.  Must be called after
 * every shared-state write and after the "completed" notification. */
static void barrier_exit( mock_barrier_t* b )
{
  mock_lock();
  if ( b->active > 0 )
  {
    b->active--;
  }
  mock_unlock();
}

/* Create the mock lock and every lifecycle semaphore ONCE, on the single-threaded
 * bootstrap path (wifi_hal_mock_reset() runs from the unit-test main thread
 * before any Wi-Fi worker task exists), so no two threads can ever race the
 * build and overwrite the shared pointers.  Creation is idempotent for
 * re-entrant resets.  The partially built set is never published: g_mock_ready
 * flips true only after every primitive is created and safely visible, and each
 * failure path releases the primitives created so far before returning. */
static bool mock_bootstrap( void )
{
  osal_status_t status;

  if ( g_mock_ready )
  {
    return true;
  }

  g_lock = NULL;
  g_init_entered_sem = NULL;
  g_init_completed_sem = NULL;
  g_stop_entered_sem = NULL;
  g_stop_completed_sem = NULL;
  g_deinit_entered_sem = NULL;
  g_deinit_completed_sem = NULL;
  g_init_barrier.wake_sem = NULL;
  g_deinit_barrier.wake_sem = NULL;

  status = osal_mutex_create( &g_lock, "wifi_mock_lock" );
  if ( status != OSAL_SUCCESS || g_lock == NULL )
  {
    g_lock = NULL;
    return false;
  }

#define CREATE_MOCK_SEM( field, name )                                                        \
  do                                                                                         \
  {                                                                                          \
    status = osal_count_sem_create( &( field ), ( name ), 0, WIFI_HAL_MOCK_SEM_MAX );       \
    if ( status != OSAL_SUCCESS || ( field ) == NULL )                                      \
    {                                                                                        \
      ( field ) = NULL;                                                                       \
      goto create_failed;                                                                     \
    }                                                                                         \
  } while ( 0 )

  CREATE_MOCK_SEM( g_init_entered_sem, "wifi_mock_init_entered" );
  CREATE_MOCK_SEM( g_init_completed_sem, "wifi_mock_init_completed" );
  CREATE_MOCK_SEM( g_stop_entered_sem, "wifi_mock_stop_entered" );
  CREATE_MOCK_SEM( g_stop_completed_sem, "wifi_mock_stop_completed" );
  CREATE_MOCK_SEM( g_deinit_entered_sem, "wifi_mock_deinit_entered" );
  CREATE_MOCK_SEM( g_deinit_completed_sem, "wifi_mock_deinit_completed" );
  CREATE_MOCK_SEM( g_init_barrier.wake_sem, "wifi_mock_init_release" );
  CREATE_MOCK_SEM( g_deinit_barrier.wake_sem, "wifi_mock_deinit_release" );

#undef CREATE_MOCK_SEM
  /* Publish the complete set only after the final primitive is created: a
   * concurrent caller observing g_mock_ready can only ever see the full set. */
  g_mock_ready = true;
  return true;

create_failed:
#undef CREATE_MOCK_SEM
  if ( g_deinit_barrier.wake_sem != NULL )
  {
    (void) osal_count_sem_delete( g_deinit_barrier.wake_sem );
    g_deinit_barrier.wake_sem = NULL;
  }
  if ( g_init_barrier.wake_sem != NULL )
  {
    (void) osal_count_sem_delete( g_init_barrier.wake_sem );
    g_init_barrier.wake_sem = NULL;
  }
  if ( g_deinit_completed_sem != NULL )
  {
    (void) osal_count_sem_delete( g_deinit_completed_sem );
    g_deinit_completed_sem = NULL;
  }
  if ( g_deinit_entered_sem != NULL )
  {
    (void) osal_count_sem_delete( g_deinit_entered_sem );
    g_deinit_entered_sem = NULL;
  }
  if ( g_stop_completed_sem != NULL )
  {
    (void) osal_count_sem_delete( g_stop_completed_sem );
    g_stop_completed_sem = NULL;
  }
  if ( g_stop_entered_sem != NULL )
  {
    (void) osal_count_sem_delete( g_stop_entered_sem );
    g_stop_entered_sem = NULL;
  }
  if ( g_init_completed_sem != NULL )
  {
    (void) osal_count_sem_delete( g_init_completed_sem );
    g_init_completed_sem = NULL;
  }
  if ( g_init_entered_sem != NULL )
  {
    (void) osal_count_sem_delete( g_init_entered_sem );
    g_init_entered_sem = NULL;
  }
  (void) osal_mutex_delete( g_lock );
  g_lock = NULL;
  g_mock_ready = false;
  return false;
}

/* Returns true once the mock synchronization set is fully created and safely
 * visible.  Creation itself happens ONLY on wifi_hal_mock_reset()'s
 * single-threaded bootstrap path — this function never lazily creates — so a
 * concurrent first caller can never race the build or observe a partially
 * initialized set. */
static bool mock_ensure_created( void )
{
  return g_mock_ready &&
         g_lock != NULL &&
         g_init_entered_sem != NULL &&
         g_init_completed_sem != NULL &&
         g_stop_entered_sem != NULL &&
         g_stop_completed_sem != NULL &&
         g_deinit_entered_sem != NULL &&
         g_deinit_completed_sem != NULL &&
         g_init_barrier.wake_sem != NULL &&
         g_deinit_barrier.wake_sem != NULL;
}

/* ---- mock control API ---------------------------------------------------- */

void wifi_hal_mock_reset( void )
{
  bool init_active;
  bool deinit_active;
  bool any_active;

  /* The single-threaded bootstrap must run before taking the mock lock.  Tests
   * call reset() from the main thread before any Wi-Fi worker exists, so no two
   * threads can race the one-time creation of the lock and semaphores. */
  if ( !mock_bootstrap() || !mock_lock() )
  {
    return;
  }

  init_active   = ( g_init_barrier.active > 0 );
  deinit_active = ( g_deinit_barrier.active > 0 );
  any_active    = init_active || deinit_active || ( g_stop_active > 0 );

  /* Never clear shared state, lifecycle results, or generations while a HAL
   * call is still inside the mock.  In particular, a held call owns its
   * entered generation and release bookkeeping until it completes. */
  if ( !any_active )
  {
    memset( &g_mock, 0, sizeof( g_mock ) );
    g_init_result       = OSAL_SUCCESS;
    g_deinit_result     = OSAL_SUCCESS;
    g_init_entered_gen   = 0;
    g_init_completed_gen = 0;
    g_stop_entered_gen   = 0;
    g_stop_completed_gen = 0;
    g_deinit_entered_gen = 0;
    g_deinit_completed_gen = 0;

    g_init_barrier.active     = 0;
    g_init_barrier.waiting    = 0;
    g_init_barrier.released   = 0;
    g_deinit_barrier.active   = 0;
    g_deinit_barrier.waiting  = 0;
    g_deinit_barrier.released = 0;
  }

  if ( !any_active )
  {
    g_hold_scan_done          = false;
    g_init_barrier.hold_set   = false;
    g_deinit_barrier.hold_set = false;
  }

  /* A parked call already committed to waiting for its release grant.  Keep
   * its hold state and grant bookkeeping intact until every active lifecycle
   * call has exited. */
  mock_unlock();

  /* Drain stale notification and barrier tokens only for a quiescent mock.
   * While a lifecycle call is active, even its entered token is part of the
   * current round and must remain available; the generations are deliberately
   * preserved and make any older token harmless.  The next reset, after the
   * call has exited, drains the complete accumulated set. */
  if ( !any_active )
  {
    sem_drain( g_init_entered_sem );
    sem_drain( g_init_completed_sem );
    sem_drain( g_stop_entered_sem );
    sem_drain( g_stop_completed_sem );
    sem_drain( g_deinit_entered_sem );
    sem_drain( g_deinit_completed_sem );
    sem_drain( g_init_barrier.wake_sem );
    sem_drain( g_deinit_barrier.wake_sem );
  }
}

void wifi_hal_mock_set_connect_result( osal_status_t result )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  mock_lock();
  g_mock.connect_result = result;
  mock_unlock();
}

void wifi_hal_mock_set_start_result( osal_status_t result )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  mock_lock();
  g_mock.start_result = result;
  mock_unlock();
}

void wifi_hal_mock_set_init_result( osal_status_t result )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  mock_lock();
  g_init_result = result;
  mock_unlock();
}

void wifi_hal_mock_set_deinit_result( osal_status_t result )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  mock_lock();
  g_deinit_result = result;
  mock_unlock();
}

void wifi_hal_mock_set_scan_list( const wifi_hal_ap_record_t* list, uint16_t count )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  if ( count > WIFI_HAL_MOCK_MAX_AP )
  {
    count = WIFI_HAL_MOCK_MAX_AP;
  }

  mock_lock();
  g_mock.scan_count = count;
  if ( list && count > 0 )
  {
    memcpy( g_mock.scan_list, list, count * sizeof( wifi_hal_ap_record_t ) );
  }
  mock_unlock();
}

void wifi_hal_mock_set_ip_info( const wifi_hal_ip_info_t* info )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  if ( !info )
  {
    return;
  }

  mock_lock();
  g_mock.ip_info = *info;
  mock_unlock();
}

void wifi_hal_mock_inject_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  wifi_hal_event_cb_t cb;
  void*               user_data;

  if ( !mock_ensure_created() )
  {
    return;
  }

  /* Copy the callback under the mock lock (the worker may be clearing it
   * inside wifi_hal_deinit) and invoke it only after the lock is released. */
  mock_lock();
  cb        = g_mock.event_cb;
  user_data = g_mock.user_data;
  mock_unlock();

  if ( cb )
  {
    cb( event, data, user_data );
  }
}

void wifi_hal_mock_set_scan_done_hold( bool hold )
{
  if ( !mock_ensure_created() )
  {
    return;
  }

  mock_lock();
  g_hold_scan_done = hold;
  mock_unlock();
}

void wifi_hal_mock_set_init_hold( bool hold )
{
  if ( mock_ensure_created() )
  {
    barrier_set_hold( &g_init_barrier, hold );
  }
}

void wifi_hal_mock_set_deinit_hold( bool hold )
{
  if ( mock_ensure_created() )
  {
    barrier_set_hold( &g_deinit_barrier, hold );
  }
}

void wifi_hal_mock_release_init_hold( void )
{
  if ( mock_ensure_created() )
  {
    barrier_release_one( &g_init_barrier );
  }
}

void wifi_hal_mock_release_deinit_hold( void )
{
  if ( mock_ensure_created() )
  {
    barrier_release_one( &g_deinit_barrier );
  }
}

bool wifi_hal_mock_wait_init_entered( uint32_t timeout_ms )
{
  /* TASK-134 compatibility: preserve the original one-shot notification
   * semantics.  In particular, callers may drain stale acknowledgements with
   * timeout 0; the generation-based API below is the level-triggered API for
   * waits that must identify a particular lifecycle round. */
  if ( !mock_ensure_created() )
  {
    return false;
  }
  return osal_count_sem_timed_wait( g_init_entered_sem, timeout_ms ) == OSAL_SUCCESS;
}

bool wifi_hal_mock_wait_init_entered_gen( uint32_t gen, uint32_t timeout_ms )
{
  return gen_wait( &g_init_entered_gen, g_init_entered_sem, gen, timeout_ms );
}

bool wifi_hal_mock_wait_init_completed_gen( uint32_t gen, uint32_t timeout_ms )
{
  return gen_wait( &g_init_completed_gen, g_init_completed_sem, gen, timeout_ms );
}

bool wifi_hal_mock_wait_stop_entered_gen( uint32_t gen, uint32_t timeout_ms )
{
  return gen_wait( &g_stop_entered_gen, g_stop_entered_sem, gen, timeout_ms );
}

bool wifi_hal_mock_wait_stop_completed_gen( uint32_t gen, uint32_t timeout_ms )
{
  return gen_wait( &g_stop_completed_gen, g_stop_completed_sem, gen, timeout_ms );
}

bool wifi_hal_mock_wait_deinit_entered_gen( uint32_t gen, uint32_t timeout_ms )
{
  return gen_wait( &g_deinit_entered_gen, g_deinit_entered_sem, gen, timeout_ms );
}

bool wifi_hal_mock_wait_deinit_completed_gen( uint32_t gen, uint32_t timeout_ms )
{
  return gen_wait( &g_deinit_completed_gen, g_deinit_completed_sem, gen, timeout_ms );
}

void wifi_hal_mock_get_lifecycle_snapshot( wifi_hal_mock_lifecycle_snapshot_t* out )
{
  if ( !out || !mock_ensure_created() || !mock_lock() )
  {
    return;
  }

  out->initialized          = g_mock.initialized;
  out->started              = g_mock.started;
  out->init_count           = g_mock.init_count;
  out->deinit_count         = g_mock.deinit_count;
  out->stop_count           = g_mock.stop_count;
  out->start_count          = g_mock.start_count;
  out->init_entered_gen     = g_init_entered_gen;
  out->init_completed_gen   = g_init_completed_gen;
  out->stop_entered_gen     = g_stop_entered_gen;
  out->stop_completed_gen   = g_stop_completed_gen;
  out->deinit_entered_gen   = g_deinit_entered_gen;
  out->deinit_completed_gen = g_deinit_completed_gen;
  out->event_cb             = g_mock.event_cb;
  out->user_data            = g_mock.user_data;
  mock_unlock();
}

uint32_t wifi_hal_mock_get_init_count( void )
{
  uint32_t count;

  if ( !mock_ensure_created() || !mock_lock() )
  {
    return 0;
  }
  count = g_mock.init_count;
  mock_unlock();
  return count;
}

uint32_t wifi_hal_mock_get_deinit_count( void )
{
  uint32_t count;

  if ( !mock_ensure_created() || !mock_lock() )
  {
    return 0;
  }
  count = g_mock.deinit_count;
  mock_unlock();
  return count;
}

uint32_t wifi_hal_mock_get_stop_count( void )
{
  uint32_t count;

  if ( !mock_ensure_created() || !mock_lock() )
  {
    return 0;
  }
  count = g_mock.stop_count;
  mock_unlock();
  return count;
}

const wifi_hal_mock_state_t* wifi_hal_mock_get_state( void )
{
  return &g_mock;
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
  bool          should_wait;
  osal_status_t result;

  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  if ( !init )
  {
    return OSAL_INVALID_POINTER;
  }

  /* Optional AP DNS override: validate it when supplied, otherwise store as
   * omitted (non-captive DHCP keeps the platform default DNS).  Rejected
   * configurations return before any shared state is touched. */
  if ( init->ap_dns && init->ap_dns[0] != '\0' )
  {
    if ( !wifi_hal_is_valid_ipv4( init->ap_dns ) )
    {
      return OSAL_ERR_INVALID_ARGUMENT;
    }
  }

  /* Atomic lifecycle transition: the "entered" generation notification, the
   * in-flight-call accounting, the attempt counter and the decision to park on
   * the release barrier all happen under a single lock acquisition.  A
   * concurrent reset therefore either runs entirely before the call commits
   * (and sees no call in flight) or entirely after (and preserves the shared
   * state plus the release grants), so a hold/release round can never be lost
   * and reset can never clear state that the acknowledged call still uses. */
  mock_lock();
  g_init_entered_gen++;
  g_init_barrier.active++;
  g_mock.init_count++;
  if ( init->ap_dns && init->ap_dns[0] != '\0' )
  {
    strncpy( g_mock.ap_dns, init->ap_dns, sizeof( g_mock.ap_dns ) - 1 );
    g_mock.ap_dns_set = true;
  }
  else
  {
    g_mock.ap_dns[0]  = '\0';
    g_mock.ap_dns_set = false;
  }
  should_wait = g_init_barrier.hold_set;
  if ( should_wait )
  {
    g_init_barrier.waiting++;
  }
  mock_unlock();

  sem_wake( g_init_entered_sem );

  if ( should_wait )
  {
    /* Initialization barrier: hold the Wi-Fi worker inside wifi_hal_init()
     * without installing the event callback until the test releases the call. */
    barrier_wait( &g_init_barrier );
  }

  mock_lock();
  result = g_init_result;
  mock_unlock();

  if ( result != OSAL_SUCCESS )
  {
    notify_generation( &g_init_completed_gen, g_init_completed_sem );
    barrier_exit( &g_init_barrier );
    return result;
  }

  mock_lock();
  g_mock.initialized = true;
  g_mock.event_cb    = init->event_cb;
  g_mock.user_data   = init->user_data;
  mock_unlock();

  notify_generation( &g_init_completed_gen, g_init_completed_sem );
  barrier_exit( &g_init_barrier );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit( void )
{
  bool          should_wait;
  osal_status_t result;

  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  /* Atomic lifecycle transition (see wifi_hal_init).  The deinit attempt
   * counter is incremented here — in the same locked entry transition as the
   * entered generation and the in-flight accounting — so an acknowledged (held)
   * deinit attempt is already visible to the counter API, matching the init
   * counter contract.  The "not completed" signal for a held call remains the
   * completed generation, which is only raised after the call is released. */
  mock_lock();
  g_deinit_entered_gen++;
  g_deinit_barrier.active++;
  g_mock.deinit_count++;
  should_wait = g_deinit_barrier.hold_set;
  if ( should_wait )
  {
    g_deinit_barrier.waiting++;
  }
  mock_unlock();

  sem_wake( g_deinit_entered_sem );

  if ( should_wait )
  {
    /* Teardown barrier: hold the worker until the test releases the call. */
    barrier_wait( &g_deinit_barrier );
  }

  /* The deinit attempt counter was already advanced at entry (line "atomic
   * lifecycle transition"); here we only tear down shared state. */
  mock_lock();
  result             = g_deinit_result;
  g_mock.initialized = false;
  g_mock.started     = false;
  g_mock.event_cb    = NULL;
  g_mock.user_data   = NULL;
  mock_unlock();

  notify_generation( &g_deinit_completed_gen, g_deinit_completed_sem );
  barrier_exit( &g_deinit_barrier );
  return result;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  osal_status_t result;

  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  g_mock.mode       = mode;
  g_mock.start_count++;
  g_mock.started    = ( g_mock.start_result == OSAL_SUCCESS );
  result            = g_mock.start_result;
  mock_unlock();
  return result;
}

osal_status_t wifi_hal_stop( void )
{
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  g_stop_active++;
  g_stop_entered_gen++;
  g_mock.stop_count++;
  g_mock.started   = false;
  g_mock.connected = false;
  mock_unlock();

  sem_wake( g_stop_entered_sem );
  notify_generation( &g_stop_completed_gen, g_stop_completed_sem );

  mock_lock();
  if ( g_stop_active > 0 )
  {
    g_stop_active--;
  }
  mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  g_mock.sta_cfg = *config;
  mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  g_mock.ap_cfg = *config;
  mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  osal_status_t result;

  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  result = g_mock.connect_result;
  if ( result == OSAL_SUCCESS )
  {
    g_mock.connected = true;
  }
  mock_unlock();
  return result;
}

osal_status_t wifi_hal_disconnect( void )
{
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  g_mock.connected = false;
  mock_unlock();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start_scan( bool block )
{
  (void) block;

  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  bool                fire      = false;
  wifi_hal_event_cb_t cb        = NULL;
  void*               user_data = NULL;

  mock_lock();
  g_mock.scan_start_count++;
  /* If a callback is registered, fire SCAN_DONE so the management layer picks
   * up the results — unless the test has asked to hold the completion in order
   * to observe the in-flight scan window.  The callback and the hold flag are
   * read under the mock lock (the worker may be clearing the callback inside
   * wifi_hal_deinit); the callback is invoked only after the lock is
   * released. */
  if ( g_mock.event_cb != NULL && !g_hold_scan_done )
  {
    fire      = true;
    cb        = g_mock.event_cb;
    user_data = g_mock.user_data;
  }
  mock_unlock();

  if ( fire && cb != NULL )
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
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  uint16_t to_copy = g_mock.scan_count;
  if ( to_copy > *in_out_count )
  {
    to_copy = *in_out_count;
  }

  memcpy( records, g_mock.scan_list, to_copy * sizeof( wifi_hal_ap_record_t ) );
  mock_unlock();
  *in_out_count = to_copy;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  *out_info = g_mock.ip_info;
  mock_unlock();
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
  if ( !mock_ensure_created() )
  {
    return OSAL_ERROR;
  }

  mock_lock();
  g_mock.power_save = enabled;
  mock_unlock();
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