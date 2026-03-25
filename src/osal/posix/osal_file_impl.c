#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osal_file.h"
#include "osal_littlefs_backend.h"
#include "lfs.h"

#define OSAL_LFS_MAX_OPEN_FILES 32

typedef struct
{
    bool in_use;
    lfs_file_t file;
    char path[OSAL_MAX_PATH_LEN];
} osal_lfs_open_file_t;

static osal_lfs_open_file_t g_open_files[OSAL_LFS_MAX_OPEN_FILES];

static int lfs_fd_to_slot(osal_file_id_t filedes)
{
    int slot = (int)filedes - 1;
    if (slot < 0 || slot >= OSAL_LFS_MAX_OPEN_FILES)
    {
        return -1;
    }
    if (!g_open_files[slot].in_use)
    {
        return -1;
    }
    return slot;
}

static int alloc_slot(void)
{
    for (int i = 0; i < OSAL_LFS_MAX_OPEN_FILES; ++i)
    {
        if (!g_open_files[i].in_use)
        {
            return i;
        }
    }
    return -1;
}

static int access_to_lfs_flags(os_file_access_t access_mode)
{
    switch (access_mode)
    {
        case OSAL_WRITE_ONLY:
            return LFS_O_WRONLY;
        case OSAL_READ_WRITE:
            return LFS_O_RDWR;
        case OSAL_READ_ONLY:
        default:
            return LFS_O_RDONLY;
    }
}

static int seek_to_lfs(osal_file_seek_t whence)
{
    switch (whence)
    {
        case OSAL_SEEK_CUR:
            return LFS_SEEK_CUR;
        case OSAL_SEEK_END:
            return LFS_SEEK_END;
        case OSAL_SEEK_SET:
        default:
            return LFS_SEEK_SET;
    }
}

static uint32_t lfs_mode_to_osal(uint8_t type)
{
    return (type == LFS_TYPE_DIR) ? OSAL_FILESTAT_MODE_DIR : (OSAL_FILESTAT_MODE_READ | OSAL_FILESTAT_MODE_WRITE);
}

static int32_t validate_io_buffer(const void *buffer, size_t nbytes)
{
    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0)
    {
        return OSAL_ERR_INVALID_SIZE;
    }
    if (!g_osal_lfs_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }
    return OSAL_SUCCESS;
}

osal_file_id_t osal_open_create(const char *path, osal_file_flag_t flags, os_file_access_t access_mode)
{
    if (path == NULL)
    {
        return (osal_file_id_t)OSAL_INVALID_POINTER;
    }
    if (!g_osal_lfs_mounted)
    {
        return (osal_file_id_t)OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    char norm_path[OSAL_MAX_PATH_LEN];
    int32_t rc = osal_lfs_path_normalize(path, norm_path, sizeof(norm_path));
    if (rc != OSAL_SUCCESS)
    {
        return (osal_file_id_t)rc;
    }

    int slot = alloc_slot();
    if (slot < 0)
    {
        return (osal_file_id_t)OSAL_ERR_NO_FREE_IDS;
    }

    int lfs_flags = access_to_lfs_flags(access_mode);
    if (flags & OSAL_FILE_FLAG_CREATE)
    {
        lfs_flags |= LFS_O_CREAT;
    }
    if (flags & OSAL_FILE_FLAG_TRUNCATE)
    {
        lfs_flags |= LFS_O_TRUNC;
    }

    int err = lfs_file_open(&g_osal_lfs, &g_open_files[slot].file, norm_path, lfs_flags);
    if (err != 0)
    {
        return (osal_file_id_t)osal_lfs_map_error(err);
    }

    g_open_files[slot].in_use = true;
    strncpy(g_open_files[slot].path, norm_path, sizeof(g_open_files[slot].path) - 1);
    g_open_files[slot].path[sizeof(g_open_files[slot].path) - 1] = '\0';

    return (osal_file_id_t)(slot + 1);
}

int32_t osal_close(osal_file_id_t filedes)
{
    int slot = lfs_fd_to_slot(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    int err = lfs_file_close(&g_osal_lfs, &g_open_files[slot].file);
    g_open_files[slot].in_use = false;
    g_open_files[slot].path[0] = '\0';

    return osal_lfs_map_error(err);
}

int32_t osal_read(osal_file_id_t filedes, void *buffer, size_t nbytes)
{
    int32_t rc = validate_io_buffer(buffer, nbytes);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    int slot = lfs_fd_to_slot(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    lfs_ssize_t res = lfs_file_read(&g_osal_lfs, &g_open_files[slot].file, buffer, nbytes);
    if (res < 0)
    {
        return osal_lfs_map_error((int)res);
    }

    return (int32_t)res;
}

int32_t osal_write(osal_file_id_t filedes, const void *buffer, size_t nbytes)
{
    int32_t rc = validate_io_buffer(buffer, nbytes);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    int slot = lfs_fd_to_slot(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    lfs_ssize_t res = lfs_file_write(&g_osal_lfs, &g_open_files[slot].file, buffer, nbytes);
    if (res < 0)
    {
        return osal_lfs_map_error((int)res);
    }

    return (int32_t)res;
}

int32_t osal_file_allocate(osal_file_id_t filedes, uint32_t offset, uint32_t len)
{
    (void)filedes;
    (void)offset;
    (void)len;
    return OSAL_ERR_OPERATION_NOT_SUPPORTED;
}

int32_t osal_file_truncate(osal_file_id_t filedes, uint32_t len)
{
    int slot = lfs_fd_to_slot(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    int err = lfs_file_truncate(&g_osal_lfs, &g_open_files[slot].file, len);
    return osal_lfs_map_error(err);
}

int32_t osal_chmod(const char *path, os_file_access_t access_mode)
{
    if (path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    (void)access_mode;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

int32_t osal_stat(const char *path, osal_fstat_t *filestats)
{
    if (path == NULL || filestats == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (!g_osal_lfs_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    char norm_path[OSAL_MAX_PATH_LEN];
    int32_t rc = osal_lfs_path_normalize(path, norm_path, sizeof(norm_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    struct lfs_info info;
    int err = lfs_stat(&g_osal_lfs, norm_path, &info);
    if (err < 0)
    {
        return osal_lfs_map_error(err);
    }

    filestats->file_mode_bits = lfs_mode_to_osal(info.type);
    filestats->file_size = info.size;
    filestats->file_time.tv_sec = 0;
    filestats->file_time.tv_nsec = 0;
    return OSAL_SUCCESS;
}

int32_t osal_lseek(osal_file_id_t filedes, uint32_t offset, osal_file_seek_t whence)
{
    int slot = lfs_fd_to_slot(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    lfs_soff_t res = lfs_file_seek(&g_osal_lfs, &g_open_files[slot].file, (lfs_soff_t)offset, seek_to_lfs(whence));
    if (res < 0)
    {
        return osal_lfs_map_error((int)res);
    }
    return (int32_t)res;
}

int32_t osal_remove(const char *path)
{
    if (path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (!g_osal_lfs_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    char norm_path[OSAL_MAX_PATH_LEN];
    int32_t rc = osal_lfs_path_normalize(path, norm_path, sizeof(norm_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    int err = lfs_remove(&g_osal_lfs, norm_path);
    return osal_lfs_map_error(err);
}

int32_t osal_rename(const char *old_filename, const char *new_filename)
{
    if (old_filename == NULL || new_filename == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (!g_osal_lfs_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    char old_path[OSAL_MAX_PATH_LEN];
    char new_path[OSAL_MAX_PATH_LEN];

    int32_t rc = osal_lfs_path_normalize(old_filename, old_path, sizeof(old_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    rc = osal_lfs_path_normalize(new_filename, new_path, sizeof(new_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    int err = lfs_rename(&g_osal_lfs, old_path, new_path);
    return osal_lfs_map_error(err);
}

int32_t osal_cp(const char *src, const char *dest)
{
    if (src == NULL || dest == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    osal_file_id_t src_fd = osal_open_create(src, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    if (src_fd < 0)
    {
        return src_fd;
    }

    osal_file_id_t dst_fd = osal_open_create(dest, OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE, OSAL_WRITE_ONLY);
    if (dst_fd < 0)
    {
        (void)osal_close(src_fd);
        return dst_fd;
    }

    int32_t status = OSAL_SUCCESS;
    char buf[512];

    while (true)
    {
        int32_t nread = osal_read(src_fd, buf, sizeof(buf));
        if (nread < 0)
        {
            status = nread;
            break;
        }
        if (nread == 0)
        {
            break;
        }

        size_t offset = 0;
        while (offset < (size_t)nread)
        {
            int32_t nwritten = osal_write(dst_fd, buf + offset, (size_t)nread - offset);
            if (nwritten < 0)
            {
                status = nwritten;
                break;
            }
            offset += (size_t)nwritten;
        }

        if (status != OSAL_SUCCESS)
        {
            break;
        }
    }

    (void)osal_close(src_fd);
    (void)osal_close(dst_fd);
    return status;
}

int32_t osal_mv(const char *src, const char *dest)
{
    int32_t rc = osal_rename(src, dest);
    if (rc == OSAL_SUCCESS)
    {
        return OSAL_SUCCESS;
    }

    rc = osal_cp(src, dest);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    return osal_remove(src);
}

int32_t osal_fd_get_info(osal_file_id_t filedes, osal_file_prop_t *fd_prop)
{
    if (fd_prop == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    int slot = lfs_fd_to_slot(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    strncpy(fd_prop->path, g_open_files[slot].path, sizeof(fd_prop->path) - 1);
    fd_prop->path[sizeof(fd_prop->path) - 1] = '\0';
    fd_prop->user = filedes;

    return OSAL_SUCCESS;
}

int32_t osal_file_open_check(const char *filename)
{
    if (filename == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    char norm_path[OSAL_MAX_PATH_LEN];
    int32_t rc = osal_lfs_path_normalize(filename, norm_path, sizeof(norm_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    for (int i = 0; i < OSAL_LFS_MAX_OPEN_FILES; ++i)
    {
        if (g_open_files[i].in_use && strcmp(g_open_files[i].path, norm_path) == 0)
        {
            return OSAL_SUCCESS;
        }
    }

    return OSAL_ERROR;
}
