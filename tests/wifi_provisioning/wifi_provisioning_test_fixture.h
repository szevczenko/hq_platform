/*
 * Shared OSAL-backed filesystem + component-ownership fixture for the POSIX
 * Wi-Fi HTTP provisioning integration-style tests (TASK-142).
 *
 * Every integration-style provisioning executable mounts its own unique
 * temporary littlefs image, installed through
 * wifi_provisioning_test_fixture_configure() before any test runs.  A
 * parallel or failed CTest run therefore can never contaminate another test's
 * filesystem state, and no shared wifi_ap.json or fixed shared image survives
 * a successful test.
 *
 * The fixture owns the whole component stack in the documented ownership order
 * (OSAL -> Mongoose -> Wi-Fi management -> provisioning) and tears it down in
 * exactly the reverse order, removing the temporary image only after the
 * Wi-Fi/configuration workers have been stopped and joined.  It is used from a
 * test's Unity setUp()/tearDown() hooks.
 */

#ifndef WIFI_PROVISIONING_TEST_FIXTURE_H_
#define WIFI_PROVISIONING_TEST_FIXTURE_H_

#include <stdbool.h>
#include <stdint.h>

/* LittleFS layout installed by the fixture. */
#define WIFI_PROVISIONING_TEST_FIXTURE_BLOCK_SIZE   4096u
#define WIFI_PROVISIONING_TEST_FIXTURE_BLOCK_COUNT  256u

/* Mount point used by every provisioning test that installs the image.  An
 * empty mount is harmless on targets where the platform provides no OSAL
 * littlefs; on POSIX the dedicated image is mounted at this path. */
#define WIFI_PROVISIONING_TEST_FIXTURE_MOUNT_POINT  "/"

/* Install the unique per-executable temporary image path.  Must be called once
 * (typically at the top of main(), before UNITY_BEGIN()) so setUp() can create
 * and mount this executable's own image.  Passing NULL keeps the default. */
void wifi_provisioning_test_fixture_configure( const char * image_path );

/* Bring the whole provisioning stack up in dependency order: create + mount a
 * fresh unique image, reseed the mock HAL (when linked), initialize Wi-Fi
 * management, start it and synchronize on readiness, then bring up the shared
 * Mongoose process.  Intended to be called from a test's Unity setUp(). */
void wifi_provisioning_test_fixture_setup( void );

/* Reverse-order teardown: stop the provisioning listeners, stop and
 * deinitialize the Wi-Fi management worker, deinitialize the shared Mongoose
 * process, and only then remove the unique image.  Intended to be called from
 * a test's Unity tearDown(). */
void wifi_provisioning_test_fixture_teardown( void );

/* Return the installed unique image path (for diagnostics). */
const char * wifi_provisioning_test_fixture_image_path( void );

/* Return the mount point used by the fixture. */
const char * wifi_provisioning_test_fixture_mount_point( void );

/* Wait for the Wi-Fi management worker to report ready (readiness
 * synchronization; never a fixed wall-clock sleep). */
bool wifi_provisioning_test_fixture_wait_wifi_ready( uint32_t timeout_ms );

#endif /* WIFI_PROVISIONING_TEST_FIXTURE_H_ */