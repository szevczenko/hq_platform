/*
 * Wi-Fi provisioning automatic fallback controller unit tests (POSIX).
 *
 * Exercises the policy layer against lightweight mocks of the Wi-Fi
 * management event API, the provisioning application lifecycle API and the
 * OSAL mutex/timer API:
 *  - no saved credential               -> provisioning starts on init,
 *  - successful saved credential       -> no portal; goes ONLINE,
 *  - exhausted saved credentials       -> provisioning starts once,
 *  - fallback budget                  -> opens only after N consecutive
 *                                        CONNECT_FAILED, reset on success,
 *                                        zero disables fallback,
 *  - transient disconnect              -> provisioning is never opened,
 *  - success grace period              -> portal kept, then STA-only,
 *  - failure inside the grace window   -> portal kept, timer cancelled,
 *  - zero grace period                 -> immediate STA-only transition,
 *  - explicit stop                     -> overrides the pending timer,
 *  - pre-init grace override           -> the configured value is armed,
 *  - stale expiry                      -> ignored after cancel and deinit,
 *  - disconnect-versus-expiry          -> the cancelled window is a no-op,
 *  - deinit-versus-expiry              -> the timer is joined/safe,
 *  - stale expiry vs a new session     -> never stops a new portal,
 *  - stop failure / mode failure       -> recoverable, never ONLINE,
 *  - mode acknowledgement              -> ONLINE only after MODE_CHANGED,
 *  - init/deinit are idempotent,
 *  - deinit unsubscribes every callback,
 *  - opt-in state-change notification  -> fired on every documented transition,
 *  - stale-session notification        -> discarded after deinit/re-init,
 *  - NULL callback                     -> safe no-op,
 *  - re-entrant get_state() callback   -> no deadlock/panic.
 *
 * The controller source is compiled against mock implementations of
 * wifi_mgmt_is_read_data/subscribe/unsubscribe/request_mode,
 * wifi_http_provisioning_start/stop, and the osal_timer_* and osal_mutex_*
 * APIs so the policy can be driven deterministically by firing typed events
 * and by firing the grace timeout on demand.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "osal_bin_sem.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_task.h"
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
static bool        s_request_mode_succeeds = true;

/* Mock of the provisioning listener stop; a test may force a stop failure. */
static bool s_stop_succeeds = true;

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

/* --- OSAL log mock -------------------------------------------------------- */
/* The controller logs a dropped deferred item (queue full). This single-
 * threaded test binary does not link the real OSAL log implementation, so a
 * swallow-all sink keeps the policy logic linkable and silent. */
void osal_log_printf( const char *level, const char *format, ... )
{
  (void) level;
  (void) format;
}

/* --- OSAL mutex mocks (single-threaded controller test) ------------------ */
static pthread_mutex_t s_mock_mutex;

osal_status_t osal_mutex_create( osal_mutex_id_t *mutex_id, const char *name )
{
  (void) name;
  if ( mutex_id == NULL ) return OSAL_INVALID_POINTER;
  *mutex_id = &s_mock_mutex;
  return OSAL_SUCCESS;
}

osal_status_t osal_mutex_take( osal_mutex_id_t mutex_id )
{
  (void) mutex_id;
  return OSAL_SUCCESS;
}

osal_status_t osal_mutex_give( osal_mutex_id_t mutex_id )
{
  (void) mutex_id;
  return OSAL_SUCCESS;
}

osal_status_t osal_mutex_delete( osal_mutex_id_t mutex_id )
{
  (void) mutex_id;
  return OSAL_SUCCESS;
}

/* --- Mock of the Wi-Fi persistence query used during init. -------------------- */
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

/* Mock of the runtime STA-only transition request. Logs after the listener stop
 * only so the ordering assertion is deterministic. */
bool wifi_mgmt_request_mode( wifi_type_t type )
{
  ++s_request_mode_count;
  s_last_mode = type;
  log_action( "M" );
  return s_request_mode_succeeds;
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
  return s_stop_succeeds;
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

/* --- OSAL binary semaphore mocks (single-threaded controller test) ------- */
/* The controller creates process-lifetime semaphores (worker-exit signal,
 * test-hold channel) and only uses them from the deferred worker and deinit.
 * Under this single-threaded mock every wait succeeds immediately and every
 * give is a no-op. */
osal_status_t osal_bin_sem_create( osal_bin_sem_id_t *sem_id, const char *name,
                                   uint32_t initial_value )
{
  (void) name;
  (void) initial_value;
  if ( sem_id == NULL ) return OSAL_INVALID_POINTER;
  *sem_id = (osal_bin_sem_id_t) 0x1;    /* any non-NULL opaque handle. */
  return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_delete( osal_bin_sem_id_t sem_id )
{
  (void) sem_id;
  return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_give( osal_bin_sem_id_t sem_id )
{
  (void) sem_id;
  return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_timed_wait( osal_bin_sem_id_t sem_id, uint32_t timeout_ms )
{
  (void) sem_id;
  (void) timeout_ms;
  return OSAL_SUCCESS;
}

/* --- OSAL task mocks ----------------------------------------------------- */
/* osal_task_create records the deferred worker routine instead of spawning a
 * thread; the test drains the queue synchronously by invoking the routine. On
 * an empty queue the mock receive fails immediately, so one invocation of the
 * routine applies every queued item in FIFO order and returns. */
static void (*s_task_routine)(void *) = NULL;
static void  *s_task_arg              = NULL;

osal_status_t osal_task_create( osal_task_id_t *task_id, const char *task_name,
                                void (*routine)(void *), void *arg,
                                osal_stackptr_t stack_pointer, size_t stack_size,
                                osal_priority_t priority, const osal_task_attr_t *attr )
{
  (void) task_name;
  (void) stack_pointer;
  (void) stack_size;
  (void) priority;
  (void) attr;
  if ( task_id == NULL || routine == NULL ) return OSAL_INVALID_POINTER;
  s_task_routine = routine;
  s_task_arg     = arg;
  *task_id       = (osal_task_id_t) 0x1;
  return OSAL_SUCCESS;
}

osal_status_t osal_task_delete( osal_task_id_t task_id )
{
  (void) task_id;
  return OSAL_SUCCESS;
}

uint32_t osal_task_get_time_ms( void )
{
  return 0u;
}

osal_status_t osal_task_delay_ms( uint32_t milliseconds )
{
  (void) milliseconds;
  return OSAL_SUCCESS;
}

/* --- OSAL queue mocks ---------------------------------------------------- */
/* Opaque FIFO item store big enough for the controller's private deferred
 * item (action + event + session). At most one controller queue exists at a
 * time, so a single global mock queue is sufficient. */
#define MOCK_QUEUE_MAX_ITEMS 16u
#define MOCK_QUEUE_ITEM_SIZE 64u

typedef struct
{
  uint8_t  items[MOCK_QUEUE_MAX_ITEMS][MOCK_QUEUE_ITEM_SIZE];
  uint32_t item_size;
  uint32_t count;
  uint32_t head;
} mock_queue_t;

static mock_queue_t s_mock_queue;

static void mock_queue_reset( void )
{
  s_mock_queue.item_size = 0u;
  s_mock_queue.count     = 0u;
  s_mock_queue.head      = 0u;
}

osal_status_t osal_queue_create( osal_queue_id_t *queue_id, const char *name,
                                 uint32_t max_items, uint32_t item_size )
{
  (void) name;
  if ( queue_id == NULL ) return OSAL_INVALID_POINTER;
  if ( max_items == 0u || item_size == 0u ) return OSAL_QUEUE_INVALID_SIZE;
  s_mock_queue.item_size =
    item_size < MOCK_QUEUE_ITEM_SIZE ? item_size : MOCK_QUEUE_ITEM_SIZE;
  s_mock_queue.count = 0u;
  s_mock_queue.head  = 0u;
  *queue_id          = (osal_queue_id_t) 0x1;
  return OSAL_SUCCESS;
}

osal_status_t osal_queue_send( osal_queue_id_t queue_id, const void *item,
                               uint32_t timeout_ms )
{
  uint32_t tail;

  (void) queue_id;
  (void) timeout_ms;
  if ( s_mock_queue.count >= MOCK_QUEUE_MAX_ITEMS ) return OSAL_QUEUE_FULL;
  tail = ( s_mock_queue.head + s_mock_queue.count ) % MOCK_QUEUE_MAX_ITEMS;
  memcpy( s_mock_queue.items[tail], item, s_mock_queue.item_size );
  ++s_mock_queue.count;
  return OSAL_SUCCESS;
}

osal_status_t osal_queue_receive( osal_queue_id_t queue_id, void *buffer,
                                  uint32_t timeout_ms )
{
  (void) queue_id;
  (void) timeout_ms;
  if ( s_mock_queue.count == 0u ) return OSAL_QUEUE_EMPTY;
  memcpy( buffer, s_mock_queue.items[s_mock_queue.head], s_mock_queue.item_size );
  s_mock_queue.head = ( s_mock_queue.head + 1u ) % MOCK_QUEUE_MAX_ITEMS;
  --s_mock_queue.count;
  return OSAL_SUCCESS;
}

osal_status_t osal_queue_delete( osal_queue_id_t queue_id )
{
  (void) queue_id;
  mock_queue_reset();
  return OSAL_SUCCESS;
}

uint32_t osal_queue_get_count( osal_queue_id_t queue_id )
{
  (void) queue_id;
  return s_mock_queue.count;
}

/* Helpers ---------------------------------------------------------------- */

static int sub_count_for( wifi_mgmt_event_t event )
{
  int n = 0, i;
  for ( i = 0; i < s_sub_count; ++i ) if ( s_subs[ i ].event == event ) ++n;
  return n;
}

/* Run the recorded deferred worker routine once. Under the task mock the
 * worker is never spawned as a real thread; the queue mock makes an empty
 * receive fail immediately, so one invocation drains every queued item in
 * FIFO order and returns. Initially the routine may not be recorded (before
 * init), in which case there is nothing to drain. */
static void drain_deferred( void )
{
  if ( s_task_routine != NULL ) s_task_routine( s_task_arg );
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
  /* The event callback only posts to the deferred queue; apply the queued
   * item(s) synchronously so the caller can assert the resulting state. */
  drain_deferred();
}

/* Simulate the one-shot grace timer expiring. As with a real timer, expiry
 * first makes the timer dormant, then the expiry callback runs (which posts
 * the grace-expiry to the deferred queue, then drained below). */
static void fire_grace_expiry( void )
{
  s_timer_active = false;
  if ( s_timer_cb != NULL ) s_timer_cb( &s_mock_timer );
  drain_deferred();
}

static void reset_mocks( void )
{
  /* Deinitialize the controller first so its internal static state does not
   * leak across tests (this also cancels/deletes the grace timer, invalidates
   * the session and unsubscribes the delegates). No direct portal stop is
   * issued, matching the controller contract. */
  wifi_provisioning_controller_deinit();
  s_has_saved_credentials = false;
  s_provision_start_count = 0;
  s_provision_stop_count  = 0;
  s_sub_count             = 0;
  s_request_mode_count    = 0;
  s_request_mode_succeeds = true;
  s_stop_succeeds          = true;
  s_last_mode             = T_WIFI_TYPE_SERVER;
  s_timer_cb              = NULL;
  s_timer_active          = false;
  s_timer_period          = 0u;
  s_timer_change_count    = 0u;
  s_task_routine          = NULL;
  s_task_arg              = NULL;
  mock_queue_reset();
  s_action_log_len        = 0;
  memset( s_action_log, 0, sizeof( s_action_log ) );
}

/* Assert that the mocked osal_timer was armed for the given period. */
static void assert_timer_armed( uint32_t period_ms )
{
  TEST_ASSERT_TRUE( osal_timer_is_active( NULL ) );
  TEST_ASSERT_EQUAL_UINT32( period_ms, s_timer_period );
}

/* --- state-change notification helpers ------------------------------------ */

#define NOTIFY_MAX 64

/* Golden record of one delivered state-change notification. */
typedef struct
{
  wifi_provisioning_controller_state_t prev;
  wifi_provisioning_controller_state_t cur;
  uint32_t                             session;
} notify_record_t;

static notify_record_t s_notify_records[ NOTIFY_MAX ];
static int             s_notify_count        = 0;
static void           *s_notify_user_ctx_seen = NULL;

/* Product-side mirror state maintained by the filtering callback. */
static wifi_provisioning_controller_state_t s_product_state = WIFI_PROVISIONING_CONTROLLER_DISABLED;
static uint32_t s_product_view_session = 0u;   /* Newest lifecycle token accepted. */
static int      s_product_stale_count  = 0;    /* Notifications dropped as stale. */

static void record_notify( wifi_provisioning_controller_state_t prev,
                           wifi_provisioning_controller_state_t cur,
                           uint32_t session,
                           void *user_ctx )
{
  s_notify_user_ctx_seen = user_ctx;
  if ( s_notify_count < NOTIFY_MAX )
  {
    s_notify_records[ s_notify_count ].prev    = prev;
    s_notify_records[ s_notify_count ].cur     = cur;
    s_notify_records[ s_notify_count ].session = session;
    ++s_notify_count;
  }
}

/* Plain recorder: accepts every notification. */
static void on_state_changed( wifi_provisioning_controller_state_t prev,
                              wifi_provisioning_controller_state_t cur,
                              uint32_t session,
                              void *user_ctx )
{
  record_notify( prev, cur, session, user_ctx );
}

/* Product-style callback: keeps a mirror state machine and drops notifications
 * whose session token predates the newest lifecycle it has seen, exactly what
 * a state-machine-driven product does with the generation token. */
static void on_state_changed_filter_session( wifi_provisioning_controller_state_t prev,
                                             wifi_provisioning_controller_state_t cur,
                                             uint32_t session,
                                             void *user_ctx )
{
  (void) prev;
  if ( session < s_product_view_session )
  {
    ++s_product_stale_count;    /* stale: discarded, mirror not updated. */
    return;
  }
  s_product_view_session = session;
  s_product_state        = cur;
  record_notify( prev, cur, session, user_ctx );
}

/* Re-entrant callback: queries get_state() from inside the notification. The
 * controller must deliver notifications with its lock released, so this must
 * neither deadlock nor panic and must observe the very state just entered. */
static void on_state_changed_query_state( wifi_provisioning_controller_state_t prev,
                                          wifi_provisioning_controller_state_t cur,
                                          uint32_t session,
                                          void *user_ctx )
{
  record_notify( prev, cur, session, user_ctx );
  TEST_ASSERT_EQUAL( cur, wifi_provisioning_controller_get_state() );
}

/* Convenience: install the recorder and initialize with no saved credentials
 * (fresh lifecycle, provisioning starts immediately). */
static void init_with_recorder( const wifi_provisioning_controller_config_t *cfg )
{
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init_with_config( cfg ) );
}

static void reset_notify_records( void )
{
  s_notify_count          = 0;
  s_notify_user_ctx_seen  = (void*) 0;
  s_product_state         = WIFI_PROVISIONING_CONTROLLER_DISABLED;
  s_product_view_session  = 0u;
  s_product_stale_count   = 0;
}

/* Assert that the recorded notifications equal the expected (prev, cur)
 * sequence and that every delivered token belongs to the same controller
 * lifecycle (identical, non-zero session). The absolute value is intentionally
 * not pinned: the session is a running generation counter that only increases
 * across the lifecycles exercised by the whole test process. */
static void assert_notify_sequence( const wifi_provisioning_controller_state_t *expected,
                                    int count )
{
  uint32_t first_session;
  int i;

  TEST_ASSERT_TRUE( count > 0 );
  TEST_ASSERT_EQUAL_INT( count, s_notify_count );
  first_session = s_notify_records[ 0 ].session;
  TEST_ASSERT_TRUE( first_session != 0u );
  for ( i = 0; i < count; ++i )
  {
    TEST_ASSERT_EQUAL_INT( expected[ 2 * i ],     s_notify_records[ i ].prev );
    TEST_ASSERT_EQUAL_INT( expected[ 2 * i + 1 ], s_notify_records[ i ].cur );
    TEST_ASSERT_EQUAL_UINT32( first_session, s_notify_records[ i ].session );
  }
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
 * PROVISIONING. A later CONNECT_FAILED must not open it again. */
static void test_no_saved_credentials_starts_once( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
  TEST_ASSERT_EQUAL_INT( 4, s_sub_count );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );

  /* A qualifying fallback is started exactly once even with more traffic. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_DISCONNECTED );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* Successful saved credential retires the startup AP and reaches ONLINE only
 * after the STA-only mode change is acknowledged. */
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
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
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
  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
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
/* Fallback budget honored: with a per-device budget of N the portal opens only
 * on the Nth consecutive CONNECT_FAILED while awaiting a saved-credential
 * connection; the N-1 failures stay quiet and only the exhausted-budget
 * fallback delivers the state-change notification. */
static void test_fallback_budget_n_honored( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed, NULL, true, 3u };

  reset_mocks();
  reset_notify_records();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init_with_config( &cfg ) );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );
  TEST_ASSERT_EQUAL_INT( 1, s_notify_count );   /* init: DISABLED -> AWAITING_CONNECT */

  /* Budget 3: the first two failures never open the portal and produce no
   * notification (the policy decision has not been made yet). */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_notify_count );

  /* The third consecutive failure exhausts the budget: fallback fires and the
   * AWAITING_CONNECT -> PROVISIONING notification is delivered. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
  TEST_ASSERT_EQUAL_INT( 2, s_notify_count );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                         s_notify_records[ 1 ].prev );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         s_notify_records[ 1 ].cur );

  /* Once fired, further failures do not start a second portal. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* The budget is reset on a successful saved-credential connect and re-armed
 * when the controller returns to AWAITING_CONNECT from ONLINE: failures from
 * one cycle never carry into the next. */
static void test_fallback_budget_reset_on_success( void )
{
  wifi_provisioning_controller_config_t cfg = { NULL, NULL, true, 3u };

  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init_with_config( &cfg ) );

  /* Two failures against a budget of 3: still awaiting, no portal. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  /* A successful saved-credential connect resets the budget, and the return
   * to AWAITING_CONNECT re-arms it for a fresh cycle. */
  fire( WIFI_MGMT_EVENT_CONNECTED );
  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  fire( WIFI_MGMT_EVENT_DISCONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );

  /* Two fresh failures stay quiet: the counter was reset by CONNECTED (and
   * re-armed on the ONLINE loss); without the reset the cumulative 4 failures
   * would already have exhausted the budget of 3. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  /* The third fresh consecutive failure exhausts the re-armed budget. */
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* A zero budget disables the failure-driven fallback entirely: no amount of
 * CONNECT_FAILED opens the portal, while the fresh-device path (no saved
 * credential -> portal on init) stays immediate. */
static void test_fallback_budget_zero_disables_fallback( void )
{
  wifi_provisioning_controller_config_t cfg = { NULL, NULL, true, 0u };

  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init_with_config( &cfg ) );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );

  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );

  /* The fresh-device path is unaffected: no saved credential opens the portal
   * immediately even though the fallback budget is zero. */
  wifi_provisioning_controller_deinit();
  reset_mocks();                       /* also clears s_has_saved_credentials */
  TEST_ASSERT_FALSE( s_has_saved_credentials );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init_with_config( &cfg ) );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* Default-budget backward compatibility: a config that leaves the budget
 * unset (or plain init()) keeps the historical first-CONNECT_FAILED behavior
 * supplied by the Kconfig default (1). */
static void test_fallback_budget_default_backward_compat( void )
{
  wifi_provisioning_controller_config_t unset = { NULL, NULL, false, 0u };

  /* Config without fallback_budget_set: the Kconfig default (1) applies, so a
   * single CONNECT_FAILED still opens the portal. */
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init_with_config( &unset ) );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );

  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );

  /* Plain init() (NULL config) keeps the same default behavior. */
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  fire( WIFI_MGMT_EVENT_CONNECT_FAILED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_start_count );
}

/* Init and deinit must be idempotent: repeated calls do not re-subscribe and
 * a deinit while already disabled is a safe no-op. */
static void test_init_deinit_idempotent_unsubscribes_all( void )
{
  reset_mocks();
  s_has_saved_credentials = true;
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( 4, s_sub_count );

  /* Repeated init does not re-subscribe. */
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( 4, s_sub_count );

  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_sub_count );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_CONNECTED ) );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_DISCONNECTED ) );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_CONNECT_FAILED ) );
  TEST_ASSERT_EQUAL_INT( 0, sub_count_for( WIFI_MGMT_EVENT_MODE_CHANGED ) );

  /* Deinit while disabled is a safe no-op. */
  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL_INT( 0, s_sub_count );
}

/* Success during the grace period: the portal stays up until the timer fires,
 * then the listeners shut down and only after the STA-only confirmation does
 * the controller report ONLINE. */
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

  /* Expiry stops the listeners, requests STA-only but must NOT report ONLINE
   * until the mode change is confirmed. */
  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  assert_stop_precedes_mode();

  /* The requested STA-only mode is confirmed : ONLINE at last. */
  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
}

/* Zero grace period: the portal is retired on the station IP and ONLINE only
 * after the STA-only mode is confirmed. */
static void test_zero_grace_shuts_down_immediately( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 0 );

  fire( WIFI_MGMT_EVENT_CONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  TEST_ASSERT_EQUAL_UINT32( 0u, s_timer_change_count );
  assert_stop_precedes_mode();

  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
}

/* A grace value configured before init is the one that is actually armed
 * (the build default is never applied once an override exists). */
static void test_pre_override_is_armed( void )
{
  reset_mocks();
  wifi_provisioning_controller_set_success_grace_ms( 333 );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_GRACE,
                     wifi_provisioning_controller_get_state() );
  assert_timer_armed( 333 );
}

/* A failure inside the grace window cancels the timer and keeps the portal. */
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

/* An expiry racing a disconnect (disconnect-versus-expiry) is a stale no-op:
 * after the window is aborted the late expiry must do nothing. */
static void test_disconnect_then_stale_expiry_is_ignored( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  fire( WIFI_MGMT_EVENT_DISCONNECTED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );

  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_stop_count );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
}

/* Explicit stop overrides a pending grace timer; the controller only reaches
 * DISABLED once the STA-only transition is confirmed, and a stale expiry after
 * the stop must not reopen the portal. */
static void test_explicit_stop_overrides_grace_timer( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  TEST_ASSERT_TRUE( wifi_provisioning_controller_stop() );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 1, s_request_mode_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  TEST_ASSERT_FALSE( osal_timer_is_active( NULL ) );
  assert_stop_precedes_mode();

  /* The mode confirm completes the stop to DISABLED. */
  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );

  /* A stale expiry after the explicit stop must not reopen the portal. */
  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
}

/* A listener shutdown failure leaves the controller recoverable (never
 * ONLINE): the portal stays available for another attempt. */
static void test_stop_failure_keeps_provisioning( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );

  s_stop_succeeds = false;
  TEST_ASSERT_FALSE( wifi_provisioning_controller_stop() );

  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  /* The mode request is not even issued if the listener could not stop. */
  TEST_ASSERT_EQUAL_INT( 0, s_request_mode_count );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
}

/* A mode-transition failure is recoverable: the listeners are stopped but the
 * portal is reopened and the controller never reports ONLINE. */
static void test_mode_failure_keeps_provisioning( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  s_request_mode_succeeds = false;
  const int start_after_connect = s_provision_start_count;

  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
  TEST_ASSERT_EQUAL_INT( 1, s_provision_stop_count );
  TEST_ASSERT_EQUAL( T_WIFI_TYPE_CLIENT, s_last_mode );
  /* The rejected transition reopened the portal so it stays recoverable. */
  TEST_ASSERT_EQUAL_INT( start_after_connect + 1, s_provision_start_count );

  /* No MODE_CHANGED applied -> must never be ONLINE. */
  TEST_ASSERT( wifi_provisioning_controller_get_state() !=
               WIFI_PROVISIONING_CONTROLLER_ONLINE );
}

/* Deinit versus a pending expiry: deinit cancel/deletes the timer and joins a
 * late expiry; a queued stale expiry afterwards must not alter disabled state. */
static void test_deinit_then_stale_expiry_is_ignored( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 200 );

  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );

  /* The timer was deleted, so the expiry no-op is safe. */
  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_stop_count );
}

/* A stale expiry from a cancelled/prior session cannot stop a new provisioning
 * session started after a fresh init. */
static void test_stale_expiry_cannot_stop_new_session( void )
{
  reset_mocks();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 100 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  assert_timer_armed( 100 );

  /* End the first session: the timer is cancelled/joined and fresh session. */
  wifi_provisioning_controller_deinit();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );   /* session #2 */

  /* A stale grace expiry (queued from the old session, now delivered) must
   * not stop the new session's portal. */
  TEST_ASSERT_EQUAL_INT( 2, s_provision_start_count );   /* 1 from each init */
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );

  fire_grace_expiry();
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_stop_count );
  TEST_ASSERT_EQUAL_INT( 2, s_provision_start_count );
}

/* --- state-change notification tests --------------------------------------- */

/* The hook fires for the init transitions: DISABLED -> AWAITING_CONNECT and
 * the immediate fallback entry AWAITING_CONNECT -> PROVISIONING, each carrying
 * the current lifecycle's session token and the registered user context. */
static void test_callback_fired_on_init_and_fallback( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed, (void*) 0xCAFEu };
  static const wifi_provisioning_controller_state_t expected[] = {
    WIFI_PROVISIONING_CONTROLLER_DISABLED,         WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
    WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
  };

  reset_mocks();
  reset_notify_records();
  init_with_recorder( &cfg );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  assert_notify_sequence( expected, 2 );
  TEST_ASSERT_EQUAL( (void*) 0xCAFEu, s_notify_user_ctx_seen );
}

/* The full success lifecycle is observable end to end: PROVISIONING -> GRACE
 * on station IP, GRACE -> RETIRING_AP on expiry, RETIRING_AP -> ONLINE on the
 * STA-only confirmation. All tokens belong to the single session. */
static void test_callback_fired_on_grace_expiry_to_online( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed, NULL };
  static const wifi_provisioning_controller_state_t expected[] = {
    WIFI_PROVISIONING_CONTROLLER_DISABLED,         WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
    WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
    WIFI_PROVISIONING_CONTROLLER_PROVISIONING,     WIFI_PROVISIONING_CONTROLLER_GRACE,
    WIFI_PROVISIONING_CONTROLLER_GRACE,            WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
    WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,      WIFI_PROVISIONING_CONTROLLER_ONLINE,
  };

  reset_mocks();
  reset_notify_records();
  init_with_recorder( &cfg );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );    /* PROVISIONING -> GRACE */
  fire_grace_expiry();                  /* GRACE -> RETIRING_AP */
  fire( WIFI_MGMT_EVENT_MODE_CHANGED ); /* RETIRING_AP -> ONLINE */
  assert_notify_sequence( expected, 5 );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
}

/* The grace-abort path is observable: a disconnect inside the grace window
 * moves GRACE -> PROVISIONING and keeps the portal open. */
static void test_callback_fired_on_grace_abort( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed, NULL };
  static const wifi_provisioning_controller_state_t expected[] = {
    WIFI_PROVISIONING_CONTROLLER_DISABLED,         WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
    WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
    WIFI_PROVISIONING_CONTROLLER_PROVISIONING,     WIFI_PROVISIONING_CONTROLLER_GRACE,
    WIFI_PROVISIONING_CONTROLLER_GRACE,            WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
  };

  reset_mocks();
  reset_notify_records();
  init_with_recorder( &cfg );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );   /* PROVISIONING -> GRACE */
  fire( WIFI_MGMT_EVENT_DISCONNECTED );/* GRACE -> PROVISIONING (abort) */
  assert_notify_sequence( expected, 4 );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_TRUE( wifi_provisioning_controller_is_provisioning() );
}

/* The online-loss path is observable: a transient disconnect from ONLINE
 * falls back to AWAITING_CONNECT without reopening the portal. */
static void test_callback_fired_on_online_loss( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed, NULL };
  static const wifi_provisioning_controller_state_t expected[] = {
    WIFI_PROVISIONING_CONTROLLER_DISABLED,         WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
    WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
    WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,      WIFI_PROVISIONING_CONTROLLER_ONLINE,
    WIFI_PROVISIONING_CONTROLLER_ONLINE,           WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
  };

  reset_mocks();
  s_has_saved_credentials = true;
  reset_notify_records();
  init_with_recorder( &cfg );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  fire( WIFI_MGMT_EVENT_CONNECTED );    /* AWAITING_CONNECT -> RETIRING_AP */
  fire( WIFI_MGMT_EVENT_MODE_CHANGED ); /* RETIRING_AP -> ONLINE */
  fire( WIFI_MGMT_EVENT_DISCONNECTED ); /* ONLINE -> AWAITING_CONNECT */
  assert_notify_sequence( expected, 4 );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_provision_start_count );
}

/* An explicit stop is observable: GRACE -> RETIRING_AP -> DISABLED once the
 * STA-only mode is confirmed. */
static void test_callback_fired_on_explicit_stop( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed, NULL };
  static const wifi_provisioning_controller_state_t expected[] = {
    WIFI_PROVISIONING_CONTROLLER_DISABLED,         WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
    WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
    WIFI_PROVISIONING_CONTROLLER_PROVISIONING,     WIFI_PROVISIONING_CONTROLLER_GRACE,
    WIFI_PROVISIONING_CONTROLLER_GRACE,            WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
    WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,      WIFI_PROVISIONING_CONTROLLER_DISABLED,
  };

  reset_mocks();
  reset_notify_records();
  init_with_recorder( &cfg );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );  /* PROVISIONING -> GRACE */
  TEST_ASSERT_TRUE( wifi_provisioning_controller_stop() );  /* GRACE -> RETIRING_AP */
  fire( WIFI_MGMT_EVENT_MODE_CHANGED ); /* RETIRING_AP -> DISABLED */
  assert_notify_sequence( expected, 5 );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );
}

/* The session/generation token lets a product discard notifications from a
 * prior controller lifecycle: after deinit/re-init every genuine notification
 * carries the fresh token, and a late straggler from an old lifecycle is
 * dropped by the product's session-gated filter. */
static void test_stale_session_notifications_discarded( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed_filter_session, NULL };
  uint32_t sess1, sess2, sess3;

  reset_mocks();
  reset_notify_records();
  init_with_recorder( &cfg );                     /* fresh lifecycle: D->AC, AC->P */
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING, s_product_state );
  TEST_ASSERT_EQUAL_INT( 2, s_notify_count );
  sess1 = s_notify_records[ s_notify_count - 1 ].session;
  TEST_ASSERT_EQUAL_UINT32( sess1, s_product_view_session );

  wifi_provisioning_controller_deinit();          /* DISABLED, new generation */
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED, s_product_state );
  TEST_ASSERT_EQUAL_INT( 3, s_notify_count );
  sess2 = s_notify_records[ s_notify_count - 1 ].session;
  TEST_ASSERT_TRUE( sess2 > sess1 );
  TEST_ASSERT_EQUAL_UINT32( sess2, s_product_view_session );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                     wifi_provisioning_controller_get_state() );

  init_with_recorder( &cfg );                     /* lifecycle 3: D->AC, AC->P */
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING, s_product_state );
  TEST_ASSERT_EQUAL_INT( 5, s_notify_count );
  sess3 = s_notify_records[ s_notify_count - 1 ].session;
  TEST_ASSERT_TRUE( sess3 > sess2 );
  TEST_ASSERT_EQUAL_UINT32( sess3, s_product_view_session );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );

  /* Genuine notifications across the three lifecycles: none was stale. */
  TEST_ASSERT_EQUAL_INT( 0, s_product_stale_count );

  /* A straggler from lifecycle #1 or #2 delivered after the re-init carries an
   * old token and must be discarded without touching the product mirror. */
  on_state_changed_filter_session( WIFI_PROVISIONING_CONTROLLER_GRACE,
                                   WIFI_PROVISIONING_CONTROLLER_RETIRING_AP, sess1, NULL );
  on_state_changed_filter_session( WIFI_PROVISIONING_CONTROLLER_GRACE,
                                   WIFI_PROVISIONING_CONTROLLER_RETIRING_AP, sess2, NULL );
  TEST_ASSERT_EQUAL_INT( 2, s_product_stale_count );
  TEST_ASSERT_EQUAL_INT( 5, s_notify_count );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING, s_product_state );
  TEST_ASSERT_EQUAL_UINT32( sess3, s_product_view_session );
}

/* A NULL hook (plain init() or a config with on_state_changed == NULL) is a
 * safe no-op: no callback is invoked anywhere in the lifecycle. */
static void test_null_callback_is_noop( void )
{
  wifi_provisioning_controller_config_t null_cfg = { NULL, NULL };

  /* Zero-arg init: full lifecycle with no notifications at all. */
  reset_mocks();
  reset_notify_records();
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  fire_grace_expiry();
  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_notify_count );

  /* Config-init with a NULL hook is equally a no-op. */
  reset_mocks();
  reset_notify_records();
  init_with_recorder( &null_cfg );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                     wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( 0, s_notify_count );
}

/* Re-entrancy: a product callback that queries get_state() must neither
 * deadlock nor panic; it must observe the exact state just entered. */
static void test_callback_queries_get_state_without_panic( void )
{
  wifi_provisioning_controller_config_t cfg = { on_state_changed_query_state, NULL };
  static const wifi_provisioning_controller_state_t expected[] = {
    WIFI_PROVISIONING_CONTROLLER_DISABLED,         WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
    WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
    WIFI_PROVISIONING_CONTROLLER_PROVISIONING,     WIFI_PROVISIONING_CONTROLLER_GRACE,
    WIFI_PROVISIONING_CONTROLLER_GRACE,            WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
    WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,      WIFI_PROVISIONING_CONTROLLER_ONLINE,
  };

  reset_mocks();
  reset_notify_records();
  init_with_recorder( &cfg );
  wifi_provisioning_controller_set_success_grace_ms( 200 );
  fire( WIFI_MGMT_EVENT_CONNECTED );
  fire_grace_expiry();
  fire( WIFI_MGMT_EVENT_MODE_CHANGED );
  assert_notify_sequence( expected, 5 );
  TEST_ASSERT_EQUAL( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                     wifi_provisioning_controller_get_state() );
}

/* Runner ------------------------------------------------------------------- */

static void run_controller_tests( void )
{
  RUN_TEST( test_no_saved_credentials_starts_once );
  RUN_TEST( test_saved_credentials_success_no_portal );
  RUN_TEST( test_transient_disconnect_does_not_start );
  RUN_TEST( test_exhausted_credentials_starts_once );
  RUN_TEST( test_fallback_reenables_after_deinit );
  RUN_TEST( test_fallback_budget_n_honored );
  RUN_TEST( test_fallback_budget_reset_on_success );
  RUN_TEST( test_fallback_budget_zero_disables_fallback );
  RUN_TEST( test_fallback_budget_default_backward_compat );
  RUN_TEST( test_init_deinit_idempotent_unsubscribes_all );
  RUN_TEST( test_grace_success_stops_after_expiry );
  RUN_TEST( test_zero_grace_shuts_down_immediately );
  RUN_TEST( test_failure_during_grace_keeps_portal );
  RUN_TEST( test_disconnect_during_grace_keeps_portal );
  RUN_TEST( test_disconnect_then_stale_expiry_is_ignored );
  RUN_TEST( test_explicit_stop_overrides_grace_timer );
  RUN_TEST( test_pre_override_is_armed );
  RUN_TEST( test_stop_failure_keeps_provisioning );
  RUN_TEST( test_mode_failure_keeps_provisioning );
  RUN_TEST( test_deinit_then_stale_expiry_is_ignored );
  RUN_TEST( test_stale_expiry_cannot_stop_new_session );
  RUN_TEST( test_callback_fired_on_init_and_fallback );
  RUN_TEST( test_callback_fired_on_grace_expiry_to_online );
  RUN_TEST( test_callback_fired_on_grace_abort );
  RUN_TEST( test_callback_fired_on_online_loss );
  RUN_TEST( test_callback_fired_on_explicit_stop );
  RUN_TEST( test_stale_session_notifications_discarded );
  RUN_TEST( test_null_callback_is_noop );
  RUN_TEST( test_callback_queries_get_state_without_panic );
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