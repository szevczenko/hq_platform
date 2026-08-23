/*
 * Wi-Fi HAL Mock — public control interface for unit tests
 *
 * TASK-134B adds deterministic lifecycle synchronization controls on top of the
 * TASK-134 readiness barrier:
 *   - acknowledged, per-invocation hold/release barriers for wifi_hal_init()
 *     and wifi_hal_deinit(); a test can release exactly one held deinit call
 *     while the next call stays held,
 *   - 1-based entered/completed generation notifications for HAL init, stop and
 *     deinit, so a test can distinguish a later lifecycle round from a stale
 *     notification token left over from an earlier round,
 *   - synchronized lifecycle-result setters and snapshot/counter getters so
 *     lifecycle result and counter assertions never race the Wi-Fi worker,
 *   - reset semantics that drain stale notifications and never clear shared
 *     state while a held HAL call is active.
 */

#ifndef WIFI_HAL_MOCK_H
#define WIFI_HAL_MOCK_H

#include "wifi_hal_driver.h"

#define WIFI_HAL_MOCK_MAX_AP 32

typedef struct
{
  bool                  initialized;
  bool                  started;
  bool                  connected;
  bool                  power_save;
  wifi_hal_mode_t       mode;

  uint32_t              start_count;
  uint32_t              stop_count;
  /* Each counter is incremented when the corresponding HAL invocation is
   * ENTERED (under the mock lock), so an acknowledged invocation that is parked
   * on a hold barrier is already visible to the counter API even before it
   * completes; the "not completed" signal for a held call is the matching
   * completed generation.  TASK-135 counter assertions therefore never disagree
   * with an entered-generation notification. */
  uint32_t              init_count;   /**< Number of entered wifi_hal_init calls.   */
  uint32_t              deinit_count; /**< Number of entered wifi_hal_deinit calls. */

  uint32_t              scan_start_count; /**< Number of wifi_hal_start_scan calls. */

  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t  ap_cfg;

  char                  ap_dns[16]; /**< DNS advertised by AP DHCP (empty when omitted). */
  bool                  ap_dns_set; /**< True when a non-empty ap_dns was configured.    */

  wifi_hal_ap_record_t  scan_list[WIFI_HAL_MOCK_MAX_AP];
  uint16_t              scan_count;

  wifi_hal_ip_info_t    ip_info;

  osal_status_t         connect_result;
  osal_status_t         start_result;

  wifi_hal_event_cb_t   event_cb;
  void*                 user_data;
} wifi_hal_mock_state_t;

/* Synchronized lifecycle snapshot.  Every field is copied while holding the
 * mock state lock, so assertions never race the Wi-Fi worker while it writes
 * the same lifecycle fields inside wifi_hal_init/deinit/start/stop.  Only the
 * fields consumed by TASK-135 tests are exposed; the complete mock snapshot
 * conversion stays in TASK-139. */
typedef struct
{
  bool                initialized;
  bool                started;

  /* Counters count invocations ENTERED (attempts), incremented under the mock
   * lock; a held init/deinit attempt is already counted, so the counter never
   * disagrees with the entered-generation notification. */
  uint32_t            init_count;   /**< Number of entered wifi_hal_init calls.   */
  uint32_t            deinit_count; /**< Number of entered wifi_hal_deinit calls. */
  uint32_t            stop_count;   /**< Number of entered wifi_hal_stop calls.   */
  uint32_t            start_count;  /**< Number of entered wifi_hal_start calls.  */

  /* 1-based lifecycle round generations.  Each wifi_hal_init/deinit/stop
   * invocation increments its own "entered" and "completed" generation and
   * wakes the matching notification semaphore.  Generation-based waits let
   * tests distinguish a later lifecycle round from a stale notification token
   * left over from an earlier round. */
  uint32_t            init_entered_gen;
  uint32_t            init_completed_gen;
  uint32_t            stop_entered_gen;
  uint32_t            stop_completed_gen;
  uint32_t            deinit_entered_gen;
  uint32_t            deinit_completed_gen;

  /* Event callback/user data copied under the mock lock.  The Wi-Fi worker may
   * be clearing them concurrently inside wifi_hal_deinit, so callers must not
   * read the raw mock state for callback delivery. */
  wifi_hal_event_cb_t event_cb;
  void*               user_data;
} wifi_hal_mock_lifecycle_snapshot_t;

/* Reset all mock state to defaults.  When the mock is quiescent, drains every
 * pending lifecycle notification token so a stale token from a previous round
 * can never be mistaken for a fresh entry/completion.  While a held init/deinit
 * call is parked on a barrier (or still executing inside the HAL function), the
 * shared mock state, generations, current notifications, and barrier release
 * grants are NOT cleared; the test can still release and wait for that same
 * invocation.  A later reset, after the call exits, drains the accumulated
 * notifications. */
void wifi_hal_mock_reset( void );

/* Configure the return value of wifi_hal_connect(). */
void wifi_hal_mock_set_connect_result( osal_status_t result );

/* Configure the return value of wifi_hal_start(). */
void wifi_hal_mock_set_start_result( osal_status_t result );

/* Synchronized lifecycle-result setters (default OSAL_SUCCESS).  The values are
 * read inside wifi_hal_init()/wifi_hal_deinit() under the mock lock, so the
 * configured result is always observed by the worker. */
void wifi_hal_mock_set_init_result( osal_status_t result );
void wifi_hal_mock_set_deinit_result( osal_status_t result );

/* Set the predefined AP list returned by wifi_hal_get_scanned_ap(). */
void wifi_hal_mock_set_scan_list( const wifi_hal_ap_record_t* list, uint16_t count );

/* Set the IP info returned by wifi_hal_get_sta_ip_info(). */
void wifi_hal_mock_set_ip_info( const wifi_hal_ip_info_t* info );

/* Fire an event through the registered callback (simulates HAL events).
 * The callback pointer is copied under the mock lock and invoked only after
 * the lock has been released, so a concurrent deinit that clears the callback
 * cannot race the invocation. */
void wifi_hal_mock_inject_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data );

/* When @p hold is true, wifi_hal_start_scan() does NOT fire SCAN_DONE
 * immediately so tests can observe the in-flight scan window before manually
 * completing it via inject_event(WIFI_HAL_EVT_SCAN_DONE).  The hold flag is
 * read under the mock lock inside wifi_hal_start_scan(). */
void wifi_hal_mock_set_scan_done_hold( bool hold );

/* Per-invocation hold/release controls.
 *
 * When hold is enabled every wifi_hal_init()/wifi_hal_deinit() call parks on a
 * release barrier after raising its "entered" notification.  The barrier is
 * implemented with synchronized test primitives (no busy boolean loops): the
 * parked call consumes exactly one release grant, so
 *
 *   - set_*_hold( false ) releases every call currently parked at the barrier
 *     (and disables the hold), while two concurrently parked calls both get
 *     their own grant;
 *   - release_*_hold() releases exactly one parked call and leaves the hold
 *     state untouched, so the next call parks again until it receives its own
 *     grant.
 *
 * A release grant can never outlive its parked call, so a grant from one round
 * can never release a later round.
 */
void wifi_hal_mock_set_init_hold( bool hold );
void wifi_hal_mock_set_deinit_hold( bool hold );
void wifi_hal_mock_release_init_hold( void );
void wifi_hal_mock_release_deinit_hold( void );

/* TASK-134 compatibility: consume one entered notification for
 * wifi_hal_init(), or return false when the timeout elapses.  This intentionally
 * preserves the original one-shot/token-consuming semantics, including a
 * timeout of zero for draining stale acknowledgements.  Use the generation API
 * below when a wait must identify a particular lifecycle round.
 * @return true if one init-entered notification was consumed, false on timeout. */
bool wifi_hal_mock_wait_init_entered( uint32_t timeout_ms );

/* Generation-based lifecycle notifications.
 *
 * @p gen is the 1-based lifecycle round number of the HAL invocation (the
 * first wifi_hal_deinit call of a run is round 1, the next is round 2, ...).
 * The call returns true once the mock has observed that HAL invocation
 * entering (or completing) at least round @p gen, and false on timeout.
 *
 * Because the wait is generation-checked under the mock lock a stale
 * notification token left over from an earlier round can never satisfy a wait
 * for a later round. */
bool wifi_hal_mock_wait_init_entered_gen( uint32_t gen, uint32_t timeout_ms );
bool wifi_hal_mock_wait_init_completed_gen( uint32_t gen, uint32_t timeout_ms );
bool wifi_hal_mock_wait_stop_entered_gen( uint32_t gen, uint32_t timeout_ms );
bool wifi_hal_mock_wait_stop_completed_gen( uint32_t gen, uint32_t timeout_ms );
bool wifi_hal_mock_wait_deinit_entered_gen( uint32_t gen, uint32_t timeout_ms );
bool wifi_hal_mock_wait_deinit_completed_gen( uint32_t gen, uint32_t timeout_ms );

/* Copy the lifecycle snapshot under the mock lock.  Safe to call while the
 * Wi-Fi worker is inside any HAL lifecycle function. */
void wifi_hal_mock_get_lifecycle_snapshot( wifi_hal_mock_lifecycle_snapshot_t* out );

/* Synchronized lifecycle counter getters (no race with the Wi-Fi worker). */
uint32_t wifi_hal_mock_get_init_count( void );
uint32_t wifi_hal_mock_get_deinit_count( void );
uint32_t wifi_hal_mock_get_stop_count( void );

/* Get read-only pointer to internal mock state for assertions.
 * NOTE: this is the raw, unsynchronized mock state (legacy accessor used by
 * pre-TASK-134B tests).  New tests must use wifi_hal_mock_get_lifecycle_snapshot()
 * so counter/result assertions do not race the Wi-Fi worker. */
const wifi_hal_mock_state_t* wifi_hal_mock_get_state( void );

#endif