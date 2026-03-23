/*
 * OSAL Mount/Filesystem Lifecycle Tests
 */

#include <stdio.h>
#include <string.h>

#include "osal_file.h"
#include "osal_mount.h"

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
    printf("TEST: %s\n", name);                              \
    printf("==================================================\n")

#define TEST_END() \
    printf("--------------------------------------------------\n")

#define TEST_IMAGE_PATH "/tmp/osal_mount_test.img"
#define TEST_MOUNT_POINT "/"
#define TEST_FILE_PATH "/mount_suite_file.bin"
#define TEST_BLOCK_SIZE 4096U
#define TEST_BLOCK_COUNT 256U

static void reset_filesystem(void)
{
    (void)osal_unmount(TEST_MOUNT_POINT);
    (void)osal_rmfs(TEST_IMAGE_PATH);
}

static void test_mount_lifecycle(void)
{
    TEST_START("mkfs/initfs/mount/unmount lifecycle");

    reset_filesystem();

    int32_t rc = osal_initfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_initfs succeeds");

    rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_mkfs succeeds");

    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_mount succeeds");

    rc = osal_unmount(TEST_MOUNT_POINT);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_unmount succeeds");

    rc = osal_rmfs(TEST_IMAGE_PATH);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_rmfs succeeds");

    TEST_END();
}

static void test_mount_argument_validation(void)
{
    TEST_START("Argument validation");

    char long_path[OSAL_MAX_PATH_LEN + 16];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    int32_t rc = osal_mkfs(NULL, NULL, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_mkfs(NULL devname) returns OSAL_INVALID_POINTER");

    rc = osal_mount(NULL, TEST_MOUNT_POINT);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_mount(NULL devname) returns OSAL_INVALID_POINTER");

    rc = osal_mount(TEST_IMAGE_PATH, NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_mount(NULL mount_point) returns OSAL_INVALID_POINTER");

    rc = osal_mount(long_path, TEST_MOUNT_POINT);
    TEST_ASSERT(rc == OSAL_FS_ERR_PATH_TOO_LONG, "osal_mount(too_long_devname) returns OSAL_FS_ERR_PATH_TOO_LONG");

    rc = osal_unmount(NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_unmount(NULL) returns OSAL_INVALID_POINTER");

    rc = osal_rmfs(NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_rmfs(NULL) returns OSAL_INVALID_POINTER");

    TEST_END();
}

static void test_stat_volume_flow(void)
{
    TEST_START("filesys_stat_volume before/after mount");

    reset_filesystem();

    osal_statvfs_t st;
    memset(&st, 0, sizeof(st));

    int32_t rc = osal_filesys_stat_volume(TEST_IMAGE_PATH, &st);
    TEST_ASSERT(rc == OSAL_ERR_INCORRECT_OBJ_STATE, "stat volume before mount returns INCORRECT_OBJ_STATE");

    rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT(rc == OSAL_SUCCESS, "mkfs for stat test succeeds");

    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT(rc == OSAL_SUCCESS, "mount for stat test succeeds");

    rc = osal_filesys_stat_volume(TEST_IMAGE_PATH, &st);
    TEST_ASSERT(rc == OSAL_SUCCESS, "stat volume after mount succeeds");
    TEST_ASSERT(st.block_size > 0U, "stat volume reports non-zero block size");
    TEST_ASSERT(st.total_blocks > 0U, "stat volume reports non-zero total blocks");
    TEST_ASSERT(st.blocks_free <= st.total_blocks, "free blocks are not greater than total blocks");

    osal_file_id_t fd = osal_open_create(TEST_FILE_PATH,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file on mounted fs succeeds");

    char payload[1024];
    memset(payload, 'M', sizeof(payload));
    rc = osal_write(fd, payload, sizeof(payload));
    TEST_ASSERT(rc == (int32_t)sizeof(payload), "write payload succeeds");
    TEST_ASSERT(osal_close(fd) == OSAL_SUCCESS, "close written file succeeds");

    osal_statvfs_t st_after;
    memset(&st_after, 0, sizeof(st_after));
    rc = osal_filesys_stat_volume(TEST_IMAGE_PATH, &st_after);
    TEST_ASSERT(rc == OSAL_SUCCESS, "stat volume after write succeeds");
    TEST_ASSERT(st_after.blocks_free <= st.blocks_free, "free blocks do not increase after write");

    TEST_ASSERT(osal_unmount(TEST_MOUNT_POINT) == OSAL_SUCCESS, "unmount after stat test succeeds");
    TEST_ASSERT(osal_rmfs(TEST_IMAGE_PATH) == OSAL_SUCCESS, "rmfs after stat test succeeds");

    TEST_END();
}

static void test_chkfs_not_implemented(void)
{
    TEST_START("chkfs not implemented");

    int32_t rc = osal_chkfs(TEST_IMAGE_PATH, false);
    TEST_ASSERT(rc == OSAL_ERR_NOT_IMPLEMENTED, "osal_chkfs returns OSAL_ERR_NOT_IMPLEMENTED");

    TEST_END();
}

int osal_mount_tests_run(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    printf("\n");
    printf("==================================================\n");
    printf("             OSAL Mount API Tests                 \n");
    printf("==================================================\n");

    test_mount_lifecycle();
    test_mount_argument_validation();
    test_stat_volume_flow();
    test_chkfs_not_implemented();

    reset_filesystem();

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
        printf("\nALL MOUNT TESTS PASSED\n\n");
    }
    else
    {
        printf("\nMOUNT TESTS FAILED\n\n");
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
    int failed = osal_mount_tests_run();

#ifndef ESP_PLATFORM
    return (failed == 0) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
