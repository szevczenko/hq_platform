/*
 * POSIX Wi-Fi HAL — lifecycle teardown regression tests
 *
 * Exercises the callback-quiescent, session-operation-barrier, repeat-safe
 * deinit contract of the real POSIX HAL simulator
 * (src/wifi/platforms/posix/wifi_hal_driver.c):
 *   1. deinit before any init is a successful no-op,
 *   2. init/deinit/deinit: repeated deinit is a no-op and a later init works,
 *   3. partial primitive-init unwind: an injected pthread_mutex_init failure
 *      part-way through session creation rolls back every primitive created so
 *      far (exact reverse order),
 *   4. partial primitive-init unwind that itself fails: the session enters
 *      CLEANUP_REQUIRED, admission stays closed, and a deinit retry releases
 *      the retained primitive,
 *   5. pthread_create failure: a failed timer create leaves no joinable handle,
 *      so deinit joins nothing and succeeds,
 *   6. join failure: deinit returns an error with callbacks disabled and the
 *      exact thread handle retained; a retry deinit re-attempts the same join
 *      and completes,
 *   7. a public scan operation admitted while deinit begins: deinit waits for
 *      the admitted operation and operations arriving once teardown has begun
 *      are rejected without touching session resources,
 *   8. a connect (timer-start) operation admitted while deinit begins: deinit
 *      waits for the admitted operation, then cancels and joins the timer
 *      before destroying the per-session primitives,
 *   9. a timer-driven event racing deinit is cancelled and joined,
 *  10. an in-flight callback racing deinit is waited out (quiescence), and a
 *      callback that re-enters a HAL API does not deadlock,
 *  11. repeated init/deinit cycles never reuse destroyed pthread objects,
 *  12. a timer thread that completed on its own is still joined by deinit,
 *  13. wifi_hal_stop before any init is a successful idempotent no-op,
 *  14. init/stop/stop/deinit/stop/deinit: repeated stop and stop after a
 *      complete deinit are no-ops and a fresh stop/deinit round reports both
 *      calls successful,
 *  15. an injected stop join failure retains the exact join source
 *      (CLEANUP_REQUIRED); a retry stop re-attaches the same join and only
 *      then succeeds, and deinit completes the whole session afterwards.
 *
 * Cross-thread callback rendezvous use semaphores (the callback posts an
 * "entered" semaphore and blocks on a gate the main thread releases); lifecycle
 * transition rendezvous use the HAL's condition variable.  No volatile
 * polling or clock()-based busy waits are needed to make the races
 * deterministic.
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"
#include "wifi_hal_driver.h"

#ifdef WIFI_HAL_POSIX_TESTING
/* Failure-injection API compiled into the HAL only under
 * WIFI_HAL_POSIX_TESTING.  Each function fails the (n+1)-th call of the
 * selected pthread primitive (n successful calls happen first, then one
 * failure), resetting afterwards. */
void wifi_hal_testing_fail_pthread_create_after( int32_t successes );
void wifi_hal_testing_fail_pthread_join_after( int32_t successes );
void wifi_hal_testing_fail_mutex_init_after( int32_t successes );
void wifi_hal_testing_fail_cond_init_after( int32_t successes );
void wifi_hal_testing_fail_mutex_destroy_after( int32_t successes );
void wifi_hal_testing_fail_cond_destroy_after( int32_t successes );
void wifi_hal_testing_reset( void );
void wifi_hal_testing_wait_for_deinit_started( void );
#endif

/* -------------------------------------------------------------------------- */
/*  Shared helpers                                                            */
/* -------------------------------------------------------------------------- */

static wifi_hal_init_t _make_init( const char*       ap_dns,
                                   wifi_hal_event_cb_t cb,
                                   void*            user_data )
{
  wifi_hal_init_t init = {
    .ap_ip      = "192.168.1.1",
    .ap_gateway = "192.168.1.1",
    .ap_netmask = "255.255.255.0",
    .event_cb   = cb,
    .user_data  = user_data,
  };
  init.ap_dns = ap_dns;
  return init;
}

/* Ensure a clean session between tests.  Safe whether or not the previous test
 * left the HAL initialized (deinit of an uninitialized HAL is a no-op). */
static void _reset_session( void )
{
  (void) wifi_hal_deinit();
}

static void _sleep_ms( uint32_t ms )
{
  struct timespec ts;
  ts.tv_sec  = (time_t) ( ms / 1000U );
  ts.tv_nsec = (long) ( ( ms % 1000U ) * 1000000UL );
  (void) nanosleep( &ts, NULL );
}

/* Bounded semaphore wait used for every cross-thread rendezvous.  The bound
 * only guards against a genuinely broken HAL (infinite hang); the normal path
 * always posts the semaphore deterministically. */
static void _wait_sem( sem_t* sem, const char* what )
{
  struct timespec ts;
  clock_gettime( CLOCK_REALTIME, &ts );
  ts.tv_sec += 10;
  int rc = sem_timedwait( sem, &ts );
  TEST_ASSERT_EQUAL_INT32_MESSAGE( 0, rc, what );
}

/* -------------------------------------------------------------------------- */
/*  Per-test callback state                                                   */
/* -------------------------------------------------------------------------- */

static uint32_t g_cb_a_events;
static uint32_t g_cb_b_events;

static void _cb_count_a( wifi_hal_event_t             event,
                         const wifi_hal_event_data_t* data,
                         void*                      user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
  g_cb_a_events++;
}

static void _cb_count_b( wifi_hal_event_t             event,
                         const wifi_hal_event_data_t* data,
                         void*                      user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
  g_cb_b_events++;
}

/* -------------------------------------------------------------------------- */
/*  Race-test state                                                            */
/* -------------------------------------------------------------------------- */

static sem_t g_cb_gate;          /* Released by main to unblock the callback. */
static sem_t g_cb_entered_sem;  /* Posted by the callback when it blocks.    */
static sem_t g_connect_cb_gate; /* Gate for the connect-gate callback.       */

static bool           g_cb_finished;    /* Callback returned.                */
static osal_status_t  g_deinit_result;  /* Deinit thread return value.       */
static osal_status_t  g_producer_result; /* Producer thread return value.     */
static uint32_t       g_race_events;    /* Total deliveries observed.        */
static bool           g_nested_done;    /* One nested delivery was exercised. */

static pthread_t g_deinit_thread;
static pthread_t g_producer_thread;

/* Callback used for the SCAN_DONE race tests.  The outer delivery (first
 * SCAN_DONE) re-enters wifi_hal_start_scan once — proving the callback is
 * invoked without any HAL lock — then blocks on g_cb_gate until the main
 * thread releases it.  Later deliveries simply count. */
static void race_event_cb( wifi_hal_event_t             event,
                           const wifi_hal_event_data_t* data,
                           void*                        user_data )
{
  (void) data;
  (void) user_data;
  g_race_events++;

  if ( event == WIFI_HAL_EVT_SCAN_DONE && !g_nested_done )
  {
    g_nested_done = true;
    (void) wifi_hal_start_scan( false );
    (void) sem_post( &g_cb_entered_sem );
    (void) sem_wait( &g_cb_gate );
    g_cb_finished = true;
  }
}

/* Callback for the connect (timer-start) race test: blocks the connect
 * operation mid-flight on the first delivered event (GOT_IP). */
static void connect_gate_cb( wifi_hal_event_t             event,
                             const wifi_hal_event_data_t* data,
                             void*                        user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
  g_cb_a_events++;
  (void) sem_post( &g_cb_entered_sem );
  (void) sem_wait( &g_connect_cb_gate );
}

static void* producer_start_scan( void* arg )
{
  (void) arg;
  (void) wifi_hal_start_scan( false );
  return NULL;
}

static void* producer_connect( void* arg )
{
  (void) arg;
  g_producer_result = wifi_hal_connect();
  return NULL;
}

static void* deinit_runner( void* arg )
{
  (void) arg;
  g_deinit_result = wifi_hal_deinit();
  return NULL;
}

/* Sanity helper: configure the station for a given simulated AP (started). */
static void _setup_sta( const char* ssid, const char* password )
{
  wifi_hal_sta_config_t sta_config = { 0 };
  strncpy( sta_config.ssid, ssid, sizeof( sta_config.ssid ) - 1 );
  strncpy( sta_config.password, password, sizeof( sta_config.password ) - 1 );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ),
                             "start station mode" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_set_sta_config( &sta_config ),
                             "set station config" );
}

/* ============================================================================
 * Test 1: Deinit before init is a successful no-op
 * ========================================================================== */
static void test_deinit_before_init( void )
{
  _reset_session();

  /* Never initialized: must be a successful no-op that touches nothing. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit before init is a no-op" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "repeated deinit before init is a no-op" );

  /* The no-op must not have corrupted anything: a full lifecycle still works. */
  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ),
                             "init after deinit-before-init works" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit after normal init works" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit after deinit is a no-op" );
}

/* ============================================================================
 * Test 2: init / deinit / deinit — repeated teardown is safe and a fresh
 *         session does not touch stale callback state
 * ========================================================================== */
static void test_init_deinit_deinit( void )
{
  _reset_session();

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ),
                             "first init succeeds" );

  /* Repeated init of an already initialized session is an idempotent no-op. */
  wifi_hal_init_t init2 = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init2 ),
                             "repeated init is an idempotent no-op" );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "first deinit succeeds" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "second deinit is a successful no-op" );

  /* A fresh session must deliver to the new callback only. */
  g_cb_a_events = 0;
  g_cb_b_events = 0;
  wifi_hal_init_t init3 = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init3 ),
                             "re-init after deinit/deinit succeeds" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "scan accepted in the fresh session" );
  TEST_ASSERT_EQUAL_MESSAGE( 0u, g_cb_a_events,
                             "stale session callback never invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events,
                             "fresh session callback receives events" );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 3: Partial primitive-init unwind (injected pthread_mutex_init failure)
 *         releases the primitives created before the failing step; deinit stays
 *         a safe no-op and a retry works.  This replaces the old "invalid
 *         ap_dns" partial-init injection, which never proved rollback from a
 *         pthread creation failure.
 * ========================================================================== */
static void test_primitive_init_unwind( void )
{
#ifdef WIFI_HAL_POSIX_TESTING
  _reset_session();
  wifi_hal_testing_reset();

  /* Session primitive creation order: disc_mutex, disc_cond, conn_mutex,
   * conn_cond, state_mutex, cb_mutex, cb_cond.  Fail the 3rd mutex init
   * (state_mutex): four primitives created before it must be unwound. */
  wifi_hal_testing_fail_mutex_init_after( 2 );

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_init( &init ),
                             "init fails at the injected mutex init" );

  /* The unwind released everything: deinit is a no-op and a retry works. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit after failed init is a safe no-op" );

  wifi_hal_init_t init2 = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init2 ),
                             "re-init after partial-init failure works" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
#endif
}

/* ============================================================================
 * Test 4: Partial primitive-init unwind that itself fails.  The session is
 *         left in CLEANUP_REQUIRED with the exact validity flags retained;
 *         admission stays closed (operations fail without touching session
 *         objects), and a deinit retry releases the retained primitive.
 * ========================================================================== */
static void test_primitive_init_unwind_partial( void )
{
#ifdef WIFI_HAL_POSIX_TESTING
  _reset_session();
  wifi_hal_testing_reset();

  /* Fail the last cond init (cb_cond) and make the reverse-order unwind fail
   * on its second destroy (state_mutex).  The unwind is left partial. */
  wifi_hal_testing_fail_cond_init_after( 2 );       /* 3rd cond init = cb_cond */
  wifi_hal_testing_fail_mutex_destroy_after( 1 );   /* 2nd destroy = state_mutex */

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_init( &init ),
                             "init fails with incomplete unwind" );

  /* CLEANUP_REQUIRED: admission is closed, so operations fail without locking
   * or destroying the partially released session objects. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "operations are rejected in CLEANUP_REQUIRED" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_set_power_save( true ),
                             "configuration calls are rejected too" );

  /* Init cannot replace a retained session; deinit owns the retry. */
  wifi_hal_init_t blocked_init = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_init( &blocked_init ),
                             "init cannot replace cleanup-required session" );

  /* Deinit retries and completes the release; a fresh session then works. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit retry completes the partial unwind" );

  wifi_hal_init_t init2 = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init2 ),
                             "fresh init after cleanup retry" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
#endif
}

/* ============================================================================
 * Test 5: A failed timer-thread pthread_create publishes no joinable handle:
 *         connect reports the failure and deinit joins nothing (so no garbage
 *         handle is ever passed to pthread_join) and succeeds.
 * ========================================================================== */
static void test_pthread_create_failure( void )
{
#ifdef WIFI_HAL_POSIX_TESTING
  _reset_session();
  wifi_hal_testing_reset();

  g_cb_a_events = 0;

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  _setup_sta( "disconnect_15_sec", "12345678" );

  /* The next pthread_create (the disconnect timer) fails.  connect() already
   * delivered GOT_IP, then returns the timer-start failure. */
  wifi_hal_testing_fail_pthread_create_after( 0 );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_connect(),
                             "connect reports the failed timer create" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events,
                             "GOT_IP still delivered before the timer start" );

  /* No thread was published: deinit must not join a garbage handle. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit succeeds with no joinable handle" );

  /* Session fully reusable. */
  g_cb_b_events = 0;
  wifi_hal_init_t reuse = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &reuse ), "re-init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "fresh callback invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
#endif
}

/* ============================================================================
 * Test 6: pthread_join failure during deinit.  Deinit returns an error, keeps
 *         the exact thread handle and per-session primitives, disables
 *         callbacks and admission (CLEANUP_REQUIRED); a retry deinit
 *         re-attaches the same join and completes.
 * ========================================================================== */
static void test_join_failure_then_deinit_retry( void )
{
#ifdef WIFI_HAL_POSIX_TESTING
  _reset_session();
  wifi_hal_testing_reset();

  g_cb_a_events = 0;

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  _setup_sta( "disconnect_15_sec", "12345678" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted (timer running)" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events, "GOT_IP delivered" );

  /* Fail the single pthread_join deinit performs on the disconnect timer. */
  wifi_hal_testing_fail_pthread_join_after( 0 );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_deinit(),
                             "deinit reports the join failure" );

  /* Teardown failed: callbacks are disabled and admission is closed, so
   * further operations fail without touching the retained session objects. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "admission closed in CLEANUP_REQUIRED" );

  /* Retry deinit re-attaches the exact same join, then drains and destroys. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit retry joins and completes" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "uninitialized session rejects operations" );

  /* Fresh session: no stale deliveries from the old one. */
  g_cb_b_events = 0;
  wifi_hal_init_t reuse = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &reuse ), "re-init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "fresh callback invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events,
                             "no late delivery from the failed-teardown session" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
#endif
}

/* ============================================================================
 * Test 7: A public scan operation admitted while deinit begins.  Deinit closes
 *         admission and waits for the admitted operation; operations arriving
 *         once teardown has begun are rejected without touching session
 *         resources; after the operation drains, deinit completes.
 * ========================================================================== */
static void test_op_admitted_while_deinit( void )
{
  _reset_session();

  g_race_events  = 0;
  g_nested_done  = false;
  g_cb_finished  = false;
  g_cb_a_events  = 0;
  g_cb_b_events  = 0;

  wifi_hal_init_t init = _make_init( NULL, race_event_cb, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  /* Producer fires SCAN_DONE synchronously; the callback blocks, keeping the
   * scan operation admitted and in flight while deinit begins. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_producer_thread, NULL,
                                                producer_start_scan, NULL ),
                             "producer thread created" );
  _wait_sem( &g_cb_entered_sem, "callback entered" );

  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_deinit_thread, NULL,
                                                deinit_runner, NULL ),
                             "deinit thread created" );
  /* Wait for the actual ACTIVE -> DEINITIALIZING transition.  The admitted
   * scan callback remains blocked, so deinit cannot have completed here. */
#ifdef WIFI_HAL_POSIX_TESTING
  wifi_hal_testing_wait_for_deinit_started();
#endif

  /* The transition was observed above, so this probe is unambiguously a call
   * arriving during teardown.  It must fail without touching session objects. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "operations arriving during teardown are rejected" );

  /* Release the callback: the admitted scan drains and deinit completes. */
  (void) sem_post( &g_cb_gate );
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_join( g_deinit_thread, NULL ),
                             "deinit thread joined" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, g_deinit_result,
                             "deinit succeeded after the admitted op drained" );
  TEST_ASSERT_TRUE_MESSAGE( g_cb_finished,
                            "blocked callback returned before deinit did" );

  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_join( g_producer_thread, NULL ),
                             "producer thread joined" );

  /* After deinit nothing is admitted either. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "post-deinit operations fail as uninitialized" );

  /* Session is reusable with a fresh callback. */
  wifi_hal_init_t reinit = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &reinit ), "re-init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "new callback sees the event" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 8: A connect (timer-start) operation admitted while deinit begins.  The
 *         GOT_IP callback keeps the connect operation admitted and blocked;
 *         deinit waits for it, then cancels + joins the disconnect timer the
 *         operation started before destroying the session primitives.
 * ========================================================================== */
static void test_timer_start_op_admitted_while_deinit( void )
{
  _reset_session();

  g_cb_a_events     = 0;
  g_cb_b_events     = 0;
  g_producer_result = OSAL_ERROR;

  wifi_hal_init_t init = _make_init( NULL, connect_gate_cb, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  _setup_sta( "disconnect_15_sec", "12345678" );

  /* connect() is admitted; its GOT_IP callback blocks, so the operation cannot
   * release its lease yet and the disconnect timer is not started until the
   * callback returns. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_producer_thread, NULL,
                                                producer_connect, NULL ),
                             "connect thread created" );
  _wait_sem( &g_cb_entered_sem, "connect callback blocked" );

  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_deinit_thread, NULL,
                                                deinit_runner, NULL ),
                             "deinit thread created" );
#ifdef WIFI_HAL_POSIX_TESTING
  wifi_hal_testing_wait_for_deinit_started();
#endif

  /* Release the callback: connect starts the 15 s timer, returns and releases
   * its lease; deinit then cancels + joins the timer before destroying the
   * session primitives. */
  (void) sem_post( &g_connect_cb_gate );
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_join( g_deinit_thread, NULL ),
                             "deinit thread joined" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, g_deinit_result,
                             "deinit joined the timer and succeeded" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_join( g_producer_thread, NULL ),
                             "connect thread joined" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, g_producer_result,
                             "connect completed after unblocking" );

  /* The timer was joined (not leaked): a short grace window shows no event
   * firing from the old session after deinit. */
  _sleep_ms( 200 );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events,
                             "no timer callback after deinit" );

  /* Fresh session is fully functional afterwards. */
  wifi_hal_init_t reuse = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &reuse ), "re-init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "fresh callback invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 9: Timer-driven event racing deinit — the slow-connect background
 *         thread is cancelled and joined, so its GOT_IP event never fires.
 * ========================================================================== */
static void test_event_racing_deinit_timer( void )
{
  _reset_session();

  g_race_events = 0;
  g_nested_done = false;

  wifi_hal_init_t init = _make_init( NULL, race_event_cb, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  _setup_sta( "slow_connect", "12345678" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted (GOT arrives after 3 s)" );

  /* Deinit immediately: it must cancel + join the timer thread before the 3 s
   * deadline and before destroying the per-session primitives. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit races the slow-connect timer" );

  /* Wait out the original 3 s window: the GOT_IP must never have fired. */
  _sleep_ms( 3200 );
  TEST_ASSERT_EQUAL_MESSAGE( 0u, g_race_events,
                             "no callback delivered after timer cancelled by deinit" );
}

/* ============================================================================
 * Test 10: A callback already in flight races deinit: deinit blocks until the
 * callback returns (quiescence barrier), then completes.
 * ========================================================================== */
static void test_callback_inflight_racing_deinit( void )
{
  _reset_session();

  g_cb_finished  = false;
  g_race_events  = 0;
  g_nested_done  = false;
  g_cb_a_events  = 0;
  g_cb_b_events  = 0;

  wifi_hal_init_t init = _make_init( NULL, race_event_cb, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  /* Producer: fires SCAN_DONE synchronously; the callback blocks mid-flight. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_producer_thread, NULL,
                                                producer_start_scan, NULL ),
                             "producer thread created" );
  _wait_sem( &g_cb_entered_sem, "callback reached its blocking point" );

  /* Deinit in a separate thread: it must NOT return while the callback is
   * blocked inside the HAL event delivery. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_deinit_thread, NULL,
                                                deinit_runner, NULL ),
                             "deinit thread created" );
#ifdef WIFI_HAL_POSIX_TESTING
  wifi_hal_testing_wait_for_deinit_started();
#endif

  /* The callback is still blocked, so deinit is necessarily waiting for its
   * in-flight delivery and cannot have completed yet. */

  /* Unblock the in-flight callback; deinit may now complete. */
  (void) sem_post( &g_cb_gate );
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_join( g_deinit_thread, NULL ),
                             "deinit thread joined" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, g_deinit_result,
                             "deinit succeeded after quiescence" );
  TEST_ASSERT_TRUE_MESSAGE( g_cb_finished,
                            "in-flight callback returned before deinit did" );
  TEST_ASSERT_EQUAL_MESSAGE( 2u, g_race_events,
                             "outer + re-entrant delivery observed" );

  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_join( g_producer_thread, NULL ),
                             "producer thread joined" );

  /* The session is quiescent: a fresh init delivers only to the new callback. */
  wifi_hal_init_t reinit = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &reinit ), "re-init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "new session scan" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "new callback sees the event" );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 11: Repeated init/deinit cycles.  Every cycle must create and destroy a
 * fresh set of per-session pthread objects without reusing a destroyed object,
 * and no stale callback may be delivered across sessions.
 * ========================================================================== */
static void test_repeat_init_deinit_cycles( void )
{
  _reset_session();

  const int cycles = 100;
  g_cb_a_events    = 0;

  for ( int i = 0; i < cycles; ++i )
  {
    wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
    TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "cycle init" );
    TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                               "cycle scan accepted" );
    TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cycle deinit" );
    TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                               "cycle second deinit is a no-op" );
  }

  /* Every cycle delivered exactly one event; nothing leaked across sessions. */
  TEST_ASSERT_EQUAL_MESSAGE( (uint32_t) cycles, g_cb_a_events,
                             "one callback delivered per cycle" );

  /* A fresh session with a new callback proves the old state is gone. */
  g_cb_a_events = 0;
  g_cb_b_events = 0;
  wifi_hal_init_t fresh = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &fresh ), "fresh init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh scan accepted" );
  TEST_ASSERT_EQUAL_MESSAGE( 0u, g_cb_a_events,
                             "no stale callback after cycles" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "fresh callback invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 12: A timer thread that completed on its own is still joined by deinit.
 *          The slow-connect thread runs to its natural 3 s deadline, fires
 *          GOT_IP, and exits.  Deinit must nevertheless join it (its resources
 *          are only reclaimed by pthread_join()) before destroying the
 *          per-session primitives, and the session must be reusable afterwards.
 * ========================================================================== */
static void test_deinit_after_natural_timer_completion( void )
{
  _reset_session();

  g_cb_a_events = 0;

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  _setup_sta( "slow_connect", "12345678" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted" );

  /* Let the slow-connect thread run to completion: after 3 s it fires GOT_IP,
   * then exits — remaining joinable until joined. */
  _sleep_ms( 3500 );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events,
                             "slow connect delivered GOT_IP on its own" );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit joins the naturally completed thread" );

  /* The source is stopped and joined: no second delivery can occur. */
  _sleep_ms( 200 );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events,
                             "no callback after deinit" );

  /* A fresh session must be fully functional after the join. */
  g_cb_b_events = 0;
  wifi_hal_init_t reuse = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &reuse ), "re-init" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "re-init scan accepted" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "fresh callback invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 13: wifi_hal_stop is an idempotent successful no-op before any init.
 *         Calls stop-before-init, then proves a full lifecycle still works.
 * ========================================================================== */
static void test_stop_before_init( void )
{
  _reset_session();

  /* Never initialized: no started runtime can remain, so stop is a no-op. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "stop before init is a successful no-op" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "repeated stop before init is a no-op" );

  /* The no-op must not have corrupted anything: a full lifecycle still works. */
  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ),
                             "init after stop-before-init works" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit after normal init works" );
}

/* ============================================================================
 * Test 14: init / stop / stop / deinit / stop / deinit — repeated stop, stop
 *          after a complete deinit, and a fresh complete round all return
 *          success from both idempotent calls.
 * ========================================================================== */
static void test_init_stop_stop_deinit_stop_deinit( void )
{
  _reset_session();

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  /* Stop with nothing started: successful no-op in the active session. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "stop with nothing started is a no-op" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "repeated stop is a no-op" );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "deinit" );

  /* Stop after a complete deinit (UNINITIALIZED): idempotent no-op. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "stop after deinit is a no-op" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit after deinit is a no-op" );

  /* A fresh complete round must return success from both calls. */
  wifi_hal_init_t fresh = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &fresh ),
                             "fresh init for a clean round" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "fresh round stop succeeds" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "fresh round deinit succeeds" );
}

/* ============================================================================
 * Test 15: an injected stop join failure leaves the session CLEANUP_REQUIRED
 *          with the exact retained join source; a later stop retries the same
 *          release and only then returns success; deinit then completes the
 *          whole session so a fresh stop/deinit round is fully clean.
 * ========================================================================== */
static void test_stop_join_failure_then_retry( void )
{
#ifdef WIFI_HAL_POSIX_TESTING
  _reset_session();
  wifi_hal_testing_reset();

  g_cb_a_events = 0;

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  /* Slow-connect starts a joinable connection timer owned by the session. */
  _setup_sta( "slow_connect", "12345678" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted (conn timer running)" );

  /* Fail the single join this stop performs on the slow-connect timer. */
  wifi_hal_testing_fail_pthread_join_after( 0 );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_stop(),
                             "stop reports the injected join failure" );

  /* The retained source is still held: admission is closed (CLEANUP_REQUIRED)
   * and we never infer success from the lifecycle state alone. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "admission closed while cleanup-required" );

  /* Retry stop re-attaches the exact same join and releases the source; only
   * now may it report success. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "stop retry joins and releases the timer" );

  /* Deinit completes the whole teardown, after which a fresh stop/deinit round
   * is again clean. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit completes the whole teardown" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "post-deinit stop is a no-op" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "post-deinit deinit is a no-op" );

  /* Fresh session: no stale deliveries and a full successful round. */
  g_cb_b_events = 0;
  wifi_hal_init_t fresh = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &fresh ),
                             "fresh init works after the retry round" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "fresh round stop succeeds" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "fresh round deinit succeeds" );
  (void) g_cb_a_events;
#endif
}
/* ============================================================================
 * Test 16: a deinit join failure stops before the session-state-clear stage,
 *          leaving the HAL CLEANUP_REQUIRED with g_sim.started still true (a
 *          started runtime the failed teardown never got to release).  A later
 *          wifi_hal_stop() retries the whole retained stop-owned release — the
 *          joinable timer AND the started runtime — and only then reports
 *          success; deinit completes the session afterwards.
 * ========================================================================== */
static void test_cleanup_required_retains_started_stop_retry( void )
{
#ifdef WIFI_HAL_POSIX_TESTING
  _reset_session();
  wifi_hal_testing_reset();

  g_cb_a_events = 0;

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  /* Start a session and open a stop-owned disconnect timer. */
  _setup_sta( "disconnect_15_sec", "12345678" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted (disconnect timer running)" );
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_a_events, "GOT_IP delivered" );

  /* Deinit fails on the disconnect-timer join before the state-clear stage:
   * the lifecycle becomes CLEANUP_REQUIRED and the runtime stays started. */
  wifi_hal_testing_fail_pthread_join_after( 0 );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_deinit(),
                             "deinit stops at the retained join" );

  /* Admission is closed and stop must not infer success from that state. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERROR, wifi_hal_start_scan( false ),
                             "admission closed in CLEANUP_REQUIRED" );

  /* The retry stop releases the retained timer AND the started runtime; only
   * a genuinely-complete release reports success. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "retry stop releases retained timer + runtime" );

  /* Deinit then completes the whole teardown, after which a fresh stop/deinit
   * round is again clean. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit completes the whole teardown" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "post-deinit stop is a no-op" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "post-deinit deinit is a no-op" );

  /* Fresh session: a full clean round still works. */
  g_cb_b_events = 0;
  wifi_hal_init_t fresh = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &fresh ),
                             "fresh init works after the retry round" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_stop(),
                             "fresh round stop succeeds" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "fresh round deinit succeeds" );
  TEST_ASSERT_EQUAL_MESSAGE( 0u, g_cb_b_events,
                             "fresh session delivers no stale events" );
#endif
}

/* -------------------------------------------------------------------------- */
/*  Runner                                                                    */
/* -------------------------------------------------------------------------- */

void setUp( void )
{
  TEST_ASSERT_EQUAL_MESSAGE( 0, sem_init( &g_cb_gate, 0, 0 ), "sem init gate" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, sem_init( &g_cb_entered_sem, 0, 0 ),
                             "sem init entered" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, sem_init( &g_connect_cb_gate, 0, 0 ),
                             "sem init connect gate" );
}

void tearDown( void )
{
  (void) sem_destroy( &g_cb_gate );
  (void) sem_destroy( &g_cb_entered_sem );
  (void) sem_destroy( &g_connect_cb_gate );
}

void wifi_hal_posix_tests_run( void )
{
  RUN_TEST( test_deinit_before_init );
  RUN_TEST( test_init_deinit_deinit );
  RUN_TEST( test_primitive_init_unwind );
  RUN_TEST( test_primitive_init_unwind_partial );
  RUN_TEST( test_pthread_create_failure );
  RUN_TEST( test_join_failure_then_deinit_retry );
  RUN_TEST( test_op_admitted_while_deinit );
  RUN_TEST( test_timer_start_op_admitted_while_deinit );
  RUN_TEST( test_event_racing_deinit_timer );
  RUN_TEST( test_callback_inflight_racing_deinit );
  RUN_TEST( test_repeat_init_deinit_cycles );
  RUN_TEST( test_deinit_after_natural_timer_completion );
  RUN_TEST( test_stop_before_init );
  RUN_TEST( test_init_stop_stop_deinit_stop_deinit );
  RUN_TEST( test_stop_join_failure_then_retry );
  RUN_TEST( test_cleanup_required_retains_started_stop_retry );
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  wifi_hal_posix_tests_run();

#ifndef ESP_PLATFORM
  return UNITY_END();
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */