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
#include "unity.h"

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
  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  osal_status_t st = wifi_config_add_credential( &list, "MySSID", "MyPass" );
  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "add_credential returns SUCCESS" );
  TEST_ASSERT_MESSAGE( list.count == 1, "count is 1" );
  TEST_ASSERT_MESSAGE( strcmp( list.entries[0].ssid, "MySSID" ) == 0, "ssid matches" );
  TEST_ASSERT_MESSAGE( strcmp( list.entries[0].password, "MyPass" ) == 0, "password matches" );
  TEST_ASSERT_MESSAGE( list.entries[0].nb == 0, "nb is 0 for first entry" );
  TEST_ASSERT_MESSAGE( list.last_use == 0, "last_use points to new entry" );
}

/* ============================================================================
 * Test 2: Duplicate SSID updates password and nb
 * ========================================================================== */
static void test_duplicate_ssid_update( void )
{
  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  wifi_config_add_credential( &list, "Net1", "Pass1" );
  wifi_config_add_credential( &list, "Net2", "Pass2" );

  /* Update Net1 with new password */
  osal_status_t st = wifi_config_add_credential( &list, "Net1", "NewPass1" );
  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "update returns SUCCESS" );
  TEST_ASSERT_MESSAGE( list.count == 2, "count unchanged at 2" );
  TEST_ASSERT_MESSAGE( strcmp( list.entries[0].password, "NewPass1" ) == 0, "password updated" );
  TEST_ASSERT_MESSAGE( list.entries[0].nb > list.entries[1].nb, "nb of updated entry > other" );
  TEST_ASSERT_MESSAGE( list.last_use == list.entries[0].nb, "last_use tracks updated entry" );
}

/* ============================================================================
 * Test 3: Fill list to max capacity
 * ========================================================================== */
static void test_fill_to_max( void )
{
  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
  for ( int i = 0; i < WIFI_CONFIG_MAX_CREDENTIALS; ++i )
  {
    snprintf( ssid, sizeof( ssid ), "Net_%d", i );
    osal_status_t st = wifi_config_add_credential( &list, ssid, "pass" );
    TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "add succeeds" );
  }

  TEST_ASSERT_MESSAGE( list.count == WIFI_CONFIG_MAX_CREDENTIALS, "count == max" );

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
  TEST_ASSERT_MESSAGE( all_unique, "all nb values are unique" );
}

/* ============================================================================
 * Test 4: Evict oldest when full
 * ========================================================================== */
static void test_evict_oldest( void )
{
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
  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "add to full list returns SUCCESS" );
  TEST_ASSERT_MESSAGE( list.count == WIFI_CONFIG_MAX_CREDENTIALS, "count still at max" );

  /* Verify Net_0 was evicted */
  bool net0_found = false;
  bool new_found  = false;
  for ( int i = 0; i < list.count; ++i )
  {
    if ( strcmp( list.entries[i].ssid, "Net_0" ) == 0 ) { net0_found = true; }
    if ( strcmp( list.entries[i].ssid, "NewNet" ) == 0 ) { new_found = true; }
  }
  TEST_ASSERT_MESSAGE( !net0_found, "oldest entry (Net_0) was evicted" );
  TEST_ASSERT_MESSAGE( new_found, "new entry is present" );
}

/* ============================================================================
 * Test 5: Renumber when nb reaches 255
 * ========================================================================== */
static void test_renumber_at_max( void )
{
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
  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "add after renumber returns SUCCESS" );
  TEST_ASSERT_MESSAGE( list.count == 4, "count is now 4" );

  /* After renumbering + add, all nb values should be small sequential numbers */
  bool all_small = true;
  for ( int i = 0; i < list.count; ++i )
  {
    if ( list.entries[i].nb > list.count )
    {
      all_small = false;
    }
  }
  TEST_ASSERT_MESSAGE( all_small, "all nb values are small after renumber" );

  /* Verify D was added */
  bool d_found = false;
  for ( int i = 0; i < list.count; ++i )
  {
    if ( strcmp( list.entries[i].ssid, "D" ) == 0 ) { d_found = true; }
  }
  TEST_ASSERT_MESSAGE( d_found, "new entry D is present" );
}

/* ============================================================================
 * Test 6: get_by_nb lookup
 * ========================================================================== */
static void test_get_by_nb( void )
{
  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  wifi_config_add_credential( &list, "Net1", "Pass1" );
  wifi_config_add_credential( &list, "Net2", "Pass2" );
  wifi_config_add_credential( &list, "Net3", "Pass3" );

  wifi_config_entry_t entry = { 0 };

  bool found = wifi_config_get_by_nb( &list, 1, &entry );
  TEST_ASSERT_MESSAGE( found, "found entry with nb=1" );
  TEST_ASSERT_MESSAGE( strcmp( entry.ssid, "Net2" ) == 0, "nb=1 is Net2" );

  found = wifi_config_get_by_nb( &list, 99, &entry );
  TEST_ASSERT_MESSAGE( !found, "nb=99 not found returns false" );

  /* NULL pointer checks */
  found = wifi_config_get_by_nb( NULL, 0, &entry );
  TEST_ASSERT_MESSAGE( !found, "NULL list returns false" );

  found = wifi_config_get_by_nb( &list, 0, NULL );
  TEST_ASSERT_MESSAGE( !found, "NULL entry returns false" );
}

/* ============================================================================
 * Test 7: get_next circular iteration
 * ========================================================================== */
static void test_get_next_circular( void )
{
  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  wifi_config_add_credential( &list, "A", "pA" );  /* nb=0 */
  wifi_config_add_credential( &list, "B", "pB" );  /* nb=1 */
  wifi_config_add_credential( &list, "C", "pC" );  /* nb=2 */

  wifi_config_entry_t entry = { 0 };

  /* From nb=0, next should be nb=1 (B) */
  bool found = wifi_config_get_next( &list, 0, &entry );
  TEST_ASSERT_MESSAGE( found, "get_next from nb=0 found" );
  TEST_ASSERT_MESSAGE( strcmp( entry.ssid, "B" ) == 0, "next after 0 is B" );

  /* From nb=1, next should be nb=2 (C) */
  found = wifi_config_get_next( &list, 1, &entry );
  TEST_ASSERT_MESSAGE( found, "get_next from nb=1 found" );
  TEST_ASSERT_MESSAGE( strcmp( entry.ssid, "C" ) == 0, "next after 1 is C" );

  /* From nb=2 (last), should wrap to nb=0 (A) */
  found = wifi_config_get_next( &list, 2, &entry );
  TEST_ASSERT_MESSAGE( found, "get_next from nb=2 wraps" );
  TEST_ASSERT_MESSAGE( strcmp( entry.ssid, "A" ) == 0, "wrap from 2 goes to A" );

  /* Empty list */
  wifi_config_list_t empty = { 0 };
  found = wifi_config_get_next( &empty, 0, &entry );
  TEST_ASSERT_MESSAGE( !found, "get_next on empty list returns false" );
}

/* ============================================================================
 * Test 8: Save / Load round-trip
 * ========================================================================== */
static void test_save_load_roundtrip( void )
{
  wifi_config_list_t orig;
  memset( &orig, 0, sizeof( orig ) );

  wifi_config_add_credential( &orig, "Home", "home123" );
  wifi_config_add_credential( &orig, "Office", "office456" );
  wifi_config_add_credential( &orig, "Guest", "guest789" );
  orig.last_use = 1;

  osal_status_t st = wifi_config_save( &orig );
  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "save returns SUCCESS" );

  wifi_config_list_t loaded;
  memset( &loaded, 0, sizeof( loaded ) );

  st = wifi_config_load( &loaded );
  TEST_ASSERT_MESSAGE( st == OSAL_SUCCESS, "load returns SUCCESS" );
  TEST_ASSERT_MESSAGE( loaded.count == orig.count, "count matches" );
  TEST_ASSERT_MESSAGE( loaded.last_use == orig.last_use, "last_use matches" );

  for ( int i = 0; i < loaded.count; ++i )
  {
    TEST_ASSERT_MESSAGE( loaded.entries[i].nb == orig.entries[i].nb, "nb[i] matches" );
    TEST_ASSERT_MESSAGE( strcmp( loaded.entries[i].ssid, orig.entries[i].ssid ) == 0, "ssid[i] matches" );
    TEST_ASSERT_MESSAGE( strcmp( loaded.entries[i].password, orig.entries[i].password ) == 0, "password[i] matches" );
  }
}

/* ============================================================================
 * Test 9: Load from missing file
 * ========================================================================== */
static void test_load_missing_file( void )
{
  (void) osal_remove( WIFI_CONFIG_FILE_PATH );

  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  osal_status_t st = wifi_config_load( &list );
  TEST_ASSERT_MESSAGE( st != OSAL_SUCCESS, "load from missing file fails" );
  TEST_ASSERT_MESSAGE( list.count == 0, "count stays 0" );
}

/* ============================================================================
 * Test 10: Null pointer handling
 * ========================================================================== */
static void test_null_pointers( void )
{
  wifi_config_list_t list;
  memset( &list, 0, sizeof( list ) );

  TEST_ASSERT_MESSAGE( wifi_config_load( NULL ) == OSAL_INVALID_POINTER, "load(NULL) returns INVALID_POINTER" );
  TEST_ASSERT_MESSAGE( wifi_config_save( NULL ) == OSAL_INVALID_POINTER, "save(NULL) returns INVALID_POINTER" );

  TEST_ASSERT_MESSAGE( wifi_config_add_credential( NULL, "s", "p" ) == OSAL_INVALID_POINTER,
                       "add_credential(NULL list)" );
  TEST_ASSERT_MESSAGE( wifi_config_add_credential( &list, NULL, "p" ) == OSAL_INVALID_POINTER,
                       "add_credential(NULL ssid)" );
  TEST_ASSERT_MESSAGE( wifi_config_add_credential( &list, "s", NULL ) == OSAL_INVALID_POINTER,
                       "add_credential(NULL password)" );
}

/* ============================================================================
 * Runner
 * ========================================================================== */

void wifi_config_tests_run( void )
{
  setup_fs();

  RUN_TEST( test_add_single_credential );
  RUN_TEST( test_duplicate_ssid_update );
  RUN_TEST( test_fill_to_max );
  RUN_TEST( test_evict_oldest );
  RUN_TEST( test_renumber_at_max );
  RUN_TEST( test_get_by_nb );
  RUN_TEST( test_get_next_circular );
  RUN_TEST( test_save_load_roundtrip );
  RUN_TEST( test_load_missing_file );
  RUN_TEST( test_null_pointers );

  cleanup_fs();
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  wifi_config_tests_run();

#ifndef ESP_PLATFORM
  return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */