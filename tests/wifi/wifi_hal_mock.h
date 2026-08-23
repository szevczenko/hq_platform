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

/* Reset all mock state and drain pending notifications.
 *
 * This is a fixture-owner operation and must not race another mock control or
 * wait API.  It returns false, without changing state or consuming tokens,
 * when wifi_hal_init() is active or parked.  A successful reset clears state
 * and drains all existing notification/release tokens while holding the mock
 * mutex, before allowing a later init to publish a token.
 */
bool wifi_hal_mock_reset( void );

/* Configure the return value of wifi_hal_connect(). */
void wifi_hal_mock_set_connect_result( osal_status_t result );

/* Configure the return value of wifi_hal_start(). */
void wifi_hal_mock_set_start_result( osal_status_t result );

/* Configure the return value of wifi_hal_init(). Default is OSAL_SUCCESS. */
void wifi_hal_mock_set_init_result( osal_status_t result );

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

/* Acknowledged init hold/release.  When @p hold is true, a later
 * wifi_hal_init() publishes its entered acknowledgement and then parks on an
 * OSAL semaphore.  Setting it false releases the parked invocation.  The hold
 * decision and release are serialized with init entry by the mock mutex. */
void wifi_hal_mock_set_init_hold( bool hold );

/* Consume one entered notification for wifi_hal_init(), or return false when
 * the timeout elapses.  This compatibility API is intentionally one-shot:
 * each successful wait consumes exactly one acknowledgement, including when
 * timeout_ms is zero. */
bool wifi_hal_mock_wait_init_entered( uint32_t timeout_ms );

/* Get read-only pointer to internal mock state for assertions. */
const wifi_hal_mock_state_t* wifi_hal_mock_get_state( void );

#endif
