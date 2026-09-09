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
#include "esp_partition.h"
#else
#define TEST_IMAGE_PATH  "/tmp/osal_mount_test.img"
#define TEST_MOUNT_POINT "/"
#define TEST_OTHER_IMAGE_PATH "/tmp/osal_mount_test_other.img"
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

/* TASK-118: a mount failure on a corrupt/absent filesystem must be reported
 * as an error and must never reformat or erase the partition.  Formatting
 * stays reserved for the explicit entry points (osal_mkfs()/osal_rmfs()).
 *
 * POSIX: image A is formatted and populated, image B is left unformatted.
 * A failed mount on B must not touch A (marker file must survive) and B must
 * keep failing to mount (no silent reformat).
 *
 * ESP: the storage partition is erased to simulate a corrupt/absent
 * filesystem.  The failed mount must not auto-format it (a repeat mount still
 * fails), and only the explicit osal_mkfs()/osal_rmfs() path brings it back.
 */
static void test_mount_failure_preserves_contents(void)
{
    /* 1. Create a valid filesystem and plant a marker file. */
    reset_filesystem();
    int32_t rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    osal_file_id_t fd = osal_open_create("/mount_noformat_marker.bin",
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE, OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);
    const char marker[] = "TASK-118 marker";
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(marker),
                            osal_write(fd, marker, sizeof(marker)));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_close(fd));

    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));

#ifndef ESP_PLATFORM
    /* 2. A second, deliberately unformatted image (plain zeroed file). */
    FILE *raw = fopen(TEST_OTHER_IMAGE_PATH, "wb");
    TEST_ASSERT_NOT_NULL(raw);
    char zeros[TEST_BLOCK_SIZE];
    memset(zeros, 0, sizeof(zeros));
    for (size_t i = 0U; i < TEST_BLOCK_COUNT; ++i)
    {
        TEST_ASSERT_EQUAL_UINT(sizeof(zeros), fwrite(zeros, 1U, sizeof(zeros), raw));
    }
    TEST_ASSERT_EQUAL_INT32(0, fclose(raw));

    /* 3. Mounting the unformatted image reports an error... */
    rc = osal_mount(TEST_OTHER_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERROR, rc);

    /* ...and a repeat attempt still fails: the failed mount did not
     * silently format the image. */
    rc = osal_mount(TEST_OTHER_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERROR, rc);
    (void)remove(TEST_OTHER_IMAGE_PATH);

    /* 4. The failed mount attempts left the valid volume untouched. */
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    fd = osal_open_create("/mount_noformat_marker.bin", OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);
    char buffer[sizeof(marker)];
    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(marker),
                            osal_read(fd, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(marker, buffer, sizeof(marker));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_close(fd));

    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_rmfs(TEST_IMAGE_PATH));
#else
    /* 2. Corrupt the storage partition (simulates a missing/destroyed
     *    filesystem) by erasing it with the ESP partition API. */
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, TEST_IMAGE_PATH);
    TEST_ASSERT_NOT_NULL(it);
    const esp_partition_t *part = esp_partition_get(it);
    TEST_ASSERT_NOT_NULL(part);
    TEST_ASSERT_EQUAL_INT32(ESP_OK, esp_partition_erase_range(part, 0, part->size));
    esp_partition_iterator_release(it);

    /* 3. Mounting the erased partition reports an error... */
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERROR, rc);

    /* ...and a repeat attempt still fails: the failed mount did not
     * silently format the partition. */
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERROR, rc);

    /* 4. Only the explicit format entry point recovers the volume. */
    rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));
#endif
}

/* TASK-118: the explicit format entry points must keep working after the
 * no-format-on-mount-failure change (ESP-IDF 5.x register-format-unregister
 * trick retained in the backend). */
static void test_explicit_format_paths_still_work(void)
{
    reset_filesystem();

    int32_t rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));

    /* osal_rmfs() wipes the volume through the explicit format path.
     * ESP: the partition stays present and formatted, so it remounts.
     * POSIX: rmfs() removes the backing image, so the mount must now fail
     * (no implicit re-creation) and only a new explicit mkfs() recovers it. */
    rc = osal_rmfs(TEST_IMAGE_PATH);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

#ifdef ESP_PLATFORM
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));
#else
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERROR, rc);

    rc = osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, TEST_BLOCK_SIZE, TEST_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    rc = osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_unmount(TEST_MOUNT_POINT));
#endif

    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, osal_rmfs(TEST_IMAGE_PATH));
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
    RUN_TEST(test_mount_failure_preserves_contents);
    RUN_TEST(test_explicit_format_paths_still_work);

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
