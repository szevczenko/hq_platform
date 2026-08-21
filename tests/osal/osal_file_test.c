/*
 * OSAL Filesystem Tests
 *
 * Tests:
 * 1.  Open, write, read, close (basic round-trip)
 * 2.  Read/Write with NULL buffer (error handling)
 * 3.  Read/Write with zero size (error handling)
 * 4.  Close an invalid file descriptor
 * 5.  Seek operations (SET, CUR, END)
 * 6.  File stat
 * 7.  File truncate
 * 8.  File allocate
 * 9.  Chmod
 * 10. Remove
 * 11. Rename
 * 12. Copy (osal_cp)
 * 13. Move (osal_mv)
 * 14. fd_get_info on an open file
 * 15. file_open_check
 * 16. Path validation (too long, empty, NULL)
 * 17. Read at EOF returns 0
 * 18. Open with different access modes
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_file.h"
#include "osal_mount.h"
#include "unity.h"

/* Test filesystem image/mount */
#ifdef ESP_PLATFORM
#define TEST_IMAGE_PATH  "flash_test"
#define TEST_MOUNT_POINT "/littlefs"
#else
#define TEST_IMAGE_PATH  "/tmp/osal_file_test.img"
#define TEST_MOUNT_POINT "/"
#endif

/* Test file paths inside mounted littlefs */
#define TEST_FILE "/test_file.bin"
#define TEST_FILE2 "/test_file2.bin"
#define TEST_FILE3 "/test_file3.bin"

static bool is_not_found_status(int32_t rc)
{
    return (rc == OSAL_ERROR || rc == OSAL_FS_ERR_PATH_INVALID);
}

static void setup_test_fs(void)
{
    (void)osal_unmount(TEST_MOUNT_POINT);
    (void)osal_rmfs(TEST_IMAGE_PATH);
    (void)osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U);
    (void)osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT);
    (void)osal_remove(TEST_FILE);
    (void)osal_remove(TEST_FILE2);
    (void)osal_remove(TEST_FILE3);
}

static void cleanup_test_fs(void)
{
    (void)osal_remove(TEST_FILE);
    (void)osal_remove(TEST_FILE2);
    (void)osal_remove(TEST_FILE3);
    (void)osal_unmount(TEST_MOUNT_POINT);
    (void)osal_rmfs(TEST_IMAGE_PATH);
}

/* ============================================================================
 * Test 1: Basic open, write, read, close round-trip
 * ========================================================================== */
static void test_open_write_read_close(void)
{
    const char write_data[] = "Hello, OSAL filesystem!";
    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    int32_t written = osal_write(fd, write_data, sizeof(write_data));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(write_data), written);

    int32_t pos = osal_lseek(fd, 0, OSAL_SEEK_SET);
    TEST_ASSERT_EQUAL_INT32(0, pos);

    int32_t bytes_read = osal_read(fd, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(write_data), bytes_read);
    TEST_ASSERT_EQUAL_MEMORY(write_data, read_buf, sizeof(write_data));

    int32_t rc = osal_close(fd);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
}

/* ============================================================================
 * Test 2: Read/Write with NULL buffer
 * ========================================================================== */
static void test_null_buffer(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    int32_t rc = osal_write(fd, NULL, 10);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_read(fd, NULL, 10);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 3: Read/Write with zero size
 * ========================================================================== */
static void test_zero_size(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    char buf[16] = {0};
    int32_t rc = osal_write(fd, buf, 0);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_SIZE, rc);

    rc = osal_read(fd, buf, 0);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_SIZE, rc);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 4: Close an invalid file descriptor
 * ========================================================================== */
static void test_close_invalid_fd(void)
{
    int32_t rc = osal_close(-1);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_ID, rc);

    rc = osal_close(9999);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_ID, rc);
}

/* ============================================================================
 * Test 5: Seek operations (SET, CUR, END)
 * ========================================================================== */
static void test_seek_operations(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    char data[100];
    memset(data, 'A', sizeof(data));
    (void)osal_write(fd, data, sizeof(data));

    int32_t pos = osal_lseek(fd, 10, OSAL_SEEK_SET);
    TEST_ASSERT_EQUAL_INT32(10, pos);

    pos = osal_lseek(fd, 5, OSAL_SEEK_CUR);
    TEST_ASSERT_EQUAL_INT32(15, pos);

    pos = osal_lseek(fd, 0, OSAL_SEEK_END);
    TEST_ASSERT_EQUAL_INT32(100, pos);

    pos = osal_lseek(-1, 0, OSAL_SEEK_SET);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_ID, pos);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 6: File stat
 * ========================================================================== */
static void test_stat(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    const char content[] = "stat test data";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    osal_fstat_t fstat_buf;
    int32_t rc = osal_stat(TEST_FILE, &fstat_buf);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(content), (int32_t)OSAL_FILESTAT_SIZE(fstat_buf));
    TEST_ASSERT_FALSE(OSAL_FILESTAT_ISDIR(fstat_buf));

    rc = osal_stat(NULL, &fstat_buf);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_stat(TEST_FILE, NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_stat("/nonexistent", &fstat_buf);
    TEST_ASSERT_TRUE(is_not_found_status(rc));
}

/* ============================================================================
 * Test 7: File truncate
 * ========================================================================== */
static void test_truncate(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    char data[200];
    memset(data, 'B', sizeof(data));
    (void)osal_write(fd, data, sizeof(data));

    int32_t rc = osal_file_truncate(fd, 50);
    TEST_ASSERT_TRUE(rc == OSAL_SUCCESS || rc == OSAL_ERR_OPERATION_NOT_SUPPORTED);

    if (rc == OSAL_SUCCESS)
    {
        int32_t pos = osal_lseek(fd, 0, OSAL_SEEK_END);
        TEST_ASSERT_EQUAL_INT32(50, pos);
    }

    rc = osal_file_truncate(-1, 10);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_ID, rc);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 8: File allocate
 * ========================================================================== */
static void test_allocate(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    int32_t rc = osal_file_allocate(fd, 0, 4096);
    TEST_ASSERT_TRUE(rc == OSAL_SUCCESS || rc == OSAL_ERR_OPERATION_NOT_SUPPORTED);

    rc = osal_file_allocate(-1, 0, 4096);
    TEST_ASSERT_TRUE(rc == OSAL_ERR_INVALID_ID || rc == OSAL_ERR_OPERATION_NOT_SUPPORTED);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 9: Chmod
 * ========================================================================== */
static void test_chmod(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);
    (void)osal_close(fd);

    int32_t rc = osal_chmod(TEST_FILE, OSAL_READ_ONLY);
    TEST_ASSERT_TRUE(rc == OSAL_SUCCESS || rc == OSAL_ERR_NOT_IMPLEMENTED);

    rc = osal_chmod(NULL, OSAL_READ_ONLY);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);
}

/* ============================================================================
 * Test 10: Remove
 * ========================================================================== */
static void test_remove(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE2,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);
    (void)osal_close(fd);

    int32_t rc = osal_remove(TEST_FILE2);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    osal_fstat_t fstat_buf;
    rc = osal_stat(TEST_FILE2, &fstat_buf);
    TEST_ASSERT_TRUE(is_not_found_status(rc));

    rc = osal_remove("/nonexistent");
    TEST_ASSERT_TRUE(is_not_found_status(rc));

    rc = osal_remove(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);
}

/* ============================================================================
 * Test 11: Rename
 * ========================================================================== */
static void test_rename(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE2,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);
    const char content[] = "rename data";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    int32_t rc = osal_rename(TEST_FILE2, TEST_FILE3);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    osal_fstat_t fstat_buf;
    rc = osal_stat(TEST_FILE2, &fstat_buf);
    TEST_ASSERT_TRUE(is_not_found_status(rc));

    rc = osal_stat(TEST_FILE3, &fstat_buf);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(content), (int32_t)OSAL_FILESTAT_SIZE(fstat_buf));

    rc = osal_rename(NULL, TEST_FILE3);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_rename(TEST_FILE3, NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    (void)osal_remove(TEST_FILE3);
}

/* ============================================================================
 * Test 12: Copy
 * ========================================================================== */
static void test_copy(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    const char content[] = "copy test payload with some data";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    int32_t rc = osal_cp(TEST_FILE, TEST_FILE2);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    fd = osal_open_create(TEST_FILE2, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    int32_t bytes_read = osal_read(fd, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(content), bytes_read);
    TEST_ASSERT_EQUAL_MEMORY(content, read_buf, sizeof(content));
    (void)osal_close(fd);

    rc = osal_cp(NULL, TEST_FILE2);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_cp(TEST_FILE, NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    (void)osal_remove(TEST_FILE2);
}

/* ============================================================================
 * Test 13: Move
 * ========================================================================== */
static void test_move(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    const char content[] = "move test payload";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    int32_t rc = osal_mv(TEST_FILE, TEST_FILE2);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    osal_fstat_t fstat_buf;
    rc = osal_stat(TEST_FILE, &fstat_buf);
    TEST_ASSERT_TRUE(is_not_found_status(rc));

    fd = osal_open_create(TEST_FILE2, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    int32_t bytes_read = osal_read(fd, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(content), bytes_read);
    TEST_ASSERT_EQUAL_MEMORY(content, read_buf, sizeof(content));
    (void)osal_close(fd);

    rc = osal_mv(NULL, TEST_FILE2);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    (void)osal_remove(TEST_FILE2);
}

/* ============================================================================
 * Test 14: fd_get_info on an open file
 * ========================================================================== */
static void test_fd_get_info(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    osal_file_prop_t prop;
    memset(&prop, 0, sizeof(prop));

    int32_t rc = osal_fd_get_info(fd, &prop);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);
    TEST_ASSERT_GREATER_THAN(0, (int32_t)strlen(prop.path));
    TEST_ASSERT_EQUAL_INT32((int32_t)prop.user, (int32_t)fd);

    rc = osal_fd_get_info(fd, NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);

    rc = osal_fd_get_info(-1, &prop);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERR_INVALID_ID, rc);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 15: file_open_check
 * ========================================================================== */
static void test_file_open_check(void)
{
    (void)osal_remove(TEST_FILE);

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    osal_file_prop_t prop;
    memset(&prop, 0, sizeof(prop));
    (void)osal_fd_get_info(fd, &prop);

    int32_t rc = osal_file_open_check(prop.path);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS, rc);

    (void)osal_close(fd);

    rc = osal_file_open_check(prop.path);
    TEST_ASSERT_EQUAL_INT32(OSAL_ERROR, rc);

    rc = osal_file_open_check(NULL);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, rc);
}

/* ============================================================================
 * Test 16: Path validation
 * ========================================================================== */
static void test_path_validation(void)
{
    osal_file_id_t fd = osal_open_create(NULL, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_EQUAL_INT32(OSAL_INVALID_POINTER, (int32_t)fd);

    fd = osal_open_create("", OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_EQUAL_INT32(OSAL_FS_ERR_PATH_INVALID, (int32_t)fd);

    char long_path[OSAL_MAX_PATH_LEN + 16];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    fd = osal_open_create(long_path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_EQUAL_INT32(OSAL_FS_ERR_PATH_TOO_LONG, (int32_t)fd);
}

/* ============================================================================
 * Test 17: Read at EOF returns 0
 * ========================================================================== */
static void test_read_eof(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT_GREATER_THAN(-1, (int32_t)fd);

    const char data[] = "short";
    (void)osal_write(fd, data, sizeof(data));

    (void)osal_lseek(fd, 0, OSAL_SEEK_END);

    char buf[32];
    int32_t bytes_read = osal_read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT32(0, bytes_read);

    (void)osal_close(fd);
}

/* ============================================================================
 * Test 18: Open with different access modes
 * ========================================================================== */
static void test_access_modes(void)
{
    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    TEST_ASSERT_GREATER_THAN_MESSAGE(-1, (int32_t)fd, "open file WRITE_ONLY");

    const char d[] = "mode test data";
    int32_t written = osal_write(fd, d, sizeof(d));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(d), written);

    (void)osal_close(fd);

    fd = osal_open_create(TEST_FILE, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT_GREATER_THAN_MESSAGE(-1, (int32_t)fd, "open file READ_ONLY");

    char buf[64];
    int32_t bytes_read = osal_read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(d), bytes_read);
    (void)osal_close(fd);
}

/* ============================================================================
 * Main Test Runner
 * ========================================================================== */

void osal_file_tests_run(void)
{
    setup_test_fs();

    RUN_TEST(test_open_write_read_close);
    RUN_TEST(test_null_buffer);
    RUN_TEST(test_zero_size);
    RUN_TEST(test_close_invalid_fd);
    RUN_TEST(test_seek_operations);
    RUN_TEST(test_stat);
    RUN_TEST(test_truncate);
    RUN_TEST(test_allocate);
    RUN_TEST(test_chmod);
    RUN_TEST(test_remove);
    RUN_TEST(test_rename);
    RUN_TEST(test_copy);
    RUN_TEST(test_move);
    RUN_TEST(test_fd_get_info);
    RUN_TEST(test_file_open_check);
    RUN_TEST(test_path_validation);
    RUN_TEST(test_read_eof);
    RUN_TEST(test_access_modes);

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
    printf("          OSAL Filesystem Tests                   \n");
    printf("==================================================\n");
    printf("\n");

    setup_test_fs();

    osal_file_tests_run();

    cleanup_test_fs();

#ifndef ESP_PLATFORM
    return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
