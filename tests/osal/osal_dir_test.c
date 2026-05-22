/*
 * OSAL Directory API Tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_dir.h"
#include "osal_file.h"
#include "osal_mount.h"

/* Test results tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message)                         \
    do {                                                        \
        tests_run++;                                            \
        if (condition) {                                        \
            tests_passed++;                                     \
            printf("[PASS] %s\n", message);                   \
        } else {                                                \
            tests_failed++;                                     \
            printf("[FAIL] %s\n", message);                   \
        }                                                       \
    } while (0)

#define TEST_START(name)                                        \
    printf("\n==================================================\n"); \
    printf("TEST: %s\n", name);                               \
    printf("==================================================\n")

#define TEST_END() \
    printf("--------------------------------------------------\n")

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
    TEST_START("Open/read/close on empty directory");

    int32_t rc = osal_mkdir(TEST_DIR_A);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_mkdir creates empty directory");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT(dir >= 0, "osal_dir_open succeeds for empty directory");

    osal_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    rc = osal_dir_read(dir, &entry);
    TEST_ASSERT(rc == OSAL_ERR_EMPTY_SET, "osal_dir_read on empty directory returns OSAL_ERR_EMPTY_SET");

    rc = osal_dir_close(dir);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_dir_close succeeds");

    TEST_END();
}

static void test_non_empty_dir_iteration(void)
{
    TEST_START("Open/read/close on non-empty directory");

    int32_t rc = osal_mkdir(TEST_DIR_A);
    TEST_ASSERT(rc == OSAL_SUCCESS, "create parent directory");

    write_small_file(TEST_FILE_A, "abc");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT(dir >= 0, "open non-empty directory");

    bool found_file = false;
    osal_dirent_t entry;
    while ((rc = osal_dir_read(dir, &entry)) == OSAL_SUCCESS)
    {
        if (strcmp(entry.name, "file_a.txt") == 0)
        {
            found_file = true;
            TEST_ASSERT(entry.type == OSAL_DIRENT_TYPE_FILE || entry.type == OSAL_DIRENT_TYPE_UNKNOWN,
                        "entry type is file/unknown for file");
        }
    }

    TEST_ASSERT(rc == OSAL_ERR_EMPTY_SET, "iteration ends with OSAL_ERR_EMPTY_SET");
    TEST_ASSERT(found_file, "directory listing contains created file");
    TEST_ASSERT(osal_dir_close(dir) == OSAL_SUCCESS, "close non-empty directory");

    TEST_END();
}

static void test_rewind_behavior(void)
{
    TEST_START("Rewind directory behavior");

    (void)osal_mkdir(TEST_DIR_A);
    write_small_file(TEST_FILE_A, "rewind");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT(dir >= 0, "open directory for rewind");

    osal_dirent_t first;
    int32_t rc = osal_dir_read(dir, &first);
    TEST_ASSERT(rc == OSAL_SUCCESS, "first read succeeds");

    rc = osal_dir_rewind(dir);
    TEST_ASSERT(rc == OSAL_SUCCESS, "rewind succeeds");

    osal_dirent_t first_after_rewind;
    rc = osal_dir_read(dir, &first_after_rewind);
    TEST_ASSERT(rc == OSAL_SUCCESS, "read after rewind succeeds");
    TEST_ASSERT(strcmp(first.name, first_after_rewind.name) == 0,
                "first entry after rewind matches original first entry");

    TEST_ASSERT(osal_dir_close(dir) == OSAL_SUCCESS, "close directory after rewind test");

    TEST_END();
}

static void test_mkdir_rmdir_nested(void)
{
    TEST_START("mkdir/rmdir and nested traversal");

    int32_t rc = osal_mkdir(TEST_DIR_A);
    TEST_ASSERT(rc == OSAL_SUCCESS, "create top-level directory");

    rc = osal_mkdir(TEST_NESTED);
    TEST_ASSERT(rc == OSAL_SUCCESS, "create nested directory");

    write_small_file("/dir_a/nested/n.txt", "nested");

    osal_dir_id_t dir = osal_dir_open(TEST_DIR_A);
    TEST_ASSERT(dir >= 0, "open top-level directory for nested traversal");

    bool found_nested = false;
    osal_dirent_t entry;
    while ((rc = osal_dir_read(dir, &entry)) == OSAL_SUCCESS)
    {
        if (strcmp(entry.name, "nested") == 0)
        {
            found_nested = true;
            TEST_ASSERT(entry.type == OSAL_DIRENT_TYPE_DIR || entry.type == OSAL_DIRENT_TYPE_UNKNOWN,
                        "nested entry reported as directory/unknown");
        }
    }
    TEST_ASSERT(found_nested, "nested directory is visible from parent");
    TEST_ASSERT(osal_dir_close(dir) == OSAL_SUCCESS, "close parent dir handle");

    rc = osal_rmdir(TEST_DIR_A);
    TEST_ASSERT(rc != OSAL_SUCCESS, "rmdir non-empty directory fails");

    TEST_ASSERT(osal_remove("/dir_a/nested/n.txt") == OSAL_SUCCESS, "remove nested file");
    TEST_ASSERT(osal_rmdir(TEST_NESTED) == OSAL_SUCCESS, "remove nested directory");
    TEST_ASSERT(osal_rmdir(TEST_DIR_A) == OSAL_SUCCESS, "remove parent directory after cleanup");

    TEST_END();
}

static void test_invalid_path_handling(void)
{
    TEST_START("Invalid path handling");

    osal_dirent_t entry;
    char too_long[OSAL_MAX_PATH_LEN + 8];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    osal_dir_id_t dir = osal_dir_open(NULL);
    TEST_ASSERT(dir == OSAL_INVALID_POINTER, "osal_dir_open(NULL) returns OSAL_INVALID_POINTER");

    dir = osal_dir_open("");
    TEST_ASSERT(dir == OSAL_FS_ERR_PATH_INVALID, "osal_dir_open(empty) returns OSAL_FS_ERR_PATH_INVALID");

    dir = osal_dir_open(too_long);
    TEST_ASSERT(dir == OSAL_FS_ERR_PATH_TOO_LONG, "osal_dir_open(too long) returns OSAL_FS_ERR_PATH_TOO_LONG");

    int32_t rc = osal_dir_read(-1, &entry);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_ID, "osal_dir_read invalid id returns OSAL_ERR_INVALID_ID");

    rc = osal_dir_read(1, NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_dir_read NULL entry returns OSAL_INVALID_POINTER");

    rc = osal_mkdir(NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_mkdir(NULL) returns OSAL_INVALID_POINTER");

    rc = osal_rmdir(NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_rmdir(NULL) returns OSAL_INVALID_POINTER");

    TEST_END();
}

int osal_dir_tests_run(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    printf("\n");
    printf("==================================================\n");
    printf("              OSAL Directory API Tests            \n");
    printf("==================================================\n");

    setup_test_fs();
    test_empty_dir_open_read_close();

    setup_test_fs();
    test_non_empty_dir_iteration();

    setup_test_fs();
    test_rewind_behavior();

    setup_test_fs();
    test_mkdir_rmdir_nested();

    setup_test_fs();
    test_invalid_path_handling();

    cleanup_test_fs();

    printf("\n");
    printf("==================================================\n");
    printf("                  TEST SUMMARY                    \n");
    printf("==================================================\n");
    printf("  Total tests:  %d\n", tests_run);
    printf("  Passed:       %d\n", tests_passed);
    printf("  Failed:       %d\n", tests_failed);
    printf("  Success rate: %.1f%%\n", (tests_run > 0) ? (100.0 * tests_passed / tests_run) : 0.0);
    printf("==================================================\n");

    if (tests_failed == 0)
    {
        printf("\nALL DIRECTORY TESTS PASSED\n\n");
    }
    else
    {
        printf("\nDIRECTORY TESTS FAILED\n\n");
    }

    return tests_failed;
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    int failed = osal_dir_tests_run();

#ifndef ESP_PLATFORM
    return (failed == 0) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
