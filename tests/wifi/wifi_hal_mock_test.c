/*
 * Wi-Fi HAL Mock — AP DNS Configuration Unit Tests
 *
 * Exercises the optional AP DHCP DNS override exposed through wifi_hal_init_t
 * on the Wi-Fi HAL mock.  Covers:
 * 1.  Valid IPv4 DNS address is stored.
 * 2.  Omitted (NULL) DNS keeps non-captive DHCP default (not set).
 * 3.  Enabled provisioning ties the captive DNS to the AP IP.
 * 4.  Invalid IPv4 DNS address is rejected.
 * 5.  The platform-neutral IPv4 validator helper edge cases.
 *
 * Plus the TASK-134B lifecycle barrier cases: acknowledged per-invocation
 * hold/release for wifi_hal_init()/wifi_hal_deinit(), generation-based
 * entered/completed notifications for init/stop/deinit, counting release
 * semantics for multiple parked calls, and reset semantics (stale-notification
 * drain, no shared-state wipe while a call is held).
 */

#include <string.h>

#include "osal_bin_sem.h"
#include "osal_task.h"
#include "wifi_hal_mock.h"
#include "unity.h"

static void _dummy_event_cb( wifi_hal_event_t       event,
                             const wifi_hal_event_data_t* data,
                             void*                  user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
}

static wifi_hal_init_t _make_init( const char* ap_dns )
{
  wifi_hal_init_t init = {
    .ap_ip      = "192.168.1.1",
    .ap_gateway = "192.168.1.1",
    .ap_netmask = "255.255.255.0",
    .event_cb   = _dummy_event_cb,
    .user_data  = NULL,
  };
  init.ap_dns = ap_dns;
  return init;
}

/* ============================================================================
 * Test 1: Valid IPv4 DNS address is stored
 * ========================================================================== */
static void test_dns_valid_stored( void )
{
  wifi_hal_mock_reset();

  wifi_hal_init_t init = _make_init( "192.168.1.254" );
  osal_status_t   st   = wifi_hal_init( &init );

  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "valid ap_dns accepted" );
  const wifi_hal_mock_state_t* state = wifi_hal_mock_get_state();
  TEST_ASSERT_MESSAGE( state->ap_dns_set, "ap_dns marked as set" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.254", state->ap_dns, "stored DNS matches" );
}

/* ============================================================================
 * Test 2: Omitted (empty) DNS is valid for non-captive uses
 * ========================================================================== */
static void test_dns_omitted_non_captive( void )
{
  wifi_hal_mock_reset();

  /* NULL means no override — non-captive DHCP keeps the platform default. */
  wifi_hal_init_t init = _make_init( NULL );
  osal_status_t   st   = wifi_hal_init( &init );

  TEST_ASSERT_EQUAL( OSAL_SUCCESS, st );
  const wifi_hal_mock_state_t* state = wifi_hal_mock_get_state();
  TEST_ASSERT_MESSAGE( !state->ap_dns_set, "ap_dns not set when omitted" );
  TEST_ASSERT_MESSAGE( state->ap_dns[0] == '\0', "stored DNS is empty when omitted" );

  /* An explicitly empty string is also treated as omitted. */
  wifi_hal_mock_reset();
  init = _make_init( "" );
  st   = wifi_hal_init( &init );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, st );
  state = wifi_hal_mock_get_state();
  TEST_ASSERT_MESSAGE( !state->ap_dns_set, "ap_dns not set when empty string passed" );
}

/* ============================================================================
 * Test 2b: Enabled provisioning passes the AP IP as the captive DNS
 * ========================================================================== */
static void test_provisioning_passes_ap_ip_as_dns( void )
{
  wifi_hal_mock_reset();

  /* Captive provisioning advertises the soft-AP's own IP as the DNS server so
   * clients resolve every hostname back to the portal. */
  wifi_hal_init_t init = _make_init( "192.168.1.1" );
  init.ap_dns = init.ap_ip; /* provisioning ties the DNS to the AP IP */
  osal_status_t st = wifi_hal_init( &init );

  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "provisioning init with AP IP as DNS succeeds" );
  const wifi_hal_mock_state_t* state = wifi_hal_mock_get_state();
  TEST_ASSERT_MESSAGE( state->ap_dns_set, "provisioning DNS marked as set" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.1", state->ap_dns,
                                    "advertised DNS equals the AP IP" );
}

/* ============================================================================
 * Test 3: Invalid IPv4 DNS address is rejected
 * ========================================================================== */
static void test_dns_invalid_rejected( void )
{
  wifi_hal_mock_reset();

  /* A non-numeric string must be rejected. */
  wifi_hal_init_t init = _make_init( "not-an-ip" );
  osal_status_t   st   = wifi_hal_init( &init );
  TEST_ASSERT_MESSAGE( st == OSAL_ERR_INVALID_ARGUMENT, "non-numeric ap_dns rejected" );

  /* An octet > 255 must be rejected. */
  wifi_hal_mock_reset();
  init = _make_init( "300.1.1.1" );
  st   = wifi_hal_init( &init );
  TEST_ASSERT_MESSAGE( st == OSAL_ERR_INVALID_ARGUMENT, "octet >255 rejected" );

  /* Too many octets must be rejected. */
  wifi_hal_mock_reset();
  init = _make_init( "1.2.3.4.5" );
  st   = wifi_hal_init( &init );
  TEST_ASSERT_MESSAGE( st == OSAL_ERR_INVALID_ARGUMENT, "5 octets rejected" );

  /* A rejected init must not mark the HAL as configured. */
  const wifi_hal_mock_state_t* state = wifi_hal_mock_get_state();
  TEST_ASSERT_MESSAGE( !state->ap_dns_set, "ap_dns not stored on failure" );
}

/* ============================================================================
 * Test 4: IPv4 validator helper edge cases
 * ========================================================================== */
static void test_ipv4_validator( void )
{
  TEST_ASSERT_MESSAGE( wifi_hal_is_valid_ipv4( "192.168.1.1" ),  "valid private IP" );
  TEST_ASSERT_MESSAGE( wifi_hal_is_valid_ipv4( "8.8.8.8" ),      "valid public IP"  );
  TEST_ASSERT_MESSAGE( wifi_hal_is_valid_ipv4( "0.0.0.0" ),      "all-zero IP"      );
  TEST_ASSERT_MESSAGE( wifi_hal_is_valid_ipv4( "255.255.255.0" ),"subnet mask /255" );

  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( NULL ),                  "NULL invalid"      );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "" ),                    "empty invalid"     );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "256.0.0.1" ),           "octet >255 invalid" );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "1.2.3" ),               "missing octet"     );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "1.2.3.4.5" ),           "too many octets"   );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "192.168..1" ),          "empty octet"       );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "192.168.1.-1" ),        "negative octet"    );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( " 192.168.1.1" ),        "leading space"     );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "192.168.1.1 " ),        "trailing space"    );
  TEST_ASSERT_MESSAGE( !wifi_hal_is_valid_ipv4( "x.y.z.w" ),             "non-numeric"       );
}

/* ============================================================================
 * TASK-134B lifecycle barrier helpers
 *
 * A small helper task stands in for the Wi-Fi worker: it calls one HAL
 * lifecycle function and records the result.  The test thread drives the
 * barrier exclusively through the acknowledged entered/completed generation
 * notifications and the per-invocation hold/release controls, so no timing
 * sleeps are needed to place or release the worker.
 *
 * Completion is signaled with a binary semaphore (not a polled boolean), so
 * the result/done assertions are properly synchronized with the worker and do
 * not race or sleep.
 * ========================================================================== */

typedef struct
{
  osal_status_t     result;
  osal_bin_sem_id_t done_sem; /**< Posted exactly once after the HAL call returned. */
} hal_call_rec_t;

static void _deinit_call_task( void* arg )
{
  hal_call_rec_t* rec = (hal_call_rec_t*) arg;
  rec->result = wifi_hal_deinit();
  (void) osal_bin_sem_give( rec->done_sem );
}

static void _init_call_task( void* arg )
{
  hal_call_rec_t* rec  = (hal_call_rec_t*) arg;
  wifi_hal_init_t init = _make_init( NULL );
  init.user_data       = rec;
  rec->result          = wifi_hal_init( &init );
  (void) osal_bin_sem_give( rec->done_sem );
}

static void _stop_call_task( void* arg )
{
  hal_call_rec_t* rec = (hal_call_rec_t*) arg;
  rec->result = wifi_hal_stop();
  (void) osal_bin_sem_give( rec->done_sem );
}

static osal_task_id_t _spawn_hal_call( void (*routine)( void* ), hal_call_rec_t* rec )
{
  osal_task_id_t id = 0;
  osal_status_t  rc;

  rec->result   = (osal_status_t) -1;
  rec->done_sem = NULL;
  rc            = osal_bin_sem_create( &rec->done_sem, "mock_hal_done", OSAL_SEM_EMPTY );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS && rec->done_sem != NULL,
                       "create HAL call completion semaphore" );
  if ( rc != OSAL_SUCCESS || rec->done_sem == NULL )
  {
    return id;
  }

  rc = osal_task_create( &id, "mock_worker", routine, rec, NULL,
                         OSAL_TASK_MIN_STACK_SIZE * 4, 10, NULL );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS, "spawn HAL call worker task" );
  if ( rc != OSAL_SUCCESS )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
    id            = 0;
  }
  return id;
}

static bool _wait_rec_done( hal_call_rec_t* rec, uint32_t timeout_ms )
{
  return osal_bin_sem_timed_wait( rec->done_sem, timeout_ms ) == OSAL_SUCCESS;
}

static void _reap_task( osal_task_id_t id, hal_call_rec_t* rec )
{
  (void) osal_task_delete( id );
  if ( rec->done_sem != NULL )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
  }
}

/* ============================================================================
 * Test 5: Deinit barrier holds the worker until explicit release
 *
 * Places the worker inside wifi_hal_deinit() at the acknowledged barrier
 * (entered generation 1) and verifies the call does not complete until the
 * test releases exactly the held invocation.
 * ========================================================================== */
static void test_deinit_barrier_holds_until_release( void )
{
  wifi_hal_mock_reset();

  wifi_hal_mock_set_deinit_hold( true );

  hal_call_rec_t rec;
  osal_task_id_t task = _spawn_hal_call( _deinit_call_task, &rec );

  /* Worker entered deinit round 1 and committed to the barrier. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 1, 2000 ),
                            "worker entered deinit round 1" );

  /* The worker must NOT have returned while the barrier is held. */
  TEST_ASSERT_FALSE_MESSAGE( _wait_rec_done( &rec, 100 ),
                             "worker is parked, deinit has not returned" );

  /* Round 1 must NOT be reported completed while it is held. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 1, 200 ),
                             "deinit not completed while held" );

  /* The held call must not have touched the lifecycle state yet. */
  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_entered_gen, "entered gen round 1" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.deinit_completed_gen, "completed gen zero while held" );
  /* deinit_count counts attempts at entry, so the acknowledged (held) attempt
     already shows 1; the "not completed" signal is the completed generation. */
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_count, "held deinit attempt counted at entry" );

  /* Explicit release: exactly this call completes. */
  wifi_hal_mock_release_deinit_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 1, 2000 ),
                            "deinit round 1 completed after release" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec, 2000 ), "worker returned from deinit" );

  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_count, "deinit counter advanced" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_completed_gen, "completed gen advanced" );

  _reap_task( task, &rec );
}

/* ============================================================================
 * Test 6: Releasing one held deinit call leaves the next call held
 *
 * Round 1 is released and completes.  Round 2 is then parked by the same hold
 * state and must NOT be satisfied by round 1's completion notification: the
 * generation check proves a later lifecycle round is identified rather than a
 * leftover token.
 * ========================================================================== */
static void test_deinit_release_one_next_call_still_held( void )
{
  wifi_hal_mock_reset();

  wifi_hal_mock_set_deinit_hold( true );

  /* Round 1: park, release, complete. */
  hal_call_rec_t rec1;
  osal_task_id_t t1 = _spawn_hal_call( _deinit_call_task, &rec1 );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 1, 2000 ),
                            "round 1 entered" );
  wifi_hal_mock_release_deinit_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 1, 2000 ),
                            "round 1 completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec1, 2000 ), "round 1 worker returned" );

  /* Round 2: the hold state is still active, so the next deinit call is held
     again even though round 1 was already released. */
  hal_call_rec_t rec2;
  osal_task_id_t t2 = _spawn_hal_call( _deinit_call_task, &rec2 );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 2, 2000 ),
                            "round 2 entered (generation-checked, not leftover token)" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 2, 200 ),
                             "round 2 still held after round 1 release" );

  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  /* Both attempts have entered by now (round 1 completed, round 2 held), so the
     attempt-counting deinit counter is 2; the "only round 1 complete" signal is
     the completed generation still at 1. */
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_count, "two deinit attempts entered so far" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_completed_gen, "completed gen still round 1" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_entered_gen, "entered gen shows round 2" );

  /* Release round 2 explicitly. */
  wifi_hal_mock_release_deinit_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 2, 2000 ),
                            "round 2 completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec2, 2000 ), "round 2 worker returned" );

  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_count, "two deinit calls completed" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_completed_gen, "completed gen round 2" );

  _reap_task( t1, &rec1 );
  _reap_task( t2, &rec2 );
}

/* ============================================================================
 * Test 7: Disabling the hold releases every concurrently parked call
 *
 * Two deinit workers park on the barrier at the same time; set_hold(false)
 * must grant BOTH calls their own release (counting/acknowledged semantics —
 * a binary token would leave one worker parked forever).
 * ========================================================================== */
static void test_set_hold_false_releases_every_parked_call( void )
{
  wifi_hal_mock_reset();

  wifi_hal_mock_set_deinit_hold( true );

  hal_call_rec_t rec1;
  hal_call_rec_t rec2;
  osal_task_id_t t1 = _spawn_hal_call( _deinit_call_task, &rec1 );
  osal_task_id_t t2 = _spawn_hal_call( _deinit_call_task, &rec2 );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 1, 2000 ),
                            "round 1 entered" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 2, 2000 ),
                            "round 2 entered" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 2, 200 ),
                             "round 2 not completed while held" );

  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_entered_gen, "both rounds entered" );
  /* Both held attempts have entered, so the attempt-counting deinit counter is
     already 2; they have not completed (completed generation is still 0). */
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_count, "both deinit attempts entered while held" );

  /* Disabling the hold releases every parked call. */
  wifi_hal_mock_set_deinit_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 2, 2000 ),
                            "both held calls complete after hold release" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec1, 2000 ), "worker 1 returned" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec2, 2000 ), "worker 2 returned" );

  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_count, "both deinit calls completed" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.deinit_completed_gen, "completed gen round 2" );

  _reap_task( t1, &rec1 );
  _reap_task( t2, &rec2 );
}

/* ============================================================================
 * Test 8: Init and stop generation notifications (with init hold/release)
 *
 * Covers the entered/completed generation notifications for wifi_hal_init()
 * and wifi_hal_stop(), and places/releases the worker at the init barrier for
 * a second lifecycle round.
 * ========================================================================== */
static void test_init_and_stop_generation_rounds( void )
{
  wifi_hal_mock_reset();

  /* Round 1: init then stop, both through the worker tasks. */
  hal_call_rec_t irec;
  osal_task_id_t itask = _spawn_hal_call( _init_call_task, &irec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered_gen( 1, 2000 ),
                            "init round 1 entered" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 1, 2000 ),
                            "init round 1 completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &irec, 2000 ), "init worker 1 returned" );

  hal_call_rec_t srec;
  osal_task_id_t stask = _spawn_hal_call( _stop_call_task, &srec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_stop_entered_gen( 1, 2000 ),
                            "stop round 1 entered" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_stop_completed_gen( 1, 2000 ),
                            "stop round 1 completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &srec, 2000 ), "stop worker returned" );

  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.init_count, "init counter round 1" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.stop_count, "stop counter round 1" );
  TEST_ASSERT_TRUE_MESSAGE( snap.initialized, "initialized after init round 1" );
  TEST_ASSERT_FALSE_MESSAGE( snap.started, "not started after stop" );

  _reap_task( itask, &irec );
  _reap_task( stask, &srec );

  /* Round 2 init: place the worker at the init barrier and release it. */
  wifi_hal_mock_set_init_hold( true );
  hal_call_rec_t irec2;
  osal_task_id_t itask2 = _spawn_hal_call( _init_call_task, &irec2 );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered_gen( 2, 2000 ),
                            "init round 2 entered" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 2, 200 ),
                             "init round 2 held at barrier" );

  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  /* init_count counts attempts at entry, so round 2 already shows 2; the
     "not completed" signal is the completed generation still at round 1. */
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.init_count, "init round 2 entered (attempt counted)" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.init_completed_gen, "init round 2 not completed yet" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.init_entered_gen, "init entered gen round 2" );

  wifi_hal_mock_release_init_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 2, 2000 ),
                            "init round 2 completed after release" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &irec2, 2000 ), "init worker 2 returned" );

  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) snap.init_completed_gen, "init completed gen round 2" );

  _reap_task( itask2, &irec2 );
}

/* ============================================================================
 * Test 9: Reset drains stale notifications
 *
 * After a completed lifecycle round, reset must zero the generation counters
 * and drain every notification token, so no wait for a later round can be
 * satisfied by a leftover token.
 * ========================================================================== */
static void test_reset_drains_stale_notifications( void )
{
  wifi_hal_mock_reset();

  /* Produce one full init round so entered + completed tokens exist. */
  hal_call_rec_t rec;
  osal_task_id_t task = _spawn_hal_call( _init_call_task, &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered_gen( 1, 2000 ),
                            "init entered" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 1, 2000 ),
                            "init completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec, 2000 ), "init worker returned" );
  _reap_task( task, &rec );

  /* The TASK-134 compatibility wait is a one-shot/token-consuming API.  A
   * zero-timeout drain consumes the entered acknowledgement once, then a
   * second drain observes no token; generation waits below remain the API for
   * identifying a specific lifecycle round. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 0 ),
                            "consume init entered notification" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_entered( 0 ),
                             "init notification is consumed exactly once" );

  /* Reset drains all notification tokens and zeroes the generations. */
  wifi_hal_mock_reset();

  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.init_entered_gen, "init entered gen reset" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.init_completed_gen, "init completed gen reset" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.init_count, "init counter reset" );
  TEST_ASSERT_FALSE_MESSAGE( snap.initialized, "initialized flag reset" );

  /* No stale token can satisfy a fresh wait for round 1. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_entered_gen( 1, 200 ),
                             "no stale init entered token after reset" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 1, 200 ),
                             "no stale init completed token after reset" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 1, 200 ),
                             "no stale deinit entered token after reset" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 1, 200 ),
                             "no stale deinit completed token after reset" );
}

/* ============================================================================
 * Test 10: Reset does not clear shared state while a call is held
 *
 * Resetting the mock while a deinit call is parked on the barrier must keep
 * the shared mock state (the worker is still inside the HAL function) and the
 * barrier release grant intact, so the held call can still be released and
 * complete afterwards.
 * ========================================================================== */
static void test_reset_preserves_state_while_held( void )
{
  wifi_hal_mock_reset();

  /* Establish initialized state first. */
  hal_call_rec_t irec;
  osal_task_id_t itask = _spawn_hal_call( _init_call_task, &irec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 1, 2000 ),
                            "init completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &irec, 2000 ), "init worker returned" );
  _reap_task( itask, &irec );

  /* Park a deinit call on the barrier.  Keep a non-default result configured
   * so reset also proves it does not rewrite lifecycle controls mid-call. */
  wifi_hal_mock_set_deinit_result( OSAL_ERROR );
  wifi_hal_mock_set_deinit_hold( true );
  hal_call_rec_t drec;
  osal_task_id_t dtask = _spawn_hal_call( _deinit_call_task, &drec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_gen( 1, 2000 ),
                            "deinit entered" );

  /* Reset while the deinit call is held: shared state must survive. */
  wifi_hal_mock_reset();

  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_TRUE_MESSAGE( snap.initialized, "initialized not cleared while deinit held" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.init_count, "init counter not cleared while deinit held" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_entered_gen,
                             "held deinit generation survives reset" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.deinit_completed_gen,
                             "held deinit remains incomplete after reset" );

  /* The held call is still parked; releasing it lets it complete. */
  wifi_hal_mock_release_deinit_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 1, 2000 ),
                            "held deinit completes after reset + release" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &drec, 2000 ), "deinit worker returned" );
  TEST_ASSERT_EQUAL_MESSAGE( (int) OSAL_ERROR, (int) drec.result,
                             "deinit result survives reset while held" );

  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_FALSE_MESSAGE( snap.initialized, "deinit cleared initialized after release" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) snap.deinit_count, "deinit counter advanced" );

  _reap_task( dtask, &drec );

  /* A subsequent normal reset fully clears the mock. */
  wifi_hal_mock_reset();
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.init_count, "init counter reset after held round" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) snap.deinit_count, "deinit counter reset after held round" );
}

/* ============================================================================
 * Test 11: Synchronized lifecycle-result setters reach the worker
 *
 * The init/deinit result setters are read inside the HAL functions under the
 * mock lock; the worker must observe the configured value and the snapshot
 * must reflect the outcome without racing the worker.
 * ========================================================================== */
static void test_lifecycle_result_setters( void )
{
  /* Deinit result is honored. */
  wifi_hal_mock_reset();
  wifi_hal_mock_set_deinit_result( OSAL_ERROR );

  hal_call_rec_t rec;
  osal_task_id_t task = _spawn_hal_call( _deinit_call_task, &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_gen( 1, 2000 ),
                            "deinit completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &rec, 2000 ), "deinit worker returned" );
  TEST_ASSERT_EQUAL_MESSAGE( (int) OSAL_ERROR, (int) rec.result,
                             "deinit returned configured result" );
  _reap_task( task, &rec );

  /* Init result is honored and the failure round is still notified. */
  wifi_hal_mock_reset();
  wifi_hal_mock_set_init_result( OSAL_ERROR );

  hal_call_rec_t irec;
  osal_task_id_t itask = _spawn_hal_call( _init_call_task, &irec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_gen( 1, 2000 ),
                            "init failure round completed" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_rec_done( &irec, 2000 ), "init worker returned" );
  TEST_ASSERT_EQUAL_MESSAGE( (int) OSAL_ERROR, (int) irec.result,
                             "init returned configured result" );

  wifi_hal_mock_lifecycle_snapshot_t snap = { 0 };
  wifi_hal_mock_get_lifecycle_snapshot( &snap );
  TEST_ASSERT_FALSE_MESSAGE( snap.initialized, "not initialized after init failure" );
  _reap_task( itask, &irec );
}

/* ============================================================================
 * Runner
 * ========================================================================== */
void wifi_hal_mock_tests_run( void )
{
  RUN_TEST( test_dns_valid_stored );
  RUN_TEST( test_dns_omitted_non_captive );
  RUN_TEST( test_provisioning_passes_ap_ip_as_dns );
  RUN_TEST( test_dns_invalid_rejected );
  RUN_TEST( test_ipv4_validator );
  RUN_TEST( test_deinit_barrier_holds_until_release );
  RUN_TEST( test_deinit_release_one_next_call_still_held );
  RUN_TEST( test_set_hold_false_releases_every_parked_call );
  RUN_TEST( test_init_and_stop_generation_rounds );
  RUN_TEST( test_reset_drains_stale_notifications );
  RUN_TEST( test_reset_preserves_state_while_held );
  RUN_TEST( test_lifecycle_result_setters );
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  wifi_hal_mock_tests_run();

#ifndef ESP_PLATFORM
  return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */