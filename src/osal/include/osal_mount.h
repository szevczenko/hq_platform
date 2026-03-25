#ifndef OSAL_MOUNT_H
#define OSAL_MOUNT_H

#include "osal_common_type.h"
#include "osal_error.h"

/**
 * @brief File system volume statistics
 *
 * Encapsulates size and available space information for a mounted file system
 * volume.
 */
typedef struct
{
    size_t block_size;
    size_t total_blocks;
    size_t blocks_free;
} osal_statvfs_t;

/**
 * @brief Create a file system on the target
 *
 * Creates a file system on the target. This is highly dependent on the
 * underlying OS and its volume table definition.
 *
 * @note The volname parameter of RAM disks should begin with the string RAM,
 *       for example RAMDISK, RAM0, or RAM1. The underlying implementation may
 *       use this to select the correct file system type or format and to
 *       differentiate RAM disks from physical disks.
 *
 * @param[in] address     The address at which to start the new disk. If
 *                        address is NULL, space will be allocated by the OS.
 * @param[in] devname     The underlying kernel device to use, if applicable @nonnull
 * @param[in] volname     The volume name @nonnull
 * @param[in] block_size  The size of a single block on the drive
 * @param[in] num_blocks  The number of blocks to allocate for the drive
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if devname or volname is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if devname or volname is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_mkfs(char *address, const char *devname, const char *volname, size_t block_size, size_t num_blocks);


/**
 * @brief Mount a file system
 *
 * Mounts a file system or block device at the given mount point.
 *
 * @param[in] devname      The name of the drive to mount, as used by osal_mkfs @nonnull
 * @param[in] mount_point  The mount point name @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if any argument is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the mount point string is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_mount(const char *devname, const char *mount_point);


/**
 * @brief Initialize an existing file system
 *
 * Initializes a file system on the target.
 *
 * @note The volname parameter of RAM disks should begin with the string RAM,
 *       for example RAMDISK, RAM0, or RAM1. The underlying implementation may
 *       use this to select the correct file system type or format and to
 *       differentiate RAM disks from physical disks.
 *
 * @param[in] address     The address at which to start the disk. If address is
 *                        NULL, space will be allocated by the OS.
 * @param[in] devname     The underlying kernel device to use, if applicable @nonnull
 * @param[in] volname     The volume name @nonnull
 * @param[in] block_size  The size of a single block on the drive
 * @param[in] num_blocks  The number of blocks to allocate for the drive
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if devname or volname are NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the name is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_initfs(char *address, const char *devname, const char *volname, size_t block_size, size_t num_blocks);


/**
 * @brief Remove a file system
 *
 * Removes or unmaps the target file system. This is not the same as
 * unmounting the file system.
 *
 * @param[in] devname  The generic drive name @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if devname is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if devname is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_rmfs(const char *devname);


/**
 * @brief Unmount a mounted file system
 *
 * Unmounts a drive from the file system.
 *
 * @note Any open file descriptors referencing this file system should be
 *       closed prior to unmounting the drive.
 *
 * @param[in] mount_point  The mount point to remove @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if mount_point is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the mount point is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_unmount(const char *mount_point);


/**
 * @brief Obtain volume size and free-space information
 *
 * Populates the supplied osal_statvfs_t structure with the block size and the
 * total and free block counts for a file system volume.
 *
 * @param[in]  name      The device or path to operate on @nonnull
 * @param[out] stat_buf  Output structure to populate @nonnull
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if name or stat_buf is NULL
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the name is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_filesys_stat_volume(const char *name, osal_statvfs_t *stat_buf);


/**
 * @brief Check and optionally repair a file system
 *
 * Checks the file system for inconsistencies and optionally repairs it.
 *
 * @note Not all operating systems implement this function. If the underlying
 *       OS does not provide this facility, OSAL_ERR_NOT_IMPLEMENTED is
 *       returned.
 *
 * @param[in] name    The device or path to operate on @nonnull
 * @param[in] repair  Whether to also repair inconsistencies
 *
 * @return Execution status, see osal_status_t
 * @retval OSAL_SUCCESS on success
 * @retval OSAL_INVALID_POINTER if name is NULL
 * @retval OSAL_ERR_NOT_IMPLEMENTED if the operation is not supported
 * @retval OSAL_FS_ERR_PATH_TOO_LONG if the name is too long
 * @retval OSAL_ERROR if an unexpected OS error occurs
 */
int32_t osal_chkfs(const char *name, bool repair);

#endif /* OSAL_MOUNT_H */