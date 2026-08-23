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

/* Copy a full synchronized state snapshot for assertions.  The snapshot API
 * (TASK-139) never returns the mutable global address: it copies the complete
 * state under the mock mutex, so a reader never holds a pointer into g_mock
 * while a worker writes it. */
static void _snap_mock( wifi_hal_mock_state_t* out )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( out ),
                            "mock state snapshot readable" );
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
  wifi_hal_mock_state_t state = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &state ), "state snapshot readable" );
  TEST_ASSERT_MESSAGE( state.ap_dns_set, "ap_dns marked as set" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.254", state.ap_dns, "stored DNS matches" );
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
  wifi_hal_mock_state_t state = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &state ), "state snapshot readable" );
  TEST_ASSERT_MESSAGE( !state.ap_dns_set, "ap_dns not set when omitted" );
  TEST_ASSERT_MESSAGE( state.ap_dns[0] == '\0', "stored DNS is empty when omitted" );

  /* An explicitly empty string is also treated as omitted. */
  wifi_hal_mock_reset();
  init = _make_init( "" );
  st   = wifi_hal_init( &init );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, st );
  memset( &state, 0, sizeof( state ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &state ), "state snapshot readable" );
  TEST_ASSERT_MESSAGE( !state.ap_dns_set, "ap_dns not set when empty string passed" );
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
  wifi_hal_mock_state_t state = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &state ), "state snapshot readable" );
  TEST_ASSERT_MESSAGE( state.ap_dns_set, "provisioning DNS marked as set" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.1", state.ap_dns,
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
  wifi_hal_mock_state_t state = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &state ), "state snapshot readable" );
  TEST_ASSERT_MESSAGE( !state.ap_dns_set, "ap_dns not stored on failure" );
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
 * TASK-134B focused reset/init cases
 *
 * Completion is observed with a semaphore.  The tests never infer entry or
 * completion from a delay or from an unsynchronized flag.
 * ========================================================================== */

typedef struct
{
  osal_status_t     result;
  osal_bin_sem_id_t done_sem;
} mock_init_call_t;

static void _init_call_task( void* arg )
{
  mock_init_call_t* rec = (mock_init_call_t*) arg;
  wifi_hal_init_t   init = _make_init( NULL );

  rec->result = wifi_hal_init( &init );
  (void) osal_bin_sem_give( rec->done_sem );
}

static osal_task_id_t _spawn_init( mock_init_call_t* rec )
{
  osal_task_id_t id = 0;
  osal_status_t rc;

  rec->result   = (osal_status_t) -1;
  rec->done_sem = NULL;
  rc = osal_bin_sem_create( &rec->done_sem, "mock_init_done", OSAL_SEM_EMPTY );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS && rec->done_sem != NULL,
                       "create init completion semaphore" );
  if ( rc != OSAL_SUCCESS || rec->done_sem == NULL )
  {
    return id;
  }

  rc = osal_task_create( &id, "mock_init_worker", _init_call_task, rec, NULL,
                         OSAL_TASK_MIN_STACK_SIZE * 4, 10, NULL );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS, "spawn init worker task" );
  if ( rc != OSAL_SUCCESS )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
    id            = 0;
  }
  return id;
}

static bool _init_done( mock_init_call_t* rec, uint32_t timeout_ms )
{
  return osal_bin_sem_timed_wait( rec->done_sem, timeout_ms ) == OSAL_SUCCESS;
}

static void _reap_init( osal_task_id_t id, mock_init_call_t* rec )
{
  (void) osal_task_delete( id );
  if ( rec->done_sem != NULL )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
  }
}
typedef struct
{
  osal_status_t     result;
  osal_bin_sem_id_t done_sem;
} mock_deinit_call_t;

static void _deinit_call_task( void* arg )
{
  mock_deinit_call_t* rec = (mock_deinit_call_t*) arg;

  rec->result = wifi_hal_deinit();
  (void) osal_bin_sem_give( rec->done_sem );
}

static osal_task_id_t _spawn_deinit( mock_deinit_call_t* rec )
{
  osal_task_id_t id = 0;
  osal_status_t rc;

  rec->result   = (osal_status_t) -1;
  rec->done_sem = NULL;
  rc = osal_bin_sem_create( &rec->done_sem, "mock_deinit_done", OSAL_SEM_EMPTY );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS && rec->done_sem != NULL,
                       "create deinit completion semaphore" );
  if ( rc != OSAL_SUCCESS || rec->done_sem == NULL )
  {
    return id;
  }

  rc = osal_task_create( &id, "mock_deinit_worker", _deinit_call_task, rec, NULL,
                         OSAL_TASK_MIN_STACK_SIZE * 4, 10, NULL );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS, "spawn deinit worker task" );
  if ( rc != OSAL_SUCCESS )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
    id            = 0;
  }
  return id;
}

static void _reap_deinit( osal_task_id_t id, mock_deinit_call_t* rec )
{
  (void) osal_task_delete( id );
  if ( rec->done_sem != NULL )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
  }
}

static void test_held_init_acknowledge_and_release( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );
  /* Regression: disabling an idle hold must not leave a release token that
   * can release a later held invocation. */
  wifi_hal_mock_set_init_hold( false );
  wifi_hal_mock_set_init_hold( true );

  mock_init_call_t rec;
  osal_task_id_t task = _spawn_init( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 2000 ),
                            "held init entered" );
  TEST_ASSERT_FALSE_MESSAGE( _init_done( &rec, 100 ),
                             "held init has not returned" );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_FALSE_MESSAGE( st.initialized,
                             "callback not installed while held" );

  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &rec, 2000 ),
                            "init returned after release" );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, rec.result );
  memset( &st, 0, sizeof( st ) );
  _snap_mock( &st );
  TEST_ASSERT_TRUE_MESSAGE( st.initialized,
                            "HAL initialized after release" );
  _reap_init( task, &rec );

  /* A release token from this round must not release the next held init. */
  wifi_hal_mock_set_init_hold( true );
  mock_init_call_t next_rec;
  osal_task_id_t next_task = _spawn_init( &next_rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 2000 ),
                            "next held init entered" );
  TEST_ASSERT_FALSE_MESSAGE( _init_done( &next_rec, 100 ),
                             "next held init needs its own release" );
  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &next_rec, 2000 ),
                            "next init returned after its release" );
  _reap_init( next_task, &next_rec );
}

static void test_wait_init_entered_is_token_consuming( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  mock_init_call_t rec;
  osal_task_id_t task = _spawn_init( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 2000 ),
                            "init entered" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_entered( 0 ),
                             "second wait has no token" );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &rec, 2000 ), "init returned" );
  _reap_init( task, &rec );
}

static void test_reset_drains_old_tokens_only( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  mock_init_call_t old_rec;
  osal_task_id_t old_task = _spawn_init( &old_rec );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &old_rec, 2000 ), "old init returned" );
  _reap_init( old_task, &old_rec );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "quiescent reset succeeds" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_entered( 0 ),
                             "old entered token was drained" );

  mock_init_call_t fresh_rec;
  osal_task_id_t fresh_task = _spawn_init( &fresh_rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 2000 ),
                            "fresh post-reset token remains available" );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &fresh_rec, 2000 ), "fresh init returned" );
  _reap_init( fresh_task, &fresh_rec );
}

static void test_reset_rejected_while_init_held( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );
  wifi_hal_mock_set_init_hold( true );

  mock_init_call_t rec;
  osal_task_id_t task = _spawn_init( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 2000 ),
                            "held init entered" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_reset(),
                             "reset rejected while init is active" );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_EQUAL( 1, (int) st.init_count );
  TEST_ASSERT_FALSE_MESSAGE( st.initialized,
                             "active round was not reset" );

  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &rec, 2000 ),
                            "held init released after rejected reset" );
  _reap_init( task, &rec );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(),
                            "reset succeeds once init is quiescent" );
  memset( &st, 0, sizeof( st ) );
  _snap_mock( &st );
  TEST_ASSERT_EQUAL( 0, (int) st.init_count );
}
/* ============================================================================
 * TASK-134B1 focused cases
 *
 * Single-worker lifecycle generations and per-round barriers.
 * ========================================================================== */

static void test_init_release_rounds_and_stale_hints( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  /* Hold the barrier for rounds 1 and 2. */
  wifi_hal_mock_set_init_hold( true );

  /* Round 1 enters and parks. */
  mock_init_call_t r1;
  osal_task_id_t t1 = _spawn_init( &r1 );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered_level( 1, 2000 ),
                            "round 1 init entered the gate" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 1, 100 ),
                             "round 1 init not completed while parked" );

  /* Release round 1 with release_init_hold(); the hold stays enabled. */
  wifi_hal_mock_release_init_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 1, 2000 ),
                            "round 1 init completed after release" );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, r1.result );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_TRUE_MESSAGE( st.initialized,
                            "round 1 effects applied" );
  _reap_init( t1, &r1 );

  /* A stale round-1 token may still be pending.  A round-2 wait must consult
   * the counter under the mutex, so a leftover round-1 hint cannot satisfy
   * a wait that identifies round 2. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_entered_level( 2, 200 ),
                             "stale round-1 entered hint cannot satisfy round 2" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 2, 0 ),
                             "stale round-1 completed hint cannot satisfy round 2" );

  /* Round 2 enters and stays parked while the hold is still enabled. */
  mock_init_call_t r2;
  osal_task_id_t tid2 = _spawn_init( &r2 );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered_level( 2, 2000 ),
                            "round 2 init entered while hold remains enabled" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 2, 100 ),
                             "round 2 init remains parked" );
  wifi_hal_mock_release_init_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 2, 2000 ),
                            "round 2 init completed after its release" );
  _reap_init( tid2, &r2 );

  TEST_ASSERT_EQUAL( 2, (int) wifi_hal_mock_get_init_entered_count() );
  TEST_ASSERT_EQUAL( 2, (int) wifi_hal_mock_get_init_completed_count() );

  wifi_hal_mock_set_init_hold( false );
}

static void test_init_failure_still_counts_generation( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  /* A configured failure still counts as an accepted invocation attempt at the
   * same entry boundary, and its mock effects are not applied. */
  wifi_hal_mock_set_init_result( OSAL_ERROR );
  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_ERROR, wifi_hal_init( &init ) );

  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_init_entered_count() );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_init_completed_count() );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_FALSE_MESSAGE( st.initialized,
                             "failed init never initializes the HAL" );

  wifi_hal_mock_reset();
  wifi_hal_mock_set_init_hold( true );
  mock_init_call_t rec;
  osal_task_id_t task = _spawn_init( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered_level( 1, 2000 ),
                            "held init entered after reset" );

  /* A held (not just active) init blocks reset: generations and tokens from
   * the running round must not be cleared or consumed. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_reset(),
                             "reset rejected while init is parked" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_init_entered_count() );
  TEST_ASSERT_EQUAL( 0, (int) wifi_hal_mock_get_init_completed_count() );
  memset( &st, 0, sizeof( st ) );
  _snap_mock( &st );
  TEST_ASSERT_FALSE_MESSAGE( st.initialized,
                             "active round not modified by rejected reset" );

  wifi_hal_mock_release_init_hold();
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &rec, 2000 ),
                            "held init released after rejected reset" );
  _reap_init( task, &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "reset succeeds once quiescent" );
}

static void test_stop_and_deinit_generation_waits( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 1, 2000 ),
                            "init completed generation 1" );

  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_stop() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_stop_entered_level( 1, 2000 ),
                            "stop entered generation 1" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_stop_completed_level( 1, 2000 ),
                            "stop completed generation 1" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_stop_entered_count() );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_stop_completed_count() );

  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_deinit() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_level( 1, 2000 ),
                            "deinit entered generation 1" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_level( 1, 2000 ),
                            "deinit completed generation 1" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_deinit_entered_count() );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_deinit_completed_count() );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_FALSE_MESSAGE( st.initialized,
                             "deinit cleared the HAL" );
}

static void test_deinit_hold_blocks_reset_until_release( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_completed_level( 1, 2000 ),
                            "init completed before deinit hold" );

  wifi_hal_mock_set_deinit_hold( true );
  mock_deinit_call_t rec;
  osal_task_id_t task = _spawn_deinit( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_entered_level( 1, 2000 ),
                            "held deinit entered and parked" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_deinit_completed_level( 1, 100 ),
                              "deinit not completed while parked" );

  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_reset(),
                             "reset rejected while deinit is active" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_deinit_entered_count() );
  TEST_ASSERT_EQUAL( 0, (int) wifi_hal_mock_get_deinit_completed_count() );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_TRUE_MESSAGE( st.initialized,
                            "rejected reset did not modify the live round" );

  wifi_hal_mock_release_deinit_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_deinit_completed_level( 1, 2000 ),
                            "deinit completed after release" );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, rec.result );
  memset( &st, 0, sizeof( st ) );
  _snap_mock( &st );
  TEST_ASSERT_FALSE_MESSAGE( st.initialized,
                             "deinit effects applied" );
  _reap_deinit( task, &rec );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "reset succeeds once quiescent" );
}


/* ============================================================================
 * TASK-134B2 focused lifecycle snapshot + configured-result cases
 *
 * The mock now exposes a race-free, synchronized lifecycle snapshot
 * (wifi_hal_mock_get_lifecycle) plus mutex-protected init/stop/deinit result
 * setters.  Every configured failure is still a completed attempt so TASK-135
 * can build retry flows without racing the worker.
 * ========================================================================== */

static void test_lifecycle_snapshot_fields_and_zeroing( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  /* Snapshot before any init with a garbage-filled destination: the entire
   * destination is zeroed before the locked copy, so a never-written field
   * (stop_count) reads zero rather than the stale 0xAA fill. */
  wifi_hal_mock_lifecycle_t snap;
  memset( &snap, 0xAA, sizeof( snap ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "empty snapshot copied" );
  TEST_ASSERT_FALSE( snap.initialized );
  TEST_ASSERT_FALSE( snap.started );
  TEST_ASSERT_FALSE( snap.connected );
  TEST_ASSERT_FALSE( snap.event_cb_registered );
  TEST_ASSERT_EQUAL( 0, (int) snap.stop_count );
  TEST_ASSERT_EQUAL( 0, (int) snap.init_entered_gen );
  TEST_ASSERT_EQUAL( 0, (int) snap.stop_completed_gen );

  /* Init, start, and connect all succeed; the snapshot reflects them. */
  wifi_hal_init_t init = _make_init( NULL );
  init.user_data       = (void*) 0x1234; /* non-NULL so registration is visible */
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_connect() );

  /* A non-empty destination is zeroed before copy too, so fields (deinit_count,
   * stop_count) that init/start/connect never incremented still read zero. */
  memset( &snap, 0xAA, sizeof( snap ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "populated snapshot copied" );
  TEST_ASSERT_TRUE( snap.initialized );
  TEST_ASSERT_TRUE( snap.started );
  TEST_ASSERT_TRUE( snap.connected );
  TEST_ASSERT_TRUE( snap.event_cb_registered );
  TEST_ASSERT_TRUE( snap.user_data_registered );
  TEST_ASSERT_EQUAL( 1, (int) snap.init_count );
  TEST_ASSERT_EQUAL( 1, (int) snap.start_count );
  TEST_ASSERT_EQUAL( 1, (int) snap.init_entered_gen );
  TEST_ASSERT_EQUAL( 1, (int) snap.init_completed_gen );
  TEST_ASSERT_EQUAL( 0, (int) snap.stop_count );
  TEST_ASSERT_EQUAL( 0, (int) snap.deinit_count );
}

static void test_init_failure_is_visible_in_snapshot( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_mock_set_init_result( OSAL_ERROR );
  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_ERROR, wifi_hal_init( &init ) );

  wifi_hal_mock_lifecycle_t snap;
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "snapshot copied" );
  TEST_ASSERT_FALSE_MESSAGE( snap.initialized,
                             "failed init left initialization unchanged" );
  TEST_ASSERT_FALSE_MESSAGE( snap.event_cb_registered,
                             "failed init did not register a callback" );
  TEST_ASSERT_FALSE_MESSAGE( snap.user_data_registered,
                             "failed init did not register user data" );
  TEST_ASSERT_EQUAL( 1, (int) snap.init_entered_gen );
  TEST_ASSERT_EQUAL( 1, (int) snap.init_completed_gen );

  wifi_hal_mock_reset();
}

static void test_stop_failure_and_success_in_snapshot( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_connect() );

  /* A configured stop failure is a completed attempt but changes no state. */
  wifi_hal_mock_set_stop_result( OSAL_ERROR );
  TEST_ASSERT_EQUAL( OSAL_ERROR, wifi_hal_stop() );

  wifi_hal_mock_lifecycle_t snap;
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "snapshot copied" );
  TEST_ASSERT_TRUE_MESSAGE( snap.started,  "failed stop leaves started unchanged" );
  TEST_ASSERT_TRUE_MESSAGE( snap.connected, "failed stop leaves connected unchanged" );
  TEST_ASSERT_EQUAL( 1, (int) snap.stop_entered_gen );
  TEST_ASSERT_EQUAL( 1, (int) snap.stop_completed_gen );

  /* A retry that succeeds clears the same fields and is the next attempt. */
  wifi_hal_mock_set_stop_result( OSAL_SUCCESS );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_stop() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "snapshot copied after retry" );
  TEST_ASSERT_FALSE_MESSAGE( snap.started,  "successful stop clears started" );
  TEST_ASSERT_FALSE_MESSAGE( snap.connected, "successful stop clears connected" );
  TEST_ASSERT_EQUAL( 2, (int) snap.stop_entered_gen );
  TEST_ASSERT_EQUAL( 2, (int) snap.stop_completed_gen );

  wifi_hal_mock_reset();
}

static void test_deinit_failure_and_success_in_snapshot( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_init_t init = _make_init( NULL );
  init.user_data       = (void*) 0xABCD;
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );

  /* Failed deinit is a completed attempt that changes no lifecycle state. */
  wifi_hal_mock_set_deinit_result( OSAL_ERROR );
  TEST_ASSERT_EQUAL( OSAL_ERROR, wifi_hal_deinit() );

  wifi_hal_mock_lifecycle_t snap;
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "snapshot copied" );
  TEST_ASSERT_TRUE_MESSAGE( snap.initialized,
                            "failed deinit leaves initialized unchanged" );
  TEST_ASSERT_TRUE_MESSAGE( snap.event_cb_registered,
                            "failed deinit leaves callback registration unchanged" );
  TEST_ASSERT_TRUE_MESSAGE( snap.user_data_registered,
                            "failed deinit leaves user data registration unchanged" );
  TEST_ASSERT_EQUAL( 1, (int) snap.deinit_entered_gen );
  TEST_ASSERT_EQUAL( 1, (int) snap.deinit_completed_gen );

  /* A retry with success clears all lifecycle + registration state. */
  wifi_hal_mock_set_deinit_result( OSAL_SUCCESS );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_deinit() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &snap ),
                            "snapshot copied after retry" );
  TEST_ASSERT_FALSE_MESSAGE( snap.initialized, "successful deinit clears initialized" );
  TEST_ASSERT_FALSE_MESSAGE( snap.event_cb_registered,
                             "successful deinit clears callback registration" );
  TEST_ASSERT_FALSE_MESSAGE( snap.user_data_registered,
                             "successful deinit clears user data registration" );
  TEST_ASSERT_EQUAL( 2, (int) snap.deinit_entered_gen );
  TEST_ASSERT_EQUAL( 2, (int) snap.deinit_completed_gen );

  wifi_hal_mock_reset();
}

/* Re-entrant callback invoked from event delivery.  It re-enters the mock
 * control surface via the snapshot helper; if the mock lock were still held
 * during delivery (the mutex is non-recursive) this would block forever, so a
 * completing call proves the lock is released before invocation. */
static bool               s_reentrant_callback_result     = false;
static wifi_hal_mock_lifecycle_t s_reentrant_snapshot    = { 0 };
static bool               s_reentrant_state_result        = false;
static wifi_hal_mock_state_t    s_reentrant_state         = { 0 };

static void _reentrant_event_cb( wifi_hal_event_t       event,
                                 const wifi_hal_event_data_t* data,
                                 void*                  user_data )
{
  (void) event;
  (void) data;
  (void) user_data;
  /* Re-enter the full snapshot-copy API from inside the callback.  If the mock
   * mutex were still held during delivery (the mutex is non-recursive) this
   * would block forever, so a completing call proves the lock is released
   * before invocation. */
  s_reentrant_state_result    = wifi_hal_mock_get_state( &s_reentrant_state );
  s_reentrant_callback_result = wifi_hal_mock_get_lifecycle( &s_reentrant_snapshot );
}

static void test_event_delivery_holds_no_lock_during_callback( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_init_t init = _make_init( NULL );
  init.event_cb        = _reentrant_event_cb;
  init.user_data       = (void*) 0x7777;
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );

  /* inject_event() path. */
  s_reentrant_callback_result = false;
  s_reentrant_state_result    = false;
  memset( &s_reentrant_snapshot, 0, sizeof( s_reentrant_snapshot ) );
  memset( &s_reentrant_state, 0, sizeof( s_reentrant_state ) );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_SCAN_DONE, NULL );
  TEST_ASSERT_TRUE_MESSAGE( s_reentrant_callback_result,
                            "callback re-entered snapshot under no mock lock" );
  TEST_ASSERT_TRUE_MESSAGE( s_reentrant_state_result,
                            "callback re-entered full state snapshot under no lock" );
  TEST_ASSERT_TRUE( s_reentrant_snapshot.initialized );
  TEST_ASSERT_TRUE( s_reentrant_snapshot.event_cb_registered );
  TEST_ASSERT_TRUE( s_reentrant_snapshot.user_data_registered );
  TEST_ASSERT_TRUE( s_reentrant_state.initialized );
  TEST_ASSERT_NOT_NULL( (void*) s_reentrant_state.event_cb );

  /* start_scan() delivery path. */
  s_reentrant_callback_result = false;
  s_reentrant_state_result    = false;
  memset( &s_reentrant_snapshot, 0, sizeof( s_reentrant_snapshot ) );
  memset( &s_reentrant_state, 0, sizeof( s_reentrant_state ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start_scan( false ) );
  TEST_ASSERT_TRUE_MESSAGE( s_reentrant_callback_result,
                            "start_scan re-entered snapshot under no mock lock" );
  TEST_ASSERT_TRUE_MESSAGE( s_reentrant_state_result,
                            "start_scan re-entered full state snapshot under no lock" );

  wifi_hal_mock_reset();
}
/* ============================================================================
 * TASK-139 focused cases
 *
 * Full synchronized state snapshot (no caller keeps an address into g_mock),
 * mutex-protected result/configuration setters, and the connection /
 * mode-start / GOT_IP / SCAN_DONE notification channels with acknowledged
 * barrier holds.  The proof-of-work is that stale event tokens can never
 * satisfy a later requested generation and reset drains every new channel.
 * ========================================================================== */

typedef struct
{
  osal_status_t     result;
  osal_bin_sem_id_t done_sem;
} mock_connect_call_t;

static void _connect_call_task( void* arg )
{
  mock_connect_call_t* rec = (mock_connect_call_t*) arg;
  rec->result = wifi_hal_connect();
  (void) osal_bin_sem_give( rec->done_sem );
}

static osal_task_id_t _spawn_connect( mock_connect_call_t* rec )
{
  osal_task_id_t id = 0;
  osal_status_t rc;

  rec->result   = (osal_status_t) -1;
  rec->done_sem = NULL;
  rc = osal_bin_sem_create( &rec->done_sem, "mock_connect_done", OSAL_SEM_EMPTY );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS && rec->done_sem != NULL,
                       "create connect completion semaphore" );
  if ( rc != OSAL_SUCCESS || rec->done_sem == NULL )
  {
    return id;
  }

  rc = osal_task_create( &id, "mock_connect_worker", _connect_call_task, rec, NULL,
                         OSAL_TASK_MIN_STACK_SIZE * 4, 10, NULL );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS, "spawn connect worker task" );
  if ( rc != OSAL_SUCCESS )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
    id            = 0;
  }
  return id;
}

static void _reap_connect( osal_task_id_t id, mock_connect_call_t* rec )
{
  (void) osal_task_delete( id );
  if ( rec->done_sem != NULL )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
  }
}

typedef struct
{
  osal_status_t     result;
  wifi_hal_mode_t   mode;
  osal_bin_sem_id_t done_sem;
} mock_start_call_t;

static void _start_call_task( void* arg )
{
  mock_start_call_t* rec = (mock_start_call_t*) arg;
  rec->result = wifi_hal_start( rec->mode );
  (void) osal_bin_sem_give( rec->done_sem );
}

static osal_task_id_t _spawn_start( mock_start_call_t* rec )
{
  osal_task_id_t id = 0;
  osal_status_t rc;

  rec->result   = (osal_status_t) -1;
  rec->done_sem = NULL;
  rc = osal_bin_sem_create( &rec->done_sem, "mock_start_done", OSAL_SEM_EMPTY );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS && rec->done_sem != NULL,
                       "create start completion semaphore" );
  if ( rc != OSAL_SUCCESS || rec->done_sem == NULL )
  {
    return id;
  }

  rc = osal_task_create( &id, "mock_start_worker", _start_call_task, rec, NULL,
                         OSAL_TASK_MIN_STACK_SIZE * 4, 10, NULL );
  TEST_ASSERT_MESSAGE( rc == OSAL_SUCCESS, "spawn start worker task" );
  if ( rc != OSAL_SUCCESS )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
    id            = 0;
  }
  return id;
}

static void _reap_start( osal_task_id_t id, mock_start_call_t* rec )
{
  (void) osal_task_delete( id );
  if ( rec->done_sem != NULL )
  {
    (void) osal_bin_sem_delete( rec->done_sem );
    rec->done_sem = NULL;
  }
}

static void test_state_snapshot_copy_api( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_mock_state_t st = { 0 };
  memset( &st, 0xAA, sizeof( st ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &st ), "empty snapshot copied" );
  TEST_ASSERT_FALSE( st.initialized );
  TEST_ASSERT_FALSE( st.started );
  TEST_ASSERT_FALSE( st.connected );
  TEST_ASSERT_EQUAL( 0, (int) st.connect_count );
  TEST_ASSERT_EQUAL( 0, (int) st.scan_count );

  wifi_hal_init_t init = _make_init( NULL );
  init.user_data       = (void*) 0x4321;
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ) );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_connect() );

  wifi_hal_sta_config_t sta = { 0 };
  strncpy( sta.ssid, "SnapSSID", sizeof( sta.ssid ) - 1 );
  strncpy( sta.password, "SnapPass", sizeof( sta.password ) - 1 );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_set_sta_config( &sta ) );

  wifi_hal_ap_config_t ap = { 0 };
  strncpy( ap.ssid, "SnapAP", sizeof( ap.ssid ) - 1 );
  strncpy( ap.password, "SnapAPPass", sizeof( ap.password ) - 1 );
  ap.max_connection = 5;
  ap.authmode       = 3;
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_set_ap_config( &ap ) );

  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_set_power_save( true ) );

  wifi_hal_ip_info_t ip = { 0 };
  strncpy( ip.ip, "192.168.90.30", sizeof( ip.ip ) - 1 );
  wifi_hal_mock_set_ip_info( &ip );

  wifi_hal_ap_record_t recs[1] = { 0 };
  strncpy( recs[0].ssid, "NetX", sizeof( recs[0].ssid ) - 1 );
  recs[0].channel  = 7;
  recs[0].rssi     = -44;
  recs[0].authmode = 3;
  wifi_hal_mock_set_scan_list( recs, 1 );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start_scan( false ) );

  /* Re-copy into a garbage-filled destination: every field in the snapshot must
   * be overwritten by the locked copy, so no stale 0xAA fill can survive. */
  memset( &st, 0xAA, sizeof( st ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state( &st ), "populated snapshot copied" );
  TEST_ASSERT_TRUE( st.initialized );
  TEST_ASSERT_TRUE( st.started );
  TEST_ASSERT_TRUE( st.connected );
  TEST_ASSERT_TRUE( st.power_save );
  TEST_ASSERT_EQUAL( WIFI_HAL_MODE_STA, (int) st.mode );
  TEST_ASSERT_EQUAL_STRING( "SnapSSID", st.sta_cfg.ssid );
  TEST_ASSERT_EQUAL_STRING( "SnapPass", st.sta_cfg.password );
  TEST_ASSERT_EQUAL_STRING( "SnapAP", st.ap_cfg.ssid );
  TEST_ASSERT_EQUAL_STRING( "192.168.90.30", st.ip_info.ip );
  TEST_ASSERT_EQUAL( 1, (int) st.scan_count );
  TEST_ASSERT_EQUAL_STRING( "NetX", st.scan_list[0].ssid );
  TEST_ASSERT_EQUAL( 1, (int) st.init_count );
  TEST_ASSERT_EQUAL( 1, (int) st.start_count );
  TEST_ASSERT_EQUAL( 1, (int) st.connect_count );

  wifi_hal_mock_reset();
}

static void test_connect_channel_and_hold( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );

  /* Direct connect advances the connect-call and completed generations. */
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_connect() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_connect_call_level( 1, 2000 ),
                        "connect call generation 1" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_connect_completed_level( 1, 2000 ),
                        "connect completed generation 1" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_connect_call_count() );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_connect_completed_count() );

  /* A stale connect token cannot satisfy a later requested generation. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_connect_call_level( 2, 0 ),
                         "stale connect token cannot satisfy level 2" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_connect_completed_level( 2, 0 ),
                         "stale connect completion cannot satisfy level 2" );

  /* Return the station to a not-connected baseline so the held connect's
   * not-yet-applied state is observable. */
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_disconnect() );

  /* Held connect: call acknowledged at the barrier, result not yet applied. */
  wifi_hal_mock_set_connect_hold( true );
  mock_connect_call_t rec;
  osal_task_id_t task = _spawn_connect( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_connect_call_level( 2, 2000 ),
                        "held connect call entered" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_connect_completed_level( 2, 100 ),
                         "held connect not completed while parked" );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_FALSE( st.connected );

  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_reset(),
                         "reset rejected while a connect round is parked" );

  wifi_hal_mock_release_connect_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_connect_completed_level( 2, 2000 ),
                        "held connect completed after release" );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, rec.result );
  _reap_connect( task, &rec );

  memset( &st, 0, sizeof( st ) );
  _snap_mock( &st );
  TEST_ASSERT_TRUE_MESSAGE( st.connected, "connect applied after release" );

  wifi_hal_mock_set_connect_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "reset succeeds once quiescent" );
}

static void test_start_mode_channel_and_hold( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_start_entered_level( 1, 2000 ),
                        "mode-start entered generation 1" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_start_completed_level( 1, 2000 ),
                        "mode-start completed generation 1" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_start_entered_count() );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_start_completed_count() );

  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_start_entered_level( 2, 0 ),
                         "stale start token cannot satisfy level 2" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_start_completed_level( 2, 0 ),
                         "stale start completion cannot satisfy level 2" );

  /* Held mode-start: entry captures the mode, but the result effect is applied
   * only at completion.  Use a failing round so the completion effect
   * (started cleared) is distinguishable from the pre-round state. */
  wifi_hal_mock_set_start_result( OSAL_ERROR );
  wifi_hal_mock_set_start_hold( true );
  mock_start_call_t rec;
  rec.mode = WIFI_HAL_MODE_AP;
  osal_task_id_t task = _spawn_start( &rec );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_start_entered_level( 2, 2000 ),
                        "held mode-start entered" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_start_completed_level( 2, 100 ),
                         "held mode-start not completed while parked" );
  wifi_hal_mock_state_t st = { 0 };
  _snap_mock( &st );
  TEST_ASSERT_EQUAL_MESSAGE( WIFI_HAL_MODE_AP, (int) st.mode,
                         "mode captured at held start entry" );
  TEST_ASSERT_TRUE_MESSAGE( st.started,
                            "pre-round started state untouched while held" );

  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_reset(),
                         "reset rejected while mode-start round is parked" );

  wifi_hal_mock_release_start_hold();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_start_completed_level( 2, 2000 ),
                        "held mode-start completed after release" );
  TEST_ASSERT_EQUAL( OSAL_ERROR, rec.result );
  _reap_start( task, &rec );

  memset( &st, 0, sizeof( st ) );
  _snap_mock( &st );
  TEST_ASSERT_FALSE_MESSAGE( st.started,
                             "failing mode-start effect applied at completion" );

  wifi_hal_mock_set_start_hold( false );
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "reset succeeds once quiescent" );
}

static void test_got_ip_and_scan_delivery_channels( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );

  /* start_scan() auto-fire advances the SCAN_DONE delivery channel. */
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start_scan( false ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_scan_done_delivered_level( 1, 2000 ),
                        "scan_done auto-fire delivered" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_scan_done_delivered_count() );

  /* A held scan withholds the auto-fire; inject_event(SCAN_DONE) completes. */
  wifi_hal_mock_set_scan_done_hold( true );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start_scan( false ) );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_scan_done_delivered_level( 2, 100 ),
                         "scan_done withheld while held" );
  wifi_hal_mock_set_scan_done_hold( false );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_SCAN_DONE, NULL );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_scan_done_delivered_level( 2, 2000 ),
                        "injected SCAN_DONE delivered" );

  /* inject_event(GOT_IP) advances the GOT_IP delivery channel. */
  wifi_hal_event_data_t evt = { 0 };
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_got_ip_delivered_level( 1, 2000 ),
                        "GOT_IP delivered generation 1" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_got_ip_delivered_count() );

  /* The GOT_IP hold withholds delivery: no callback, no generation. */
  wifi_hal_mock_set_got_ip_hold( true );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_got_ip_delivered_level( 2, 0 ),
                         "GOT_IP withheld while held" );
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_got_ip_delivered_count() );
  wifi_hal_mock_set_got_ip_hold( false );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_got_ip_delivered_level( 2, 2000 ),
                        "GOT_IP delivered after clearing hold" );
  TEST_ASSERT_EQUAL( 2, (int) wifi_hal_mock_get_got_ip_delivered_count() );

  /* A stale event token cannot satisfy a later requested generation. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_got_ip_delivered_level( 3, 0 ),
                         "stale GOT_IP token cannot satisfy level 3" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_scan_done_delivered_level( 3, 0 ),
                         "stale SCAN_DONE token cannot satisfy level 3" );

  wifi_hal_mock_reset();
}

static void test_reset_drains_new_channels( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );

  /* Drive every channel to generation 1 with tokens pending. */
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_connect() );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start( WIFI_HAL_MODE_STA ) );
  wifi_hal_init_t init = _make_init( NULL );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_init( &init ) );
  wifi_hal_event_data_t evt = { 0 };
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_start_scan( false ) );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_connect_call_level( 1, 0 ),
                        "connect generation 1 present" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_start_entered_level( 1, 0 ),
                        "start generation 1 present" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_got_ip_delivered_level( 1, 0 ),
                        "GOT_IP generation 1 present" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_scan_done_delivered_level( 1, 0 ),
                        "scan generation 1 present" );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(),
                        "reset drains every channel atomically" );

  /* Reset zeroed the generations and drained the tokens. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_connect_call_level( 1, 0 ),
                         "connect channel drained" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_start_entered_level( 1, 0 ),
                         "start channel drained" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_got_ip_delivered_level( 1, 0 ),
                         "GOT_IP channel drained" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_wait_scan_done_delivered_level( 1, 0 ),
                         "scan channel drained" );

  /* Fresh rounds are visible again after reset. */
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, wifi_hal_connect() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_connect_call_level( 1, 2000 ),
                        "fresh connect generation after reset" );
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
  RUN_TEST( test_held_init_acknowledge_and_release );
  RUN_TEST( test_wait_init_entered_is_token_consuming );
  RUN_TEST( test_reset_drains_old_tokens_only );
  RUN_TEST( test_reset_rejected_while_init_held );
  RUN_TEST( test_init_release_rounds_and_stale_hints );
  RUN_TEST( test_init_failure_still_counts_generation );
  RUN_TEST( test_stop_and_deinit_generation_waits );
  RUN_TEST( test_deinit_hold_blocks_reset_until_release );
  RUN_TEST( test_lifecycle_snapshot_fields_and_zeroing );
  RUN_TEST( test_init_failure_is_visible_in_snapshot );
  RUN_TEST( test_stop_failure_and_success_in_snapshot );
  RUN_TEST( test_deinit_failure_and_success_in_snapshot );
  RUN_TEST( test_event_delivery_holds_no_lock_during_callback );

  /* TASK-139: full snapshot-copy API, locked config/results, and the
     connect / mode-start / GOT_IP / SCAN_DONE acknowledged channels. */
  RUN_TEST( test_state_snapshot_copy_api );
  RUN_TEST( test_connect_channel_and_hold );
  RUN_TEST( test_start_mode_channel_and_hold );
  RUN_TEST( test_got_ip_and_scan_delivery_channels );
  RUN_TEST( test_reset_drains_new_channels );
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