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

/* Test results tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test macros */
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

/* Test filesystem image/mount */
#define TEST_IMAGE_PATH "/tmp/osal_file_test.img"
#define TEST_MOUNT_POINT "/"

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
    /* Start from a clean file-backed image every run. */
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
    TEST_START("Open, Write, Read, Close");

    const char write_data[] = "Hello, OSAL filesystem!";
    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "osal_open_create returns valid fd");

    int32_t written = osal_write(fd, write_data, sizeof(write_data));
    TEST_ASSERT(written == (int32_t)sizeof(write_data),
                "osal_write returns correct byte count");

    int32_t pos = osal_lseek(fd, 0, OSAL_SEEK_SET);
    TEST_ASSERT(pos == 0, "osal_lseek SET to 0 returns 0");

    int32_t bytes_read = osal_read(fd, read_buf, sizeof(read_buf));
    TEST_ASSERT(bytes_read == (int32_t)sizeof(write_data),
                "osal_read returns correct byte count");
    TEST_ASSERT(memcmp(read_buf, write_data, sizeof(write_data)) == 0,
                "read data matches written data");

    int32_t rc = osal_close(fd);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_close succeeds");

    TEST_END();
}

/* ============================================================================
 * Test 2: Read/Write with NULL buffer
 * ========================================================================== */
static void test_null_buffer(void)
{
    TEST_START("Read/Write with NULL buffer");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for null buffer test");

    int32_t rc = osal_write(fd, NULL, 10);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_write(NULL) returns OSAL_INVALID_POINTER");

    rc = osal_read(fd, NULL, 10);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_read(NULL) returns OSAL_INVALID_POINTER");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 3: Read/Write with zero size
 * ========================================================================== */
static void test_zero_size(void)
{
    TEST_START("Read/Write with zero size");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for zero size test");

    char buf[16] = {0};
    int32_t rc = osal_write(fd, buf, 0);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_SIZE, "osal_write(size=0) returns OSAL_ERR_INVALID_SIZE");

    rc = osal_read(fd, buf, 0);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_SIZE, "osal_read(size=0) returns OSAL_ERR_INVALID_SIZE");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 4: Close an invalid file descriptor
 * ========================================================================== */
static void test_close_invalid_fd(void)
{
    TEST_START("Close invalid file descriptor");

    int32_t rc = osal_close(-1);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_ID, "osal_close(-1) returns OSAL_ERR_INVALID_ID");

    rc = osal_close(9999);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_ID, "osal_close(9999) returns OSAL_ERR_INVALID_ID");

    TEST_END();
}

/* ============================================================================
 * Test 5: Seek operations (SET, CUR, END)
 * ========================================================================== */
static void test_seek_operations(void)
{
    TEST_START("Seek operations");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for seek test");

    char data[100];
    memset(data, 'A', sizeof(data));
    (void)osal_write(fd, data, sizeof(data));

    int32_t pos = osal_lseek(fd, 10, OSAL_SEEK_SET);
    TEST_ASSERT(pos == 10, "SEEK_SET to offset 10");

    pos = osal_lseek(fd, 5, OSAL_SEEK_CUR);
    TEST_ASSERT(pos == 15, "SEEK_CUR +5 from 10 = 15");

    pos = osal_lseek(fd, 0, OSAL_SEEK_END);
    TEST_ASSERT(pos == 100, "SEEK_END offset 0 = file size (100)");

    pos = osal_lseek(-1, 0, OSAL_SEEK_SET);
    TEST_ASSERT(pos == OSAL_ERR_INVALID_ID, "osal_lseek(-1) returns OSAL_ERR_INVALID_ID");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 6: File stat
 * ========================================================================== */
static void test_stat(void)
{
    TEST_START("File stat");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for stat test");

    const char content[] = "stat test data";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    osal_fstat_t fstat_buf;
    int32_t rc = osal_stat(TEST_FILE, &fstat_buf);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_stat succeeds");
    TEST_ASSERT(OSAL_FILESTAT_SIZE(fstat_buf) == sizeof(content),
                "file size matches written data");
    TEST_ASSERT(!OSAL_FILESTAT_ISDIR(fstat_buf),
                "file is not a directory");

    rc = osal_stat(NULL, &fstat_buf);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_stat(NULL path) returns OSAL_INVALID_POINTER");

    rc = osal_stat(TEST_FILE, NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_stat(NULL buf) returns OSAL_INVALID_POINTER");

    rc = osal_stat("/nonexistent", &fstat_buf);
    TEST_ASSERT(is_not_found_status(rc), "osal_stat on nonexistent file returns not-found status");

    TEST_END();
}

/* ============================================================================
 * Test 7: File truncate
 * ========================================================================== */
static void test_truncate(void)
{
    TEST_START("File truncate");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for truncate test");

    char data[200];
    memset(data, 'B', sizeof(data));
    (void)osal_write(fd, data, sizeof(data));

    int32_t rc = osal_file_truncate(fd, 50);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_file_truncate to 50 succeeds");

    int32_t pos = osal_lseek(fd, 0, OSAL_SEEK_END);
    TEST_ASSERT(pos == 50, "file size after truncate is 50");

    rc = osal_file_truncate(-1, 10);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_ID, "osal_file_truncate(-1) returns OSAL_ERR_INVALID_ID");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 8: File allocate
 * ========================================================================== */
static void test_allocate(void)
{
    TEST_START("File allocate");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for allocate test");

    int32_t rc = osal_file_allocate(fd, 0, 4096);
    TEST_ASSERT(rc == OSAL_SUCCESS || rc == OSAL_ERR_OPERATION_NOT_SUPPORTED,
                "osal_file_allocate returns SUCCESS or NOT_SUPPORTED");

    rc = osal_file_allocate(-1, 0, 4096);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_ID || rc == OSAL_ERR_OPERATION_NOT_SUPPORTED,
                "osal_file_allocate(-1) returns INVALID_ID or NOT_SUPPORTED");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 9: Chmod
 * ========================================================================== */
static void test_chmod(void)
{
    TEST_START("Chmod");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for chmod test");
    (void)osal_close(fd);

    int32_t rc = osal_chmod(TEST_FILE, OSAL_READ_ONLY);
    TEST_ASSERT(rc == OSAL_SUCCESS || rc == OSAL_ERR_NOT_IMPLEMENTED,
                "osal_chmod returns SUCCESS or NOT_IMPLEMENTED");

    rc = osal_chmod(NULL, OSAL_READ_ONLY);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_chmod(NULL) returns OSAL_INVALID_POINTER");

    TEST_END();
}

/* ============================================================================
 * Test 10: Remove
 * ========================================================================== */
static void test_remove(void)
{
    TEST_START("Remove");

    osal_file_id_t fd = osal_open_create(TEST_FILE2,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    TEST_ASSERT(fd >= 0, "create file for remove test");
    (void)osal_close(fd);

    int32_t rc = osal_remove(TEST_FILE2);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_remove succeeds");

    osal_fstat_t fstat_buf;
    rc = osal_stat(TEST_FILE2, &fstat_buf);
    TEST_ASSERT(is_not_found_status(rc), "stat after remove returns not-found status");

    rc = osal_remove("/nonexistent");
    TEST_ASSERT(is_not_found_status(rc), "osal_remove on nonexistent returns not-found status");

    rc = osal_remove(NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_remove(NULL) returns OSAL_INVALID_POINTER");

    TEST_END();
}

/* ============================================================================
 * Test 11: Rename
 * ========================================================================== */
static void test_rename(void)
{
    TEST_START("Rename");

    osal_file_id_t fd = osal_open_create(TEST_FILE2,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    TEST_ASSERT(fd >= 0, "create file for rename test");
    const char content[] = "rename data";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    int32_t rc = osal_rename(TEST_FILE2, TEST_FILE3);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_rename succeeds");

    osal_fstat_t fstat_buf;
    rc = osal_stat(TEST_FILE2, &fstat_buf);
    TEST_ASSERT(is_not_found_status(rc), "old file no longer exists after rename");

    rc = osal_stat(TEST_FILE3, &fstat_buf);
    TEST_ASSERT(rc == OSAL_SUCCESS, "new file exists after rename");
    TEST_ASSERT(OSAL_FILESTAT_SIZE(fstat_buf) == sizeof(content),
                "renamed file has correct size");

    rc = osal_rename(NULL, TEST_FILE3);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_rename(NULL, ...) returns OSAL_INVALID_POINTER");

    rc = osal_rename(TEST_FILE3, NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_rename(..., NULL) returns OSAL_INVALID_POINTER");

    (void)osal_remove(TEST_FILE3);

    TEST_END();
}

/* ============================================================================
 * Test 12: Copy
 * ========================================================================== */
static void test_copy(void)
{
    TEST_START("Copy (osal_cp)");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "create source file for copy test");

    const char content[] = "copy test payload with some data";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    int32_t rc = osal_cp(TEST_FILE, TEST_FILE2);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_cp succeeds");

    fd = osal_open_create(TEST_FILE2, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT(fd >= 0, "open copied file for verification");

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    int32_t bytes_read = osal_read(fd, read_buf, sizeof(read_buf));
    TEST_ASSERT(bytes_read == (int32_t)sizeof(content),
                "copied file has correct byte count");
    TEST_ASSERT(memcmp(read_buf, content, sizeof(content)) == 0,
                "copied file content matches source");
    (void)osal_close(fd);

    rc = osal_cp(NULL, TEST_FILE2);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_cp(NULL, ...) returns OSAL_INVALID_POINTER");

    rc = osal_cp(TEST_FILE, NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_cp(..., NULL) returns OSAL_INVALID_POINTER");

    (void)osal_remove(TEST_FILE2);

    TEST_END();
}

/* ============================================================================
 * Test 13: Move
 * ========================================================================== */
static void test_move(void)
{
    TEST_START("Move (osal_mv)");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "create source file for move test");

    const char content[] = "move test payload";
    (void)osal_write(fd, content, sizeof(content));
    (void)osal_close(fd);

    int32_t rc = osal_mv(TEST_FILE, TEST_FILE2);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_mv succeeds");

    osal_fstat_t fstat_buf;
    rc = osal_stat(TEST_FILE, &fstat_buf);
    TEST_ASSERT(is_not_found_status(rc), "source file removed after move");

    fd = osal_open_create(TEST_FILE2, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT(fd >= 0, "open moved file for verification");

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    int32_t bytes_read = osal_read(fd, read_buf, sizeof(read_buf));
    TEST_ASSERT(bytes_read == (int32_t)sizeof(content),
                "moved file has correct byte count");
    TEST_ASSERT(memcmp(read_buf, content, sizeof(content)) == 0,
                "moved file content matches source");
    (void)osal_close(fd);

    rc = osal_mv(NULL, TEST_FILE2);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_mv(NULL, ...) returns OSAL_INVALID_POINTER");

    (void)osal_remove(TEST_FILE2);

    TEST_END();
}

/* ============================================================================
 * Test 14: fd_get_info on an open file
 * ========================================================================== */
static void test_fd_get_info(void)
{
    TEST_START("fd_get_info");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for fd_get_info test");

    osal_file_prop_t prop;
    memset(&prop, 0, sizeof(prop));

    int32_t rc = osal_fd_get_info(fd, &prop);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_fd_get_info succeeds");
    TEST_ASSERT(strlen(prop.path) > 0, "fd_get_info returns a non-empty path");
    TEST_ASSERT(prop.user == fd, "fd_get_info user field matches fd");

    rc = osal_fd_get_info(fd, NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_fd_get_info(NULL) returns OSAL_INVALID_POINTER");

    rc = osal_fd_get_info(-1, &prop);
    TEST_ASSERT(rc == OSAL_ERR_INVALID_ID, "osal_fd_get_info(-1) returns OSAL_ERR_INVALID_ID");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 15: file_open_check
 * ========================================================================== */
static void test_file_open_check(void)
{
    TEST_START("file_open_check");

    (void)osal_remove(TEST_FILE);

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for open_check test");

    osal_file_prop_t prop;
    memset(&prop, 0, sizeof(prop));
    (void)osal_fd_get_info(fd, &prop);

    int32_t rc = osal_file_open_check(prop.path);
    TEST_ASSERT(rc == OSAL_SUCCESS, "osal_file_open_check returns SUCCESS for open file");

    (void)osal_close(fd);

    rc = osal_file_open_check(prop.path);
    TEST_ASSERT(rc == OSAL_ERROR, "osal_file_open_check returns ERROR after close");

    rc = osal_file_open_check(NULL);
    TEST_ASSERT(rc == OSAL_INVALID_POINTER, "osal_file_open_check(NULL) returns OSAL_INVALID_POINTER");

    TEST_END();
}

/* ============================================================================
 * Test 16: Path validation
 * ========================================================================== */
static void test_path_validation(void)
{
    TEST_START("Path validation");

    osal_file_id_t fd = osal_open_create(NULL, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT(fd == (osal_file_id_t)OSAL_INVALID_POINTER,
                "osal_open_create(NULL) returns OSAL_INVALID_POINTER");

    fd = osal_open_create("", OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT(fd == (osal_file_id_t)OSAL_FS_ERR_PATH_INVALID,
                "osal_open_create('') returns OSAL_FS_ERR_PATH_INVALID");

    char long_path[OSAL_MAX_PATH_LEN + 16];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    fd = osal_open_create(long_path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT(fd == (osal_file_id_t)OSAL_FS_ERR_PATH_TOO_LONG,
                "osal_open_create(too_long) returns OSAL_FS_ERR_PATH_TOO_LONG");

    TEST_END();
}

/* ============================================================================
 * Test 17: Read at EOF returns 0
 * ========================================================================== */
static void test_read_eof(void)
{
    TEST_START("Read at EOF");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_READ_WRITE);
    TEST_ASSERT(fd >= 0, "open file for EOF test");

    const char data[] = "short";
    (void)osal_write(fd, data, sizeof(data));

    (void)osal_lseek(fd, 0, OSAL_SEEK_END);

    char buf[32];
    int32_t bytes_read = osal_read(fd, buf, sizeof(buf));
    TEST_ASSERT(bytes_read == 0, "read at EOF returns 0");

    (void)osal_close(fd);
    TEST_END();
}

/* ============================================================================
 * Test 18: Open with different access modes
 * ========================================================================== */
static void test_access_modes(void)
{
    TEST_START("Open with different access modes");

    osal_file_id_t fd = osal_open_create(TEST_FILE,
        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
        OSAL_WRITE_ONLY);
    TEST_ASSERT(fd >= 0, "open file WRITE_ONLY");

    const char data[] = "access mode test";
    int32_t written = osal_write(fd, data, sizeof(data));
    TEST_ASSERT(written == (int32_t)sizeof(data), "write succeeds in WRITE_ONLY mode");
    (void)osal_close(fd);

    fd = osal_open_create(TEST_FILE, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    TEST_ASSERT(fd >= 0, "open file READ_ONLY");

    char buf[64];
    int32_t bytes_read = osal_read(fd, buf, sizeof(buf));
    TEST_ASSERT(bytes_read == (int32_t)sizeof(data), "read succeeds in READ_ONLY mode");
    (void)osal_close(fd);

    TEST_END();
}

int osal_file_tests_run(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    printf("\n");
    printf("==================================================\n");
    printf("          OSAL Filesystem Tests                   \n");
    printf("==================================================\n");

    setup_test_fs();

    test_open_write_read_close();
    test_null_buffer();
    test_zero_size();
    test_close_invalid_fd();
    test_seek_operations();
    test_stat();
    test_truncate();
    test_allocate();
    test_chmod();
    test_remove();
    test_rename();
    test_copy();
    test_move();
    test_fd_get_info();
    test_file_open_check();
    test_path_validation();
    test_read_eof();
    test_access_modes();

    cleanup_test_fs();

    printf("\n");
    printf("==================================================\n");
    printf("                  TEST SUMMARY                    \n");
    printf("==================================================\n");
    printf("  Total tests:  %d\n", tests_run);
    printf("  Passed:       %d\n", tests_passed);
    printf("  Failed:       %d\n", tests_failed);
    printf("  Success rate: %.1f%%\n",
           (tests_run > 0) ? (100.0 * tests_passed / tests_run) : 0.0);
    printf("==================================================\n");

    if (tests_failed == 0) {
        printf("\nALL TESTS PASSED\n\n");
    } else {
        printf("\nSOME TESTS FAILED\n\n");
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
    int failed = osal_file_tests_run();

#ifndef ESP_PLATFORM
    return (failed == 0) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
