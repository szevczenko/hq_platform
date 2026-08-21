/*
 * OSAL Directory API Tests
 *
 * Tests:
 * 1. Open/read/close on empty directory
 * 2. Open/read/close on non-empty directory
 * 3. Rewind directory behavior
 * 4. mkdir/rmdir and nested traversal
 * 5. Invalid path handling
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_dir.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#define TEST_IMAGE_PATH  "flash_test"
#define TEST_MOUNT_POINT "/littlefs"
#else
#define TEST_IMAGE_PATH  "/tmp/osal_dir_test.img"
#define TEST_MOUNT_POINT "/"
#endif

#define TEST_DIR_A "/dir_a"
#define TEST_DIR_B "/dir_b"
#define TEST_DIR_C "/dir_c"
#define TEST_NESTED "/dir_a/nested"
#define TEST_FILE_A "/dir_a/file_a.txt"

static void write_small_file(const char *path, const char *data)
{
    osal_file_id_t fd = osal_open_create(path,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    if (fd >= 0)
    {
        (void)osal_write(fd, data, strlen(data));
        (void)osal_close(fd);
    }
}

static void setup_test_fs(void)
{
    (void)osal_unmount(TEST_MOUNT_POINT);
    (void)osal_rmfs(TEST_IMAGE_PATH);

    (void)osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U);
    (void)osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);

    (void)osal_remove(TEST_FILE_A);
    (void)osal_rmdir(TEST_NESTED);
    (void)osal_rmdir(TEST_DIR_A);
    (void)osal_rmdir(TEST_DIR_B);
    (void)osal_rmdir(TEST_DIR_C);
}

static void cleanup_test_fs(void)
{
    (void)osal_remove(TEST_FILE_A);
    (void)osal_rmdir(TEST_NESTED);
    (void)osal_rmdir(TEST_DIR_A);
    (void)osal_rmdir(TEST_DIR_B);
    (void)osal_rmdir(TEST_DIR_C);
    (void)osal_unmount(TEST_MOUNT_POINT);
    (void)osal_rmfs(TEST_IMAGE_PATH);
}

static void test_empty_dir_open_read_close(void)
{
    int32_t rc = osal_mkdir(TEST_DIR_A);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)dir);

    osal_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    rc = osal_dir_read(dir, &entry);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_EMPTY_SET, rc);

    rc = osal_dir_close(dir);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
}

static void test_non_empty_dir_iteration(void)
{
    int32_t rc = osal_mkdir(TEST_DIR_A);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    write_small_file(TEST_FILE_A, "abc");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)dir);

    bool found_file = false;
    osal_dirent_t entry;
    while ((rc = osal_dir_read(dir, &entry)) == OSAL_SUCCESS)
    {
        if (strcmp(entry.name, "file_a.txt") == 0)
        {
            found_file = true;
            TEST_ASSERT_TRUE(entry.type == OSAL_DIRENT_TYPE_FILE || entry.type == OSAL_DIRENT_TYPE_UNKNOWN);
        }
    }

    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_EMPTY_SET, rc);
    TEST_ASSERT_TRUE(found_file);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_dir_close(dir));
}

static void test_rewind_behavior(void)
{
    (void)osal_mkdir(TEST_DIR_A);
    write_small_file(TEST_FILE_A, "rewind");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)dir);

    osal_dirent_t first;
    int32_t rc = osal_dir_read(dir, &first);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_dir_rewind(dir);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    osal_dirent_t first_after_rewind;
    rc = osal_dir_read(dir, &first_after_rewind);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32(0, strcmp(first.name, first_after_rewind.name));

    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_dir_close(dir));
}

static void test_mkdir_rmdir_nested(void)
{
    int32_t rc = osal_mkdir(TEST_DIR_A);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_mkdir(TEST_NESTED);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    write_small_file("/dir_a/nested/n.txt", "nested");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)dir);

    bool found_nested = false;
    osal_dirent_t entry;
    while ((rc = osal_dir_read(dir, &entry)) == OSAL_SUCCESS)
    {
        if (strcmp(entry.name, "nested") == 0)
        {
            found_nested = true;
            TEST_ASSERT_TRUE(entry.type == OSAL_DIRENT_TYPE_DIR || entry.type == OSAL_DIRENT_TYPE_UNKNOWN);
        }
    }
    TEST_ASSERT_TRUE(found_nested);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_dir_close(dir));

    rc = osal_rmdir(TEST_DIR_A);
    TEST_ASSERT_NOT_EQUAL(OSAL_SUCCESS, rc);

    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_remove("/dir_a/nested/n.txt"));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_rmdir(TEST_NESTED));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_rmdir(TEST_DIR_A));
}

static void test_invalid_path_handling(void)
{
    char too_long[OSAL_MAX_PATH_LEN + 8];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    osal_dir_id_t dir = osal_dir_open(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, dir);

    dir = osal_dir_open("");
    TEST_ASSERT_EQUAL_INT32(OSAL_FS_ERR_PATH_INVALID, dir);

    dir = osal_dir_open(too_long);
    TEST_ASSERT_EQUAL_INT32(OSAL_FS_ERR_PATH_TOO_LONG, dir);

    osal_dirent_t entry;
    int32_t rc = osal_dir_read(-1, &entry);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_ID, rc);

    rc = osal_dir_read(1, NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_mkdir(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_rmdir(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

void osal_dir_tests_run(void)
{
    setup_test_fs();
    RUN_TEST(test_empty_dir_open_read_close);

    setup_test_fs();
    RUN_TEST(test_non_empty_dir_iteration);

    setup_test_fs();
    RUN_TEST(test_rewind_behavior);

    setup_test_fs();
    RUN_TEST(test_mkdir_rmdir_nested);

    setup_test_fs();
    RUN_TEST(test_invalid_path_handling);

    cleanup_test_fs();
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    printf("\n");
    printf("==================================================\n");
    printf("          OSAL Directory API Tests                \n");
    printf("==================================================\n");
    printf("\n");

    setup_test_fs();

    osal_dir_tests_run();

    cleanup_test_fs();

#ifndef ESP_PLATFORM
    return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */