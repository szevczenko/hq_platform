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
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_get_state()->initialized,
                             "callback not installed while held" );

  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &rec, 2000 ),
                            "init returned after release" );
  TEST_ASSERT_EQUAL( OSAL_SUCCESS, rec.result );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state()->initialized,
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
  TEST_ASSERT_EQUAL( 1, (int) wifi_hal_mock_get_state()->init_count );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_get_state()->initialized,
                             "active round was not reset" );

  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( _init_done( &rec, 2000 ),
                            "held init released after rejected reset" );
  _reap_init( task, &rec );

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(),
                            "reset succeeds once init is quiescent" );
  TEST_ASSERT_EQUAL( 0, (int) wifi_hal_mock_get_state()->init_count );
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