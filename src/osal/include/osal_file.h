#ifndef OSAL_FILE_H
#define OSAL_FILE_H

#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_time.h"
#include "osal_impl_file.h"
#include <stdint.h>

#ifndef OSAL_MAX_PATH_LEN
#define OSAL_MAX_PATH_LEN 128 /**< Maximum length of a file path, including null terminator */
#endif

typedef enum
{
    OSAL_READ_ONLY  = 0x00, /**< Read access */
    OSAL_WRITE_ONLY = 0x01, /**< Write access */
    OSAL_READ_WRITE = 0x02  /**< Read and write access */
} os_file_access_t;

typedef enum
{
    OSAL_SEEK_SET = 0, /**< Seek offset set */
    OSAL_SEEK_CUR = 1, /**< Seek offset current */
    OSAL_SEEK_END = 2, /**< Seek offset end */
} osal_file_seek_t;

/** @brief OSAL file properties */

typedef struct
{
    char           path[OSAL_MAX_PATH_LEN];
    osal_file_id_t user;
} osal_file_prop_t;

typedef struct
{
    uint32_t  file_mode_bits;
    osal_time_t file_time;
    size_t      file_size;
} osal_fstat_t;

enum
{
    OSAL_FILESTAT_MODE_EXEC  = 0x00001,
    OSAL_FILESTAT_MODE_WRITE = 0x00002,
    OSAL_FILESTAT_MODE_READ  = 0x00004,
    OSAL_FILESTAT_MODE_DIR   = 0x10000
};

/** @brief Access file stat mode bits */
#define OSAL_FILESTAT_MODE(x) ((x).file_mode_bits)
/** @brief File stat is directory logical */
#define OSAL_FILESTAT_ISDIR(x) ((x).file_mode_bits & OSAL_FILESTAT_MODE_DIR)
/** @brief File stat is executable logical */
#define OSAL_FILESTAT_EXEC(x) ((x).file_mode_bits & OSAL_FILESTAT_MODE_EXEC)
/** @brief File stat is write enabled logical */
#define OSAL_FILESTAT_WRITE(x) ((x).file_mode_bits & OSAL_FILESTAT_MODE_WRITE)
/** @brief File stat is read enabled logical */
#define OSAL_FILESTAT_READ(x) ((x).file_mode_bits & OSAL_FILESTAT_MODE_READ)
/** @brief Access file stat size field */
#define OSAL_FILESTAT_SIZE(x) ((x).file_size)
/** @brief Access file stat time field as a whole number of seconds */
#define OSAL_FILESTAT_TIME(x) (osal_time_get_total_seconds((x).file_time))

/**
 * @brief Flags that can be used with opening of a file (bitmask)
 */
typedef enum
{
    OSAL_FILE_FLAG_NONE     = 0x00,
    OSAL_FILE_FLAG_CREATE   = 0x01,
    OSAL_FILE_FLAG_TRUNCATE = 0x02
} osal_file_flag_t;

/**
 * @brief Open or create a file
 *
 * Opens an existing file or creates a new one and returns an OSAL file handle
 * that can be used with other OSAL file APIs.
 *
 * @param[in]  path        File name to create or open @nonnull
 * @param[in]  flags       File creation/open flags - see osal_file_flag_t
 * @param[in]  access_mode Intended access mode - see os_file_access_t
 *
 * @return The file handle identifier.
 * @retval OSAL_OBJECT_ID_UNDEFINED if the file could not be opened or created
 */
osal_file_id_t osal_open_create(const char *path, osal_file_flag_t flags, os_file_access_t access_mode);


/**
 * @brief Closes an open file handle
 *
 * This closes regular file handles and any other file-like resource, such as
 * network streams or pipes.
 *
 * @param[in] filedes   The handle ID to operate on
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERR_INVALID_ID if the file descriptor passed in is invalid
 * @retval OSAL_ERROR if an unexpected/unhandled error occurs
 */
int32_t osal_close(osal_file_id_t filedes);


/**
 * @brief Read from a file handle
 *
 * Reads up to nbytes from a file, and puts them into buffer.
 *
 * If the file position is at the end of file (or beyond, if the OS allows) then this
 * function will return 0.
 *
 * @param[in]  filedes  The handle ID to operate on
 * @param[out] buffer   Storage location for file data @nonnull
 * @param[in]  nbytes   Maximum number of bytes to read @nonzero
 *
 * @note All OSAL error codes are negative int32_t values.  Failure of this
 * call can be checked by testing if the result is less than 0.
 *
 * @return A non-negative byte count or appropriate error code, see osal_status_t
 * @retval OSAL_INVALID_POINTER if buffer is a null pointer
 * @retval OSAL_ERR_INVALID_SIZE if the passed-in size is not valid
 * @retval OSAL_ERROR if OS call failed
 * @retval OSAL_ERR_INVALID_ID if the file descriptor passed in is invalid
 * @retval 0 if at end of file/stream data
 */
int32_t osal_read(osal_file_id_t filedes, void *buffer, size_t nbytes);


/**
 * @brief Write to a file handle
 *
 * Writes to a file. copies up to a maximum of nbytes of buffer to the file
 * described in filedes
 *
 * @param[in] filedes   The handle ID to operate on
 * @param[in] buffer    Source location for file data @nonnull
 * @param[in] nbytes    Maximum number of bytes to write @nonzero
 *
 * @note All OSAL error codes are negative int32_t values.  Failure of this
 * call can be checked by testing if the result is less than 0.
 *
 * @return A non-negative byte count or appropriate error code, see osal_status_t
 * @retval OSAL_INVALID_POINTER if buffer is NULL
 * @retval OSAL_ERR_INVALID_SIZE if the passed-in size is not valid
 * @retval OSAL_ERROR if OS call failed
 * @retval OSAL_ERR_INVALID_ID if the file descriptor passed in is invalid
 * @retval 0 if file/stream cannot accept any more data
 */
int32_t osal_write(osal_file_id_t filedes, const void *buffer, size_t nbytes);


/**
 * @brief Pre-allocates space at the given file location
 *
 * Instructs the underlying OS/Filesystem to pre-allocate filesystem blocks
 * for the given file location and length.  After this, future writes into the
 * same file area will not fail due to lack of space.
 *
 * @param[in] filedes   The handle ID to operate on
 * @param[in] offset    The offset within the file
 * @param[in] len       The length of space to pre-allocate
 *
 * @note Some file systems do not implement this capability
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERR_OUTPUT_TOO_LARGE if this would cause the file to be too large
 * @retval OSAL_ERR_OPERATION_NOT_SUPPORTED if the filesystem does not support this
 */
int32_t osal_file_allocate(osal_file_id_t filedes, uint32_t offset, uint32_t len);


/**
 * @brief Changes the size of the file
 *
 * This changes the size of the file on disk to the specified value.  It may either
 * extend or truncate the file.
 *
 * @note No data is written to the extended file area when a file is grown using
 * this API call.  Depending on the file system in use, data blocks may not be
 * allocated at the same time (a sparse file).  Data blocks are allocated
 * at the time file data is written into the extended area.
 *
 * @param[in] filedes   The handle ID to operate on
 * @param[in] len       The desired length of the file
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERR_OUTPUT_TOO_LARGE if this would cause the file to be too large
 */
int32_t osal_file_truncate(osal_file_id_t filedes, uint32_t len);


/**
 * @brief Changes the permissions of a file
 *
 * @param[in] path        File to change @nonnull
 * @param[in] access_mode Desired access mode - see os_file_access_t
 *
 * @note Some file systems do not implement permissions.  If the underlying OS
 *       does not support this operation, then OSAL_ERR_NOT_IMPLEMENTED is returned.
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERR_NOT_IMPLEMENTED if the filesystem does not support this call
 * @retval OSAL_INVALID_POINTER if the path argument is NULL
 */
int32_t osal_chmod(const char *path, os_file_access_t access_mode);


/**
 * @brief Obtain information about a file or directory
 *
 * Returns information about a file or directory in an osal_fstat_t structure
 *
 * @param[in]  path      The file to operate on @nonnull
 * @param[out] filestats Buffer to store file information @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if path or filestats is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the path is too long to be stored locally
 * @retval OSAL_FS_ERR_NAME_TOO_LONG if the name of the file is too long to be stored
 * @retval OSAL_FS_ERR_PATH_INVALID if path cannot be parsed
 * @retval OSAL_ERROR if the OS call failed
 */
int32_t osal_stat(const char *path, osal_fstat_t *filestats);


/**
 * @brief Seeks to the specified position of an open file
 *
 * Sets the read/write pointer to a specific offset in a specific file.
 *
 * @param[in] filedes   The handle ID to operate on
 * @param[in] offset    The file offset to seek to
 * @param[in] whence    The reference point for offset, see osal_file_seek_t
 *
 * @return Byte offset from the beginning of the file or appropriate error code,
 *         see osal_status_t
 * @retval OSAL_ERR_INVALID_ID if the file descriptor passed in is invalid
 * @retval OSAL_ERROR if OS call failed
 */
int32_t osal_lseek(osal_file_id_t filedes, uint32_t offset, osal_file_seek_t whence);


/**
 * @brief Removes a file from the file system
 *
 * Removes a given filename from the drive
 *
 * @note The behavior of this API on an open file is not defined at the OSAL level
 * due to dependencies on the underlying OS which may or may not allow the related
 * operation based on a variety of potential configurations.  For portability,
 * it is recommended that applications ensure the file is closed prior to removal.
 *
 * @param[in]  path      The file to operate on @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERROR if there is no device or the driver returns error
 * @retval OSAL_INVALID_POINTER if path is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if path is too long to be stored locally
 * @retval OSAL_FS_ERR_PATH_INVALID if path cannot be parsed
 * @retval OSAL_FS_ERR_NAME_TOO_LONG if the name of the file to remove is too long
 */
int32_t osal_remove(const char *path);


/**
 * @brief Renames a file
 *
 * Changes the name of a file, where the source and destination
 * reside on the same file system.
 *
 * @note The behavior of this API on an open file is not defined at the OSAL level
 * due to dependencies on the underlying OS which may or may not allow the related
 * operation based on a variety of potential configurations.  For portability,
 * it is recommended that applications ensure the file is closed prior to removal.
 *
 * @param[in]  old_filename      The original filename @nonnull
 * @param[in]  new_filename      The desired filename @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERROR if the file could not be opened or renamed
 * @retval OSAL_INVALID_POINTER if old_filename or new_filename are NULL
 * @retval OSAL_FS_ERR_PATH_INVALID if path cannot be parsed
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the paths given are too long to be stored locally
 * @retval OSAL_FS_ERR_NAME_TOO_LONG if the new name is too long to be stored locally
 */
int32_t osal_rename(const char *old_filename, const char *new_filename);


/**
 * @brief Copies a single file from src to dest
 *
 * @note The behavior of this API on an open file is not defined at the OSAL level
 * due to dependencies on the underlying OS which may or may not allow the related
 * operation based on a variety of potential configurations.  For portability,
 * it is recommended that applications ensure the file is closed prior to removal.
 *
 * @param[in]  src       The source file to operate on @nonnull
 * @param[in]  dest      The destination file @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERROR if the file could not be accessed
 * @retval OSAL_INVALID_POINTER if src or dest are NULL
 * @retval OSAL_FS_ERR_PATH_INVALID if path cannot be parsed
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the paths given are too long to be stored locally
 * @retval OSAL_FS_ERR_NAME_TOO_LONG if the dest name is too long to be stored locally
 */
int32_t osal_cp(const char *src, const char *dest);


/**
 * @brief Move a single file from src to dest
 *
 * This first attempts to rename the file, which is faster if
 * the source and destination reside on the same file system.
 *
 * If this fails, it falls back to copying the file and removing
 * the original.
 *
 * @note The behavior of this API on an open file is not defined at the OSAL level
 * due to dependencies on the underlying OS which may or may not allow the related
 * operation based on a variety of potential configurations.  For portability,
 * it is recommended that applications ensure the file is closed prior to removal.
 *
 * @param[in]  src       The source file to operate on @nonnull
 * @param[in]  dest      The destination file @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERROR if the file could not be renamed
 * @retval OSAL_INVALID_POINTER if src or dest are NULL
 * @retval OSAL_FS_ERR_PATH_INVALID if path cannot be parsed
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the paths given are too long to be stored locally
 * @retval OSAL_FS_ERR_NAME_TOO_LONG if the dest name is too long to be stored locally
 */
int32_t osal_mv(const char *src, const char *dest);


/**
 * @brief Obtain information about an open file
 *
 * Copies the information of the given file descriptor into a structure passed in
 *
 * @param[in]  filedes  The handle ID to operate on
 * @param[out] fd_prop  Storage buffer for file information @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_ERR_INVALID_ID if the file descriptor passed in is invalid
 * @retval OSAL_INVALID_POINTER if the fd_prop argument is NULL
 */
int32_t osal_fd_get_info(osal_file_id_t filedes, osal_file_prop_t *fd_prop);


/**
 * @brief Checks to see if a file is open
 *
 * This function takes a filename and determines if the file is open. The function
 * will return success if the file is open.
 *
 * @param[in]  filename      The file to operate on @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS if the file is open
 * @retval OSAL_ERROR if the file is not open
 * @retval OSAL_INVALID_POINTER if the filename argument is NULL
 */
int32_t osal_file_open_check(const char *filename);

#endif /* OSAL_FILE_H */