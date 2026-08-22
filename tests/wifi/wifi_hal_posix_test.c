/*
 * POSIX Wi-Fi HAL — lifecycle teardown regression tests
 *
 * Exercises the callback-quiescent, repeat-safe deinit contract of the real
 * POSIX HAL simulator (src/wifi/platforms/posix/wifi_hal_driver.c):
 *   1. deinit before any init is a successful no-op,
 *   2. init/deinit/deinit: repeated deinit is a no-op and a later init works,
 *   3. partial-init cleanup (failure injected through the public API) leaves
 *      the session fully uninitialized,
 *   4. an event racing deinit: a timer-driven event source is stopped and
 *      joined, and an in-flight callback is waited out before deinit returns.
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

/* -------------------------------------------------------------------------- */
/*  Shared helpers                                                            */
/* -------------------------------------------------------------------------- */

static wifi_hal_init_t _make_init( const char*      ap_dns,
                                   wifi_hal_event_cb_t cb,
                                   void*           user_data )
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

/* -------------------------------------------------------------------------- */
/*  Per-test callback state                                                   */
/* -------------------------------------------------------------------------- */

static uint32_t g_cb_a_events;
static uint32_t g_cb_b_events;

static void _cb_count_a( wifi_hal_event_t       event,
                         const wifi_hal_event_data_t* data,
                         void*                  user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
  g_cb_a_events++;
}

static void _cb_count_b( wifi_hal_event_t       event,
                         const wifi_hal_event_data_t* data,
                         void*                  user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
  g_cb_b_events++;
}

/* -------------------------------------------------------------------------- */
/*  Race-test state                                                           */
/* -------------------------------------------------------------------------- */

static sem_t g_cb_gate;      /* Unblocked by the main thread.                 */
static volatile bool g_cb_entered;  /* Callback reached the blocking point.   */
static volatile bool g_cb_finished;  /* Callback returned.                    */
static volatile bool g_deinit_done;   /* Deinit thread finished.              */
static osal_status_t g_deinit_result; /* Deinit thread return value.          */
static uint32_t g_race_events;        /* Total deliveries observed.            */
static bool g_nested_done;            /* One nested delivery was exercised.    */

static pthread_t g_deinit_thread;
static pthread_t g_producer_thread;

static void race_event_cb( wifi_hal_event_t       event,
                           const wifi_hal_event_data_t* data,
                           void*                  user_data )
{
  (void) data;
  (void) user_data;
  g_race_events++;

  if ( event == WIFI_HAL_EVT_SCAN_DONE && !g_nested_done )
  {
    /* Outer delivery: re-enter the HAL from inside the callback.  If the HAL
     * held a lifecycle/callback lock across the callback this nested delivery
     * would deadlock, proving the callback is invoked without any HAL lock. */
    g_nested_done = true;
    (void) wifi_hal_start_scan( false );
    g_cb_entered  = true;
    (void) sem_wait( &g_cb_gate );
    g_cb_finished = true;
  }
}

static void* producer_start_scan( void* arg )
{
  (void) arg;
  (void) wifi_hal_start_scan( false );
  return NULL;
}

/* Waits (bounded) for the callback to reach its blocking point. */
static bool _wait_cb_entered( uint32_t timeout_ms )
{
  const uint32_t start = (uint32_t) clock() / ( CLOCKS_PER_SEC / 1000U );
  while ( !g_cb_entered )
  {
    uint32_t now = (uint32_t) clock() / ( CLOCKS_PER_SEC / 1000u );
    if ( now - start >= timeout_ms )
    {
      return false;
    }
    _sleep_ms( 5 );
  }
  return true;
}

static void* deinit_runner( void* arg )
{
  (void) arg;
  g_deinit_result = wifi_hal_deinit();
  g_deinit_done   = true;
  return NULL;
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
 * Test 3: Partial-init failure (injected via invalid ap_dns after the
 *         per-session primitives were allocated) leaves the session fully
 *         uninitialized; deinit stays a no-op and a retry works.
 * ========================================================================== */
static void test_partial_init_cleanup( void )
{
  _reset_session();

  wifi_hal_init_t init = _make_init( "999.1.1.1", _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERR_INVALID_ARGUMENT, wifi_hal_init( &init ),
                             "invalid ap_dns rejected after primitive creation" );

  /* The failed init must have released its resources: deinit is a no-op. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit after failed init is a safe no-op" );

  /* A complete lifecycle must work afterwards. */
  wifi_hal_init_t init2 = _make_init( NULL, _cb_count_b, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init2 ),
                             "re-init after partial-init failure works" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start_scan( false ),
                             "fresh session accepts a scan" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 4a: Timer-driven event racing deinit — the slow-connect background
 *          thread is cancelled and joined, so its GOT_IP event never fires.
 * ========================================================================== */
static void test_event_racing_deinit_timer( void )
{
  _reset_session();

  g_race_events = 0;
  g_nested_done = false;

  wifi_hal_init_t init = _make_init( NULL, race_event_cb, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  wifi_hal_sta_config_t sta_config = { 0 };
  strncpy( sta_config.ssid, "slow_connect", sizeof( sta_config.ssid ) - 1 );
  strncpy( sta_config.password, "12345678", sizeof( sta_config.password ) - 1 );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ),
                             "start station mode" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_set_sta_config( &sta_config ),
                             "set slow-connect credentials" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted (GOT arrives after 3 s)" );

  /* Deinit immediately: it must cancel+join the timer thread before the 3 s
   * deadline and before destroying the per-session primitives. */
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(),
                             "deinit races the slow-connect timer" );

  /* Wait out the original 3 s window: the GOT_IP must never have fired. */
  _sleep_ms( 3200 );
  TEST_ASSERT_EQUAL_MESSAGE( 0u, g_race_events,
                             "no callback delivered after timer cancelled by deinit" );
}

/* ============================================================================
 * Test 5: Repeated init/deinit cycles.  Every cycle must create and destroy a
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
 * Test 6: A timer thread that completed on its own is still joined by deinit.
 *          The slow-connect thread runs to its natural 3 s deadline, fires
 *          GOT_IP, and marks itself inactive.  Deinit must nevertheless join
 *          it (its resources are only reclaimed by pthread_join()) before
 *          destroying the per-session primitives, and the session must be
 *          reusable afterwards.
 * ========================================================================== */
static void test_deinit_after_natural_timer_completion( void )
{
  _reset_session();

  g_cb_a_events = 0;

  wifi_hal_init_t init = _make_init( NULL, _cb_count_a, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  wifi_hal_sta_config_t sta_config = { 0 };
  strncpy( sta_config.ssid, "slow_connect", sizeof( sta_config.ssid ) - 1 );
  strncpy( sta_config.password, "12345678", sizeof( sta_config.password ) - 1 );

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ),
                             "start station mode" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_set_sta_config( &sta_config ),
                             "set slow-connect credentials" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_connect(),
                             "connect accepted" );

  /* Let the slow-connect thread run to completion: after 3 s it fires GOT_IP,
   * then writes inactive and exits — remaining joinable until joined.  The
   * margin accounts for scheduling delays under load. */
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
  TEST_ASSERT_EQUAL_MESSAGE( 1u, g_cb_b_events, "new callback invoked" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_deinit(), "cleanup deinit" );
}

/* ============================================================================
 * Test 4b: A callback already in flight races deinit: deinit blocks until the
 * callback returns (quiescence barrier), then completes.
 * ========================================================================== */
static void test_event_racing_deinit_inflight( void )
{
  _reset_session();

  g_cb_entered  = false;
  g_cb_finished = false;
  g_deinit_done = false;
  g_race_events = 0;
  g_nested_done = false;
  g_cb_a_events = 0;
  g_cb_b_events = 0;
  TEST_ASSERT_EQUAL_MESSAGE( 0, sem_init( &g_cb_gate, 0, 0 ), "sem init" );

  wifi_hal_init_t init = _make_init( NULL, race_event_cb, NULL );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, wifi_hal_init( &init ), "init" );

  /* Producer: fires SCAN_DONE synchronously; the callback blocks mid-flight. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_producer_thread, NULL,
                                                producer_start_scan, NULL ),
                             "producer thread created" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_cb_entered( 2000 ),
                            "callback reached its blocking point" );

  /* Deinit in a separate thread: it must NOT return while the callback is
   * blocked inside the HAL event delivery. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, pthread_create( &g_deinit_thread, NULL,
                                                deinit_runner, NULL ),
                             "deinit thread created" );

  _sleep_ms( 100 );
  TEST_ASSERT_FALSE_MESSAGE( g_deinit_done,
                             "deinit waits for the in-flight callback" );

  /* Unblock the in-flight callback; deinit may now complete. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, sem_post( &g_cb_gate ), "sem post" );
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

/* -------------------------------------------------------------------------- */
/*  Runner                                                                    */
/* -------------------------------------------------------------------------- */

void setUp( void )
{
}

void tearDown( void )
{
}

void wifi_hal_posix_tests_run( void )
{
  RUN_TEST( test_deinit_before_init );
  RUN_TEST( test_init_deinit_deinit );
  RUN_TEST( test_partial_init_cleanup );
  RUN_TEST( test_event_racing_deinit_timer );
  RUN_TEST( test_event_racing_deinit_inflight );
  RUN_TEST( test_repeat_init_deinit_cycles );
  RUN_TEST( test_deinit_after_natural_timer_completion );
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