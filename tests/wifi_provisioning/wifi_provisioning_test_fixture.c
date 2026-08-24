/*
 * Shared OSAL-backed filesystem + component-ownership fixture for the POSIX
 * Wi-Fi HTTP provisioning integration-style tests (TASK-142).
 *
 * Each executable that links this source installs its own unique temporary
 * littlefs image (see wifi_provisioning_test_fixture_configure), so a parallel
 * or failed CTest run can never contaminate another test as state, and no
 * fixed wifi_ap.json or fixed shared image survives a successful test.
 *
 * The fixture brings the component stack up in dependency order (filesystem ->
 * Wi-Fi management -> shared Mongoose) and tears it down in exactly the reverse
 * order (provisioning -> Wi-Fi management -> Mongoose -> filesystem), removing
 * the temporary image only after the Wi-Fi/configuration worker is stopped and
 * joined.
 */

#include <string.h>

#include "mongoose_process.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "unity.h"
#include "wifi_config.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"

#ifdef WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
#include "wifi_hal_mock.h"
#endif

#include "wifi_provisioning_test_fixture.h"

/* Sanity bound for the readiness sync.  The Wi-Fi worker publishes readiness
 * through a semaphore, so this is only an upper bound on a deadlocked worker,
 * never a fixed wait. */
#define WIFI_PROVISIONING_TEST_FIXTURE_READY_TIMEOUT_MS 4000u

#ifndef WIFI_PROVISIONING_TEST_FIXTURE_DEFAULT_IMAGE
#define WIFI_PROVISIONING_TEST_FIXTURE_DEFAULT_IMAGE "/tmp/wifi_provisioning_test.img"
#endif

static char s_image_path[OSAL_MAX_PATH_LEN] = WIFI_PROVISIONING_TEST_FIXTURE_DEFAULT_IMAGE;

void wifi_provisioning_test_fixture_configure( const char * image_path )
{
  if ( image_path == NULL || image_path[0] == '\0' )
  {
    return;
  }

  size_t n = strlen( image_path );
  if ( n >= sizeof( s_image_path ) )
  {
    n = sizeof( s_image_path ) - 1u;
  }
  memcpy( s_image_path, image_path, n );
  s_image_path[n] = '\0';
}

const char * wifi_provisioning_test_fixture_image_path( void )
{
  return s_image_path;
}

const char * wifi_provisioning_test_fixture_mount_point( void )
{
  return WIFI_PROVISIONING_TEST_FIXTURE_MOUNT_POINT;
}

bool wifi_provisioning_test_fixture_wait_wifi_ready( uint32_t timeout_ms )
{
  return wifi_mgmt_wait_ready( timeout_ms );
}

/* Create + mount a fresh unique image and drop any resurrected config left on
 * it (e.g. from a prior crashed run of this same executable). */
static void fixture_setup_fs( void )
{
  ( void ) osal_unmount( WIFI_PROVISIONING_TEST_FIXTURE_MOUNT_POINT );
  ( void ) osal_rmfs( s_image_path );
  ( void ) osal_mkfs( NULL, s_image_path, WIFI_PROVISIONING_TEST_FIXTURE_MOUNT_POINT,
                      WIFI_PROVISIONING_TEST_FIXTURE_BLOCK_SIZE,
                      WIFI_PROVISIONING_TEST_FIXTURE_BLOCK_COUNT );
  ( void ) osal_mount( s_image_path, WIFI_PROVISIONING_TEST_FIXTURE_MOUNT_POINT );
  ( void ) osal_remove( WIFI_CONFIG_FILE_PATH );
}

/* Remove the mounted config and the unique image.  Only called after the last
 * worker that can write the image (Wi-Fi/configuration) has been joined. */
static void fixture_cleanup_fs( void )
{
  ( void ) osal_remove( WIFI_CONFIG_FILE_PATH );
  ( void ) osal_unmount( WIFI_PROVISIONING_TEST_FIXTURE_MOUNT_POINT );
  ( void ) osal_rmfs( s_image_path );
}

void wifi_provisioning_test_fixture_setup( void )
{
  fixture_setup_fs();

#ifdef WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
  /* Reseed the mock HAL so each isolated run starts deterministic: no held
   * invocation, no surviving lifecycle counters, and start/connect succeed. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock HAL must reset in setUp" );
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );
#endif

  /* Clean slate for the shared process, then bring the stack up in ownership
   * order: Wi-Fi management must be running before provisioning, and the
   * shared Mongoose process must be up before provisioning can bind. */
  MongooseProcess_Deinit();
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  wifi_mgmt_init();
  wifi_mgmt_start();

  /* Readiness synchronization: wait on the worker's ready signal instead of a
   * fixed polling loop, so the setup cost is proportional to the host and is
   * safe under parallel CTest scheduling. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( WIFI_PROVISIONING_TEST_FIXTURE_READY_TIMEOUT_MS ),
                            "Wi-Fi management worker must report ready" );

  MongooseProcess_Init();
  TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
}

void wifi_provisioning_test_fixture_teardown( void )
{
  /* Release any deliberately held mock invocation so the Wi-Fi worker, its
   * timers and Mongoose callbacks can all drain before teardown. */
#ifdef WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
  wifi_hal_mock_set_init_hold( false );
  wifi_hal_mock_set_start_hold( false );
  wifi_hal_mock_set_connect_hold( false );
  wifi_hal_mock_set_deinit_hold( false );
  wifi_hal_mock_set_got_ip_hold( false );
  wifi_hal_mock_set_scan_done_hold( false );
#endif

  /* Reverse ownership order.  The provisioning listeners close while the shared
   * Mongoose process is still alive, then Wi-Fi is stopped and joined, then the
   * shared process is deinitialized, and only then is the unique image removed. */
  ( void ) wifi_http_provisioning_stop();
  ( void ) wifi_mgmt_stop();
  ( void ) wifi_mgmt_deinit();
  MongooseProcess_Deinit();

  fixture_cleanup_fs();
}