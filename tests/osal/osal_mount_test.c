/*
 * OSAL Mount/Filesystem Lifecycle Tests
 *
 * Tests:
 * 1.  mkfs/initfs/mount/unmount lifecycle
 * 2.  Argument validation (NULL, too-long paths)
 * 3.  filesys_stat_volume before/after mount
 * 4.  chkfs not implemented
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_file.h"
#include "osal_mount.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#define TEST_IMAGE_PATH  "flash_test"
#define TEST_MOUNT_POINT "/littlefs"
#else
#define TEST_IMAGE_PATH  "/tmp/osal_mount_test.img"
#define TEST_MOUNT_POINT "/"
#endif
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
    reset_filesystem();

    int32_t rc = osal_initfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_unmount(TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_rmfs(TEST_IMAGE_PATH);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
}

static void test_mount_argument_validation(void)
{
    char long_path[OSAL_MAX_PATH_LEN + 16];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    int32_t rc = osal_mkfs(NULL, NULL, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_mount(NULL, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_mount(TEST_IMAGE_PATH, NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_mount(long_path, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_FS_ERR_PATH_TOO_LONG, rc);

    rc = osal_unmount(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_rmfs(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);
}

static void test_stat_volume_flow(void)
{
    reset_filesystem();

    osal_statvfs_t st;
    memset(&st, 0, sizeof(st));

    int32_t rc = osal_filesys_stat_volume(TEST_IMAGE_PATH, &st);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INCORRECT_OBJ_STATE, rc);

    rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_filesys_stat_volume(TEST_IMAGE_PATH, &st);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_TRUE(st.block_size > 0U);
    TEST_ASSERT_TRUE(st.total_blocks > 0U);
    TEST_ASSERT_TRUE(st.blocks_free <= st.total_blocks);

    osal_file_id_t fd = osal_open_create(TEST_FILE_PATH,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    char payload[1024];
    memset(payload, 'M', sizeof(payload));
    rc = osal_write(fd, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(payload), rc);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_close(fd));

    osal_statvfs_t st_after;
    memset(&st_after, 0, sizeof(st_after));
    rc = osal_filesys_stat_volume(TEST_IMAGE_PATH, &st_after);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_TRUE(st_after.blocks_free <= st.blocks_free);

    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_rmfs(TEST_IMAGE_PATH));
}

static void test_chkfs_not_implemented(void)
{
    int32_t rc = osal_chkfs(TEST_IMAGE_PATH, false);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_NOT_IMPLEMENTED, rc);
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

void osal_mount_tests_run(void)
{
    RUN_TEST(test_mount_lifecycle);
    RUN_TEST(test_mount_argument_validation);
    RUN_TEST(test_stat_volume_flow);
    RUN_TEST(test_chkfs_not_implemented);

    reset_filesystem();
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
    printf("            OSAL Mount/Filesystem Tests           \n");
    printf("==================================================\n");
    printf("\n");

    reset_filesystem();

    osal_mount_tests_run();

    reset_filesystem();

#ifndef ESP_PLATFORM
    return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
