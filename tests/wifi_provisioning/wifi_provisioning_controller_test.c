/*
 * Wi-Fi provisioning automatic fallback controller unit tests (POSIX).
 *
 * Exercises the policy layer against lightweight mocks of the Wi-Fi
 * management event API and the provisioning application lifecycle API:
 *  - no saved credential               -> provisioning starts on init,
 *  - successful saved credential       -> no portal; goes ONLINE,
 *  - exhausted saved credentials       -> provisioning starts once,
 *  - transient disconnect              -> provisioning is never opened,
 *  - success grace period              -> portal kept, then STA-only,
 *  - failure inside the grace window   -> portal kept, timer cancelled,
 *  - zero grace period                 -> immediate STA-only transition,
 *  - explicit stop                     -> overrides the pending timer,
 *  - init/deinit are idempotent,
 *  - deinit unsubscribes every callback.
 *
 * The controller source is compiled against mock implementations of
 * wifi_mgmt_is_read_data/subscribe/unsubscribe, wifi_http_provisioning_start,
 * wifi_http_provisioning_stop, wifi_mgmt_request_mode and the osal_timer_*
 * API so the policy can be driven deterministically by injecting typed events
 * and by firing the grace timeout on demand.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "osal_timer.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"
#include "wifi_provisioning_controller.h"

/* ---------------------------------------------------------------- mocks --- */

#define MOCK_MAX_SUBS 16

typedef struct
{
  wifi_mgmt_event_t    event;
  wifi_mgmt_event_cb_t cb;
  void*                user_data;
} mock_sub_t;

static bool      s_has_saved_credentials = false;
static int       s_provision_start_count = 0;
static int       s_provision_stop_count  = 0;
static mock_sub_t s_subs[ MOCK_MAX_SUBS ];
static int       s_sub_count             = 0;

/* Mock of the runtime mode-transition request. */
static wifi_type_t s_last_mode          = T_WIFI_TYPE_SERVER;
static int         s_request_mode_count = 0;

/* Mock grace timer bookkeeping. */
static void (*s_timer_cb)(osal_timer_id_t) = NULL;
static bool     s_timer_active             = false;
static uint32_t s_timer_period             = 0u;
static uint32_t s_timer_change_count       = 0u;
static struct osal_timer_internal s_mock_timer;

/* Records the order of shutdown steps so a test can prove the listeners are
 * stopped before the STA-only transition: 'S' = listener stop, 'M' = mode. */
static char  s_action_log[64];
static int   s_action_log_len = 0;

static void log_action( const char *action )
{
  int n = (int) strlen( action );
  int i;
  for ( i = 0; i < n && s_action_log_len < 63; ++i )
  {
    s_action_log[ s_action_log_len++ ] = action[i];
  }
}

/* Mock of the Wi-Fi persistence query used during init. */
bool wifi_mgmt_is_read_data( void )
{
  return s_has_saved_credentials;
}

/* Mock subscription table recording callbacks so tests can fire events. */
bool wifi_mgmt_subscribe( wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void* user_data )
{
  int i;

  if ( !cb ) return false;
  for ( i = 0; i < s_sub_count; ++i )
  {
    if ( s_subs[ i ].event == event && s_subs[ i ].cb == cb &&
         s_subs[ i ].user_data == user_data )
      return false;    /* duplicate registration. */
  }
  if ( s_sub_count >= MOCK_MAX_SUBS ) return false;
  s_subs[ s_sub_count ].event     = event;
  s_subs[ s_sub_count ].cb        = cb;
  s_subs[ s_sub_count ].user_data = user_data;
  ++s_sub_count;
  return true;
}

bool wifi_mgmt_unsubscribe( wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void* user_data )
{
  int i;

  if ( !cb ) return false;
  for ( i = 0; i < s_sub_count; ++i )
  {
    if ( s_subs[ i ].event == event && s_subs[ i ].cb == cb &&
         s_subs[ i ].user_data == user_data )
    {
      int j;
      for ( j = i + 1; j < s_sub_count; ++j ) s_subs[ j - 1 ] = s_subs[ j ];
      --s_sub_count;
      return true;
    }
  }
  return false;
}

/* Mock of the runtime STA-only transition request. Logs before/after the
 * listener stop only so the ordering assertion is deterministic. */
bool wifi_mgmt_request_mode( wifi_type_t type )
{
  ++s_request_mode_count;
  s_last_mode = type;
  log_action( "M" );
  return true;
}

/* Mock of the explicit provisioning lifecycle API. */
bool wifi_http_provisioning_start( void )
{
  ++s_provision_start_count;
  return true;
}

bool wifi_http_provisioning_stop( void )
{
  ++s_provision_stop_count;
  log_action( "S" );
  return true;
}

/* Mock of the grace timer. The controller creates one timer at init and arms
 * it with change_period + start on success; tests fire the recorded callback
 * to simulate expiry without waiting for real time. */
osal_status_t osal_timer_create( osal_timer_id_t *timer_id, const char *name,
                                 uint32_t period_ms, bool auto_reload,
                                 void (*callback)(osal_timer_id_t),
                                 void *callback_arg,
                                 osal_stackptr_t stack_pointer, size_t stack_size )
{
  (void) name;
  (void) period_ms;
  (void) auto_reload;
  (void) callback_arg;
  (void) stack_pointer;
  (void) stack_size;
  s_timer_cb     = callback;
  s_timer_active = false;
  *timer_id      = &s_mock_timer;
  return OSAL_SUCCESS;
}

osal_status_t osal_timer_change_period( osal_timer_id_t timer_id,
                                        uint32_t new_period_ms,
                                        uint32_t timeout_ms )
{
  (void) timer_id;
  (void) timeout_ms;
  s_timer_period       = new_period_ms;
  s_timer_change_count++;
  return OSAL_SUCCESS;
}

osal_status_t osal_timer_start( osal_timer_id_t timer_id, uint32_t timeout_ms )
{
  (void) timer_id;
  (void) timeout_ms;
  s_timer_active = true;
  return OSAL_SUCCESS;
}

osal_status_t osal_timer_stop( osal_timer_id_t timer_id, uint32_t timeout_ms )
{
  (void) timer_id;
  (void) timeout_ms;
  s_timer_active = false;
  return OSAL_SUCCESS;
}

osal_status_t osal_timer_delete( osal_timer_id_t timer_id, uint32_t timeout_ms )
{
  (void) timer_id;
  (void) timeout_ms;
  s_timer_active = false;
  s_timer_cb     = NULL;
  return OSAL_SUCCESS;
}

bool osal_timer_is_active( osal_timer_id_t timer_id )
{
  (void) timer_id;
  return s_timer_active;
}

/* Helpers ---------------------------------------------------------------- */

static int sub_count_for( wifi_mgmt_event_t event )
{
  int n = 0, i;
  for ( i = 0; i < s_sub_count; ++i ) if ( s_subs[ i ].event == event ) ++n;
  return n;
}

static void fire( wifi_mgmt_event_t event )
{
  mock_sub_t  snapshot[ MOCK_MAX_SUBS ];
  int         n = 0, i;
  for ( i = 0; i < s_sub_count; ++i )
  {
    if ( s_subs[ i ].event == event ) snapshot[ n++ ] = s_subs[ i ];
  }
  for ( i = 0; i < n; ++i ) snapshot[ i ].cb( event, snapshot[ i ].user_data );
}

/* Simulate the one-shot grace timer expiring. As with the real timer, expiry
 * first makes the timer dormant, then the expiry callback runs. */
static void fire_grace_expiry( void )
{
  s_timer_active = false;
  if ( s_timer_cb != NULL ) s_timer_cb( &s_mock_timer );
}

static void reset_mocks( void )
{
  /* Deinitialise the controller first so its internal static state does not
   * leak across tests (this also cancels/deletes the grace timer and
   * unsubscribes from the current table). It is a safe no-op when the
   * controller is already disabled. */
  wifi_provisioning_controller_deinit();
  s_has_saved_credentials = false;
  s_provision_start_count = 0;
  s_provision_stop_count  = 0;
  s_sub_count             = 0;
  s_request_mode_count    = 0;
  s_last_mode             = T_WIFI_TYPE_SERVER;
  s_timer_cb              = NULL;
  s_timer_active          = false;
  s_timer_period          = 0u;
  s_timer_change_count    = 0u;
  s_action_log_len        = 0;
  memset( s_action_log, 0, sizeof( s_action_log ) );
}

/* Assert that the mocked osal_timer was armed for the given period. */
static void assert_timer_armed( uint32_t period_ms )
{
  TEST_ASSERT_TRUE( osal_timer_is_active( NULL ) );
  TEST_ASSERT_EQUAL_UINT32( period_ms, s_timer_period );
}

/* Assert that a shutdown logged "S" (listener stop) before any "M"
 * (mode transition), i.e. that the STA-only request follows the shutdown. */
static void assert_stop_precedes_mode( void )
{
  TEST_ASSERT_TRUE( s_action_log_len >= 2 );
  TEST_ASSERT_EQUAL_CHAR( 'S', s_action_log[0] );
  TEST_ASSERT_EQUAL_CHAR( 'M', s_action_log[1] );
}

/* Test cases ------------------------------------------------------------- */

/* No saved credential: init must start provisioning exactly once and reach
 * PROVISIONING. A later CONNECT_FAILED must not open it a second time. */
static void test_no_saved_credentials_starts_once( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
  TEST_ASSERT_EQUAL_INT( 3, s_sub_count );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );

  /* A qualifying fallback is started exactly once even with more traffic. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_DISCONNECTED );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* Successful saved credential -> AWAITING_CONNECT -> ONLINE, no portal. */
static void test_saved_credentials_success_no_portal( void )
{
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  fire( WIFI_MGMT_EVENT_CONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );
}

/* A transient disconnect after a successful connect never opens the portal. */
static void test_transient_disconnect_does_not_start( void )
{
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  fire( WIFI_MGMT_EVENT_DISCONNECTED );

  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  /* Repeated transient loss must stay quiet. */
  fire( WIFI_MGMT_EVENT_DISCONNECTED );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );
}

/* Exhausted saved credentials start the portal, but only once. */
static void test_exhausted_credentials_starts_once( void )
{
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );

  /* A repeated failure after the fallback must not start a second portal. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* A fresh init (after deinit) re-arms the fallback-once guard. */
static void test_fallback_reenables_after_deinit( void )
{
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );

  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );

  /* A fresh init with saved credentials re-arms the fallback guard. */
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL_INT( 2, s_provision_start_count );
}

/* Init and deinit must be idempotent: repeated calls do not re-subscribe and
 * a deinit while already disabled is a safe no-op. */
static void test_init_deinit_idempotent_unsubscribes_all( void )
{
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( 3, s_sub_count );

  /* Repeated init does not re-subscribe. */
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( 3, s_sub_count );

  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_sub_count );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_CONNECTED ) );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_DISCONNECTED ) );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_CONNECT_FAILED ) );

  /* Deinit while disabled is a safe no-op. */
  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL_INT( 0, s_sub_count );
}

/* Success during the grace period: the portal stays up until the timer fires,
 * then the listeners shut down and the mode transitions to STA-only. */
static void test_grace_success_stops_after_expiry( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );

  /* A submitted credential connects: the station obtained an IP. */
  fire( WIFI_MGMT_EVENT_CONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_GRACE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  assert_timer_armed( 200 );

  /* During the grace window the portal and the temporary AP must stay up. */
  TEST_ASSERT_EQUAL_INT( 0, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 0, s_request_mode_count );

  /* Expiry shuts the listeners down before requesting a STA-only transition. */
  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  assert_stop_precedes_mode();
}

/* Zero grace period: the portal is retired immediately on station IP. */
static void test_zero_grace_shuts_down_immediately( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 0 );

  fire( WIFI_MGMT_EVENT_CONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  TEST_ASSERT_EQUAL_UINT32( 0u, s_timer_change_count );
  assert_stop_precedes_mode();
}

/* Failure during the grace period cancels the timer and keeps the portal. */
static void test_failure_during_grace_keeps_portal( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  /* The fresh connection fails before the grace expires. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_stop_count );

  /* A later successful connection re-arms the grace window. */
  fire( WIFI_MGMT_EVENT_CONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_GRACE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( osal_timer_is_active( NULL ) );
}

/* A disconnect during the grace period also aborts the window. */
static void test_disconnect_during_grace_keeps_portal( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  fire( WIFI_MGMT_EVENT_DISCONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_stop_count );
}

/* Explicit stop overrides a pending grace timer and transitions to DISABLED. */
static void test_explicit_stop_overrides_grace_timer( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  TEST_ASSERT_TRUE( wifi_provisioning_controller_stop() );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  assert_stop_precedes_mode();

  /* A stale expiry after the explicit stop must not reopen the portal. */
  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
}

/* Runner ------------------------------------------------------------------- */

static void run_controller_tests( void )
{
  RUN_TEST( test_no_saved_credentials_starts_once );
  RUN_TEST( test_saved_credentials_success_no_portal );
  RUN_TEST( test_transient_disconnect_does_not_start );
  RUN_TEST( test_exhausted_credentials_starts_once );
  RUN_TEST( test_fallback_reenables_after_deinit );
  RUN_TEST( test_init_deinit_idempotent_unsubscribes_all );
  RUN_TEST( test_grace_success_stops_after_expiry );
  RUN_TEST( test_zero_grace_shuts_down_immediately );
  RUN_TEST( test_failure_during_grace_keeps_portal );
  RUN_TEST( test_disconnect_during_grace_keeps_portal );
  RUN_TEST( test_explicit_stop_overrides_grace_timer );
}

/* setUp/tearDown are intentionally empty (each test resets its own state). */
void setUp( void ) { }
void tearDown( void ) { }

int main( void )
{
  setvbuf( stdout, NULL, _IONBF, 0 );
  UNITY_BEGIN();
  run_controller_tests();
  return UNITY_END();
}