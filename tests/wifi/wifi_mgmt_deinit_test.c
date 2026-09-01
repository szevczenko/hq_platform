/*
 * Wi-Fi Management Final Deinitialization Unit Tests (TASK-135B)
 *
 * Exercises the owner-driven, bounded, retryable teardown that joins the Wi-Fi
 * worker and releases every management synchronization object.  The tests run
 * into isolation: each builds its own running module, deinitializes it, and
 * leaves the module fully uninitialized.
 *
 * Covered contracts:
 *   - deinit-before-init returns true and repeats are idempotent,
 *   - init -> deinit returns true and the HAL teardown is acknowledged,
 *   - deinit/deinit after a successful deinit returns true,
 *   - a full init -> stop-free deinit -> reinit cycle leaves no live worker and
 *     remains fully restartable,
 *   - a worker-owned stop failure makes deinit return false (state retained)
 *     and a retry after the failure is repaired succeeds.
 */

#include <stdio.h>
#include <string.h>

#include "wifi_managment.h"
#include "wifi_hal_mock.h"
#include "osal_task.h"
#include "unity.h"

/* Establish a cleanly deinitialized baseline even if another test target left
 * the module live. */
static void _deinit_clean( void )
{
  ( void ) wifi_mgmt_deinit();
}

/* Build a running module: mock reset, type, init, start, await IDLE. */
static void _deinit_build_running( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock reset succeeds" );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLIENT );
  wifi_mgmt_init();
  wifi_mgmt_start();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( 3000 ), "module ready" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(), "module running" );
}

/* ============================================================================
 * deinit-before-init and repeat deinit are idempotent with explicit results.
 * ========================================================================== */
static void test_deinit_before_init_repeats( void )
{
  /* The runner established a clean, fully deinitialized baseline. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "deinit before init returns true" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "repeat deinit stays true" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "triple deinit stays true" );
}

/* ============================================================================
 * A live module is torn down: HAL teardown is acknowledged, the worker is
 * joined, and a repeat deinit after success is an idempotent true.
 * ========================================================================== */
static void test_deinit_after_start_cycle( void )
{
  _deinit_clean();
  _deinit_build_running();

  wifi_hal_mock_lifecycle_t before = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &before ),
                            "lifecycle snapshot before deinit" );

  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "deinit of a running module succeeds" );

  wifi_hal_mock_lifecycle_t after = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_lifecycle( &after ),
                            "lifecycle snapshot after deinit" );
  TEST_ASSERT_GREATER_THAN_MESSAGE( before.deinit_count, after.deinit_count,
                                    "HAL deinitialized during deinit" );

  /* deinit/deinit are idempotent after a successful deinit. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "post-deinit repeat is true" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "post-deinit repeat is true" );
}

/* ============================================================================
 * A full init -> deinit -> re-init cycle succeeds: no worker or object is left
 * behind, and the module is fully restartable after a deinit.
 * ========================================================================== */
static void test_deinit_reinit_full_cycle( void )
{
  _deinit_clean();

  _deinit_build_running();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "first module deinitialized" );

  /* Build a fresh module from scratch: proves the previous worker/objects were
   * fully released and a new worker can be created. */
  _deinit_build_running();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(), "fresh module running" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "second module deinitialized" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "repeat over clean module is true" );
}

/* ============================================================================
 * A worker stop failure surfaces as a deinit false (state retained for retry);
 * once the worker stop is repaired a serialized deinit retry succeeds.
 * ========================================================================== */
static void test_deinit_stop_failure_retry( void )
{
  _deinit_clean();
  _deinit_build_running();

  /* Worker-owned HAL stop fails; deinit must retain everything and return false. */
  wifi_hal_mock_set_stop_result( OSAL_ERROR );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_deinit(),
                             "deinit is false when the worker stop errors" );

  /* Repair the stop and retry the same teardown serially. */
  wifi_hal_mock_set_stop_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "deinit retry succeeds after repair" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "post-retry deinit is idempotent" );
}

/* ============================================================================
 * HAL deinit failure on the stop round makes deinit false; a healthy retry
 * succeeds and leaves the module clean.
 * ========================================================================== */
static void test_deinit_hal_deinit_failure_retry( void )
{
  _deinit_clean();
  _deinit_build_running();

  wifi_hal_mock_set_deinit_result( OSAL_ERROR );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_deinit(),
                             "deinit is false when the worker deinit errors" );

  wifi_hal_mock_set_deinit_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "deinit retry succeeds after repair" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_deinit(), "post-retry deinit is idempotent" );
}

/* ============================================================================
 * Runner
 * ========================================================================== */
void wifi_mgmt_deinit_tests_run( void )
{
  /* A prior test target may have left the module live; start clean. */
  _deinit_clean();

  RUN_TEST( test_deinit_before_init_repeats );
  RUN_TEST( test_deinit_after_start_cycle );
  RUN_TEST( test_deinit_reinit_full_cycle );
  RUN_TEST( test_deinit_stop_failure_retry );
  RUN_TEST( test_deinit_hal_deinit_failure_retry );

  /* Leave the module cleanly deinitialized for any later test target. */
  _deinit_clean();
}