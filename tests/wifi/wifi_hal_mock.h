/*
 * Wi-Fi HAL Mock — public control interface for unit tests
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
  uint32_t              init_count;   /**< Number of wifi_hal_init calls.   */
  uint32_t              deinit_count; /**< Number of wifi_hal_deinit calls. */

  uint32_t              scan_start_count; /**< Number of wifi_hal_start_scan calls.        */
  uint32_t              connect_count;     /**< Number of wifi_hal_connect calls (entered gen). */

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

/* Reset all mock state and drain pending notifications.
 *
 * This is a fixture-owner operation and must not race another mock control or
 * wait API.  It returns false, without changing state or consuming tokens,
 * while any init, stop, or deinit invocation, or any held connect/mode-start
 * round, is active or parked.  A successful reset clears state and drains all
 * existing lifecycle, connection, mode-start, GOT_IP, and SCAN_DONE
 * notification/release tokens while holding the mock mutex, before allowing
 * any later entry to publish a token.
 */
bool wifi_hal_mock_reset( void );

/* Configure the return value of wifi_hal_connect(). */
void wifi_hal_mock_set_connect_result( osal_status_t result );

/* Configure the return value of wifi_hal_start(). */
void wifi_hal_mock_set_start_result( osal_status_t result );

/* Configure the return value of wifi_hal_init(). Default is OSAL_SUCCESS. */
void wifi_hal_mock_set_init_result( osal_status_t result );

/* Configure the return value of wifi_hal_stop(). Default is OSAL_SUCCESS. */
void wifi_hal_mock_set_stop_result( osal_status_t result );

/* Configure the return value of wifi_hal_deinit(). Default is OSAL_SUCCESS. */
void wifi_hal_mock_set_deinit_result( osal_status_t result );

/* Set the predefined AP list returned by wifi_hal_get_scanned_ap(). */
void wifi_hal_mock_set_scan_list( const wifi_hal_ap_record_t* list, uint16_t count );

/* Set the IP info returned by wifi_hal_get_sta_ip_info(). */
void wifi_hal_mock_set_ip_info( const wifi_hal_ip_info_t* info );

/* Fire an event through the registered callback (simulates HAL events). */
void wifi_hal_mock_inject_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data );

/* When @p hold is true, wifi_hal_start_scan() does NOT fire SCAN_DONE
 * immediately so tests can observe the in-flight scan window before manually
 * completing it via inject_event(WIFI_HAL_EVT_SCAN_DONE). */
void wifi_hal_mock_set_scan_done_hold( bool hold );

/* Per-invocation init hold controls.
 *
 * The hold decision is captured at the invocation's entry, serialized with
 * init entry by the mock mutex.  release_init_hold() releases exactly one
 * already-parked wifi_hal_init() without touching the hold; a release made
 * before an invocation parks becomes no credit for a later round.
 * set_init_hold(false) disables the hold and releases the currently parked
 * invocation (if any). */
void wifi_hal_mock_set_init_hold( bool hold );

void wifi_hal_mock_release_init_hold( void );

/* Per-invocation deinit hold, same contract as the init hold above. */
void wifi_hal_mock_set_deinit_hold( bool hold );

void wifi_hal_mock_release_deinit_hold( void );

/* Consume one entered notification for wifi_hal_init(), or return false when
 * the timeout elapses.  This compatibility API is intentionally one-shot:
 * each success consumes exactly one token, including when timeout_ms is
 * zero.  New generation-checked waits are exposed separately (see the
 * wifi_hal_mock_wait_*_level() family below) and do not change its
 * semantics. */
bool wifi_hal_mock_wait_init_entered( uint32_t timeout_ms );

/* Lifecycle generation counters.
 *
 * Every accepted lifecycle invocation (a wifi_hal_init/stop/deinit call that
 * has passed parameter validation and the entry boundary) advances the entered
 * generation counter (which is the same value as the corresponding attempt
 * counter) and, once the mock state effects and configured result are final,
 * the completed generation counter.  Both counters therefore share one entry
 * boundary, including for attempts that return a configured HAL failure. */
uint32_t wifi_hal_mock_get_init_entered_count( void );
uint32_t wifi_hal_mock_get_init_completed_count( void );
uint32_t wifi_hal_mock_get_stop_entered_count( void );
uint32_t wifi_hal_mock_get_stop_completed_count( void );
uint32_t wifi_hal_mock_get_deinit_entered_count( void );
uint32_t wifi_hal_mock_get_deinit_completed_count( void );

/* Generation-checked lifecycle waits.
 *
 * @p level is the requested lifecycle attempt (1 = first invocation).  A wait
 * first inspects the corresponding generation counter under the mock mutex and
 * treats semaphore tokens only as wake hints.  After every wake, including a
 * stale success from an earlier round, it re-checks the counter and charges
 * the elapsed wall-clock time against one wrap-safe timeout budget.  A stale
 * token left over from an earlier round can therefore never satisfy a wait for
 * a later one. */
bool wifi_hal_mock_wait_init_entered_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_init_completed_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_stop_entered_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_stop_completed_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_deinit_entered_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_deinit_completed_level( uint32_t level, uint32_t timeout_ms );

/* Focused lifecycle snapshot.
 *
 * This is the race-free lifecycle observation surface consumed by the TASK-135
 * stop tests.  It deliberately exposes only booleans and counters needed to
 * synchronize a worker with an init/start/stop/deinit outcome: no callback
 * pointer values are copied out, and the helper never returns the mutable
 * internal address.  Every field is read (and written by lifecycle workers)
 * under the mock mutex. */
typedef struct
{
  bool     initialized;              /**< wifi_hal_init succeeded.          */
  bool     started;                  /**< wifi_hal_start succeeded.         */
  bool     connected;                /**< wifi_hal_connect succeeded.       */

  uint32_t init_count;               /**< wifi_hal_init attempts (entered). */
  uint32_t start_count;              /**< wifi_hal_start attempts.          */
  uint32_t stop_count;               /**< wifi_hal_stop attempts (entered). */
  uint32_t deinit_count;             /**< wifi_hal_deinit attempts (entered). */

  uint32_t init_entered_gen;         /**< Init entered lifecycle generation.   */
  uint32_t init_completed_gen;       /**< Init completed lifecycle generation. */
  uint32_t stop_entered_gen;         /**< Stop entered lifecycle generation.   */
  uint32_t stop_completed_gen;       /**< Stop completed lifecycle generation. */
  uint32_t deinit_entered_gen;       /**< Deinit entered lifecycle generation. */
  uint32_t deinit_completed_gen;     /**< Deinit completed generation.         */

  bool     event_cb_registered;      /**< Callback registration indicator.    */
  bool     user_data_registered;     /**< User-data registration indicator.   */
} wifi_hal_mock_lifecycle_t;

/* Copy a synchronized lifecycle snapshot into @p out.
 *
 * For any non-NULL @p out the entire destination is zeroed before the mock
 * mutex is taken; on lock failure the (zeroed) destination is left untouched
 * and false is returned.  On success the snapshot fields are copied under the
 * mock mutex, so a counter/outcome reader never races a lifecycle worker, and
 * true is returned.  This API never returns the mutable global address. */
bool wifi_hal_mock_get_lifecycle( wifi_hal_mock_lifecycle_t* out );

/* Number of HAL events actually delivered through a registered callback.
 *
 * The counter is incremented synchronously by wifi_hal_mock_inject_event() on
 * the caller's thread whenever a callback is present, so it is observable
 * immediately after inject_event returns.  The TASK-135C lifecycle tests use it
 * to prove that an event injected after a successful deinit cannot reach the
 * management layer (the HAL callback is dropped during deinit). */
uint32_t wifi_hal_mock_get_delivered_event_count( void );

/* Full synchronized state snapshot.
 *
 * Copies the complete mock state (@p sta_cfg, @p ap_cfg, DNS fields, scan
 * records, IP information, power-save state, every counter, mode, connection
 * state, configured results, and the registered callback/user-data pointers)
 * into @p out.  For any non-NULL @p out the entire destination is zeroed before
 * the mock mutex is taken; on lock failure the (zeroed) destination is left
 * untouched and false is returned.  On success the fields are copied under the
 * mock mutex, so a reader never races a worker, and true is returned.  This API
 * never returns the mutable global address, so no caller can hold a pointer
 * into the internal state while a worker writes it. */
bool wifi_hal_mock_get_state( wifi_hal_mock_state_t* out );

/* Connection, mode-start, GOT_IP, and SCAN_DONE notification channels.
 *
 * Each channel advances an attempt/delivery generation counter and publishes a
 * binary-semaphore token as a wake hint only.  The generation-checked waits
 * below follow the same contract as the lifecycle waits: they first inspect the
 * counter under the mock mutex, treat every semaphore success (including a
 * stale one from an earlier round) as a hint, re-check the counter, and charge
 * the elapsed wall-clock time against one wrap-safe timeout budget.  A stale
 * token can therefore never satisfy a wait for a later generation. */

/* Connect-call channel: wifi_hal_connect() entry (attempt) and completion
 * (result applied).  The entered generation is the attempt counter exposed as
 * g_mock.connect_count / get_connect_call_count(). */
uint32_t wifi_hal_mock_get_connect_call_count( void );
uint32_t wifi_hal_mock_get_connect_completed_count( void );
bool wifi_hal_mock_wait_connect_call_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_connect_completed_level( uint32_t level, uint32_t timeout_ms );

/* Per-invocation connect hold, same contract as the init hold: the hold
 * decision is captured at the invocation's entry; release_connect_hold()
 * releases exactly one already-parked wifi_hal_connect() without touching the
 * hold, and set_connect_hold(false) disables the hold and releases the
 * currently parked invocation (if any). */
void wifi_hal_mock_set_connect_hold( bool hold );
void wifi_hal_mock_release_connect_hold( void );

/* Mode-start channel: wifi_hal_start() entry (attempt) and completion (mode
 * applied).  The entered generation is g_mock.start_count. */
uint32_t wifi_hal_mock_get_start_entered_count( void );
uint32_t wifi_hal_mock_get_start_completed_count( void );
bool wifi_hal_mock_wait_start_entered_level( uint32_t level, uint32_t timeout_ms );
bool wifi_hal_mock_wait_start_completed_level( uint32_t level, uint32_t timeout_ms );

/* Per-invocation mode-start hold, same contract as the connect hold above. */
void wifi_hal_mock_set_start_hold( bool hold );
void wifi_hal_mock_release_start_hold( void );

/* GOT_IP delivery channel: increments when WIFI_HAL_EVT_STA_GOT_IP is actually
 * delivered through a registered callback (by inject_event). */
uint32_t wifi_hal_mock_get_got_ip_delivered_count( void );
bool wifi_hal_mock_wait_got_ip_delivered_level( uint32_t level, uint32_t timeout_ms );

/* When @p hold is true, inject_event(WIFI_HAL_EVT_STA_GOT_IP) is withheld: the
 * callback is not invoked and the GOT_IP delivery generation is not advanced.
 * Clearing the hold does not retroactively deliver; the test re-injects to
 * complete delivery.  Mirrors the scan_done hold contract. */
void wifi_hal_mock_set_got_ip_hold( bool hold );

/* SCAN_DONE delivery channel: increments whenever WIFI_HAL_EVT_SCAN_DONE is
 * actually delivered through a registered callback — either the automatic
 * completion fired by wifi_hal_start_scan() or an inject_event(SCAN_DONE).  The
 * scan_done hold (wifi_hal_mock_set_scan_done_hold) suppresses the automatic
 * completion so tests can complete the scan manually. */
uint32_t wifi_hal_mock_get_scan_done_delivered_count( void );
bool wifi_hal_mock_wait_scan_done_delivered_level( uint32_t level, uint32_t timeout_ms );

#endif
