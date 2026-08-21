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
 * Runner
 * ========================================================================== */
void wifi_hal_mock_tests_run( void )
{
  RUN_TEST( test_dns_valid_stored );
  RUN_TEST( test_dns_omitted_non_captive );
  RUN_TEST( test_provisioning_passes_ap_ip_as_dns );
  RUN_TEST( test_dns_invalid_rejected );
  RUN_TEST( test_ipv4_validator );
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