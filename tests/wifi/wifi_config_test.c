/*
 * Wi-Fi Config Unit Tests
 *
 * Tests:
 * 1.  Add single credential to empty list
 * 2.  Add duplicate SSID updates password and nb
 * 3.  Fill list to max capacity
 * 4.  Evict oldest entry when list is full
 * 5.  Renumber when nb reaches 255
 * 6.  get_by_nb lookup
 * 7.  get_next circular iteration
 * 8.  Save and load round-trip via JSON
 * 9.  Load from empty / missing file
 * 10. Null pointer handling
 */

#include <stdio.h>
#include <string.h>

#include "wifi_config.h"
#include "osal_mount.h"
#include "osal_file.h"

/* Test results tracking */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test macros */
#define TEST_ASSERT( condition, message )                       \
  do {                                                          \
    tests_run++;                                                \
    if ( condition ) {                                          \
      tests_passed++;                                           \
      printf( "[PASS] %s\n", message );                         \
    } else {                                                    \
      tests_failed++;                                           \
      printf( "[FAIL] %s\n", message );                         \
    }                                                           \
  } while ( 0 )

#define TEST_START( name )                                      \
  printf( "\n==================================================\n" ); \
  printf( "TEST: %s\n", name );                                 \
  printf( "==================================================\n" )

#define TEST_END() \
  printf( "--------------------------------------------------\n" )

/* Filesystem setup for save/load tests */
#define TEST_IMAGE_PATH  "/tmp/wifi_config_test.img"
#define TEST_MOUNT_POINT "/"

static void setup_fs( void )
{
  (void) osal_unmount( TEST_MOUNT_POINT );
  (void) osal_rmfs( TEST_IMAGE_PATH );
  (void) osal_mkfs( NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U );
  (void) osal_mount( TEST_IMAGE_PATH, TEST_MOUNT_POINT );
  (void) osal_remove( WIFI_CONFIG_FILE_PATH );
}

static void cleanup_fs( void )
{
  (void) osal_remove( WIFI_CONFIG_FILE_PATH );
  (void) osal_unmount( TEST_MOUNT_POINT );
  (void) osal_rmfs( TEST_IMAGE_PATH );
}

/* ============================================================================
 * Test 1: Add single credential
 * ========================================================================== */
static void test_add_single_credential( void )
{
  TEST_START( "Add single credential to empty list" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  osal_status_t st = wifi_config_add_credential( &list, "MySSID", "MyPass" );
  TEST_ASSERT( st == OSAL_SUCCESS, "add_credential returns SUCCESS" );
  TEST_ASSERT( list.count == 1, "count is 1" );
  TEST_ASSERT( strcmp( list.entries[0].ssid, "MySSID" ) == 0, "ssid matches" );
  TEST_ASSERT( strcmp( list.entries[0].password, "MyPass" ) == 0, "password matches" );
  TEST_ASSERT( list.entries[0].nb == 0, "nb is 0 for first entry" );
  TEST_ASSERT( list.last_use == 0, "last_use points to new entry" );

  TEST_END();
}

/* ============================================================================
 * Test 2: Duplicate SSID updates password and nb
 * ========================================================================== */
static void test_duplicate_ssid_update( void )
{
  TEST_START( "Duplicate SSID updates password and nb" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  wifi_config_add_credential( &list, "Net1", "Pass1" );
  wifi_config_add_credential( &list, "Net2", "Pass2" );

  /* Update Net1 with new password */
  osal_status_t st = wifi_config_add_credential( &list, "Net1", "NewPass1" );
  TEST_ASSERT( st == OSAL_SUCCESS, "update returns SUCCESS" );
  TEST_ASSERT( list.count == 2, "count unchanged at 2" );
  TEST_ASSERT( strcmp( list.entries[0].password, "NewPass1" ) == 0, "password updated" );
  TEST_ASSERT( list.entries[0].nb > list.entries[1].nb, "nb of updated entry > other" );
  TEST_ASSERT( list.last_use == list.entries[0].nb, "last_use tracks updated entry" );

  TEST_END();
}

/* ============================================================================
 * Test 3: Fill list to max capacity
 * ========================================================================== */
static void test_fill_to_max( void )
{
  TEST_START( "Fill list to max capacity" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
  for ( int i = 0; i < WIFI_CONFIG_MAX_CREDENTIALS; ++i )
  {
    snprintf( ssid, sizeof( ssid ), "Net_%d", i );
    osal_status_t st = wifi_config_add_credential( &list, ssid, "pass" );
    TEST_ASSERT( st == OSAL_SUCCESS, "add succeeds" );
  }

  TEST_ASSERT( list.count == WIFI_CONFIG_MAX_CREDENTIALS, "count == max" );

  /* Verify all entries are unique */
  bool all_unique = true;
  for ( int i = 0; i < WIFI_CONFIG_MAX_CREDENTIALS && all_unique; ++i )
  {
    for ( int j = i + 1; j < WIFI_CONFIG_MAX_CREDENTIALS; ++j )
    {
      if ( list.entries[i].nb == list.entries[j].nb )
      {
        all_unique = false;
      }
    }
  }
  TEST_ASSERT( all_unique, "all nb values are unique" );

  TEST_END();
}

/* ============================================================================
 * Test 4: Evict oldest when full
 * ========================================================================== */
static void test_evict_oldest( void )
{
  TEST_START( "Evict oldest entry when list is full" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
  for ( int i = 0; i < WIFI_CONFIG_MAX_CREDENTIALS; ++i )
  {
    snprintf( ssid, sizeof( ssid ), "Net_%d", i );
    wifi_config_add_credential( &list, ssid, "pass" );
  }

  /* The oldest entry is Net_0 with nb=0.  Adding new entry should evict it. */
  osal_status_t st = wifi_config_add_credential( &list, "NewNet", "newpass" );
  TEST_ASSERT( st == OSAL_SUCCESS, "add to full list returns SUCCESS" );
  TEST_ASSERT( list.count == WIFI_CONFIG_MAX_CREDENTIALS, "count still at max" );

  /* Verify Net_0 was evicted */
  bool net0_found = false;
  bool new_found  = false;
  for ( int i = 0; i < list.count; ++i )
  {
    if ( strcmp( list.entries[i].ssid, "Net_0" ) == 0 ) { net0_found = true; }
    if ( strcmp( list.entries[i].ssid, "NewNet" ) == 0 ) { new_found = true; }
  }
  TEST_ASSERT( !net0_found, "oldest entry (Net_0) was evicted" );
  TEST_ASSERT( new_found, "new entry is present" );

  TEST_END();
}

/* ============================================================================
 * Test 5: Renumber when nb reaches 255
 * ========================================================================== */
static void test_renumber_at_max( void )
{
  TEST_START( "Renumber when nb reaches 255" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  /* Manually set up entries near the limit */
  list.count = 3;
  list.entries[0].nb = 252;
  strncpy( list.entries[0].ssid, "A", WIFI_CONFIG_SSID_MAX_LEN );
  strncpy( list.entries[0].password, "pA", WIFI_CONFIG_PASSWORD_MAX_LEN );
  list.entries[1].nb = 253;
  strncpy( list.entries[1].ssid, "B", WIFI_CONFIG_SSID_MAX_LEN );
  strncpy( list.entries[1].password, "pB", WIFI_CONFIG_PASSWORD_MAX_LEN );
  list.entries[2].nb = 255;
  strncpy( list.entries[2].ssid, "C", WIFI_CONFIG_SSID_MAX_LEN );
  strncpy( list.entries[2].password, "pC", WIFI_CONFIG_PASSWORD_MAX_LEN );
  list.last_use = 255;

  /* Adding a new credential should trigger renumbering */
  osal_status_t st = wifi_config_add_credential( &list, "D", "pD" );
  TEST_ASSERT( st == OSAL_SUCCESS, "add after renumber returns SUCCESS" );
  TEST_ASSERT( list.count == 4, "count is now 4" );

  /* After renumbering + add, all nb values should be small sequential numbers */
  bool all_small = true;
  for ( int i = 0; i < list.count; ++i )
  {
    if ( list.entries[i].nb > list.count )
    {
      all_small = false;
    }
  }
  TEST_ASSERT( all_small, "all nb values are small after renumber" );

  /* Verify D was added */
  bool d_found = false;
  for ( int i = 0; i < list.count; ++i )
  {
    if ( strcmp( list.entries[i].ssid, "D" ) == 0 ) { d_found = true; }
  }
  TEST_ASSERT( d_found, "new entry D is present" );

  TEST_END();
}

/* ============================================================================
 * Test 6: get_by_nb lookup
 * ========================================================================== */
static void test_get_by_nb( void )
{
  TEST_START( "get_by_nb lookup" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  wifi_config_add_credential( &list, "Net1", "Pass1" );
  wifi_config_add_credential( &list, "Net2", "Pass2" );
  wifi_config_add_credential( &list, "Net3", "Pass3" );

  wifi_config_entry_t entry = { 0 };

  bool found = wifi_config_get_by_nb( &list, 1, &entry );
  TEST_ASSERT( found, "found entry with nb=1" );
  TEST_ASSERT( strcmp( entry.ssid, "Net2" ) == 0, "nb=1 is Net2" );

  found = wifi_config_get_by_nb( &list, 99, &entry );
  TEST_ASSERT( !found, "nb=99 not found returns false" );

  /* NULL pointer checks */
  found = wifi_config_get_by_nb( NULL, 0, &entry );
  TEST_ASSERT( !found, "NULL list returns false" );

  found = wifi_config_get_by_nb( &list, 0, NULL );
  TEST_ASSERT( !found, "NULL entry returns false" );

  TEST_END();
}

/* ============================================================================
 * Test 7: get_next circular iteration
 * ========================================================================== */
static void test_get_next_circular( void )
{
  TEST_START( "get_next circular iteration" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  wifi_config_add_credential( &list, "A", "pA" );  /* nb=0 */
  wifi_config_add_credential( &list, "B", "pB" );  /* nb=1 */
  wifi_config_add_credential( &list, "C", "pC" );  /* nb=2 */

  wifi_config_entry_t entry = { 0 };

  /* From nb=0, next should be nb=1 (B) */
  bool found = wifi_config_get_next( &list, 0, &entry );
  TEST_ASSERT( found, "get_next from nb=0 found" );
  TEST_ASSERT( strcmp( entry.ssid, "B" ) == 0, "next after 0 is B" );

  /* From nb=1, next should be nb=2 (C) */
  found = wifi_config_get_next( &list, 1, &entry );
  TEST_ASSERT( found, "get_next from nb=1 found" );
  TEST_ASSERT( strcmp( entry.ssid, "C" ) == 0, "next after 1 is C" );

  /* From nb=2 (last), should wrap to nb=0 (A) */
  found = wifi_config_get_next( &list, 2, &entry );
  TEST_ASSERT( found, "get_next from nb=2 wraps" );
  TEST_ASSERT( strcmp( entry.ssid, "A" ) == 0, "wrap from 2 goes to A" );

  /* Empty list */
  wifi_config_list_t empty = { 0 };
  found = wifi_config_get_next( &empty, 0, &entry );
  TEST_ASSERT( !found, "get_next on empty list returns false" );

  TEST_END();
}

/* ============================================================================
 * Test 8: Save / Load round-trip
 * ========================================================================== */
static void test_save_load_roundtrip( void )
{
  TEST_START( "Save and load round-trip via JSON" );

  wifi_config_list_t orig;
  memset( &orig, 0, sizeof( orig ) );

  wifi_config_add_credential( &orig, "Home", "home123" );
  wifi_config_add_credential( &orig, "Office", "office456" );
  wifi_config_add_credential( &orig, "Guest", "guest789" );
  orig.last_use = 1;

  osal_status_t st = wifi_config_save( &orig );
  TEST_ASSERT( st == OSAL_SUCCESS, "save returns SUCCESS" );

  wifi_config_list_t loaded;
  memset( &loaded, 0, sizeof( loaded ) );

  st = wifi_config_load( &loaded );
  TEST_ASSERT( st == OSAL_SUCCESS, "load returns SUCCESS" );
  TEST_ASSERT( loaded.count == orig.count, "count matches" );
  TEST_ASSERT( loaded.last_use == orig.last_use, "last_use matches" );

  for ( int i = 0; i < loaded.count; ++i )
  {
    TEST_ASSERT( loaded.entries[i].nb == orig.entries[i].nb, "nb[i] matches" );
    TEST_ASSERT( strcmp( loaded.entries[i].ssid, orig.entries[i].ssid ) == 0, "ssid[i] matches" );
    TEST_ASSERT( strcmp( loaded.entries[i].password, orig.entries[i].password ) == 0, "password[i] matches" );
  }

  TEST_END();
}

/* ============================================================================
 * Test 9: Load from missing file
 * ========================================================================== */
static void test_load_missing_file( void )
{
  TEST_START( "Load from missing file" );

  (void) osal_remove( WIFI_CONFIG_FILE_PATH );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  osal_status_t st = wifi_config_load( &list );
  TEST_ASSERT( st != OSAL_SUCCESS, "load from missing file fails" );
  TEST_ASSERT( list.count == 0, "count stays 0" );

  TEST_END();
}

/* ============================================================================
 * Test 10: Null pointer handling
 * ========================================================================== */
static void test_null_pointers( void )
{
  TEST_START( "Null pointer handling" );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  TEST_ASSERT( wifi_config_load( NULL ) == OSAL_INVALID_POINTER, "load(NULL) returns INVALID_POINTER" );
  TEST_ASSERT( wifi_config_save( NULL ) == OSAL_INVALID_POINTER, "save(NULL) returns INVALID_POINTER" );

  TEST_ASSERT( wifi_config_add_credential( NULL, "s", "p" ) == OSAL_INVALID_POINTER,
               "add_credential(NULL list)" );
  TEST_ASSERT( wifi_config_add_credential( &list, NULL, "p" ) == OSAL_INVALID_POINTER,
               "add_credential(NULL ssid)" );
  TEST_ASSERT( wifi_config_add_credential( &list, "s", NULL ) == OSAL_INVALID_POINTER,
               "add_credential(NULL password)" );

  TEST_END();
}

/* ============================================================================
 * Runner
 * ========================================================================== */

int wifi_config_tests_run( void )
{
  tests_run    = 0;
  tests_passed = 0;
  tests_failed = 0;

  printf( "\n" );
  printf( "==================================================\n" );
  printf( "           Wi-Fi Config Tests                     \n" );
  printf( "==================================================\n" );

  setup_fs();

  test_add_single_credential();
  test_duplicate_ssid_update();
  test_fill_to_max();
  test_evict_oldest();
  test_renumber_at_max();
  test_get_by_nb();
  test_get_next_circular();
  test_save_load_roundtrip();
  test_load_missing_file();
  test_null_pointers();

  cleanup_fs();

  printf( "\n" );
  printf( "==================================================\n" );
  printf( "                  TEST SUMMARY                    \n" );
  printf( "==================================================\n" );
  printf( "  Total tests:  %d\n", tests_run );
  printf( "  Passed:       %d\n", tests_passed );
  printf( "  Failed:       %d\n", tests_failed );
  printf( "  Success rate: %.1f%%\n",
          ( tests_run > 0 ) ? ( 100.0 * tests_passed / tests_run ) : 0.0 );
  printf( "==================================================\n" );

  if ( tests_failed == 0 )
  {
    printf( "\nALL TESTS PASSED\n\n" );
  }
  else
  {
    printf( "\nSOME TESTS FAILED\n\n" );
  }

  return tests_failed;
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  int failed = wifi_config_tests_run();

#ifndef ESP_PLATFORM
  return ( failed == 0 ) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
