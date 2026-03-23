#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osal_file.h"
#include "osal_littlefs_backend.h"

#define OSAL_MAX_OPEN_FILES 32

typedef struct
{
    bool in_use;
    int fd;
    char path[OSAL_MAX_PATH_LEN];
} osal_open_fd_t;

static osal_open_fd_t g_open_fds[OSAL_MAX_OPEN_FILES];

static int32_t validate_path(const char *path)
{
    if (path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (path[0] == '\0')
    {
        return OSAL_FS_ERR_PATH_INVALID;
    }
    if (strlen(path) >= OSAL_MAX_PATH_LEN)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }
    if (!g_osal_lfs_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }
    return OSAL_SUCCESS;
}

static int find_slot_by_fd(int fd)
{
    for (int i = 0; i < OSAL_MAX_OPEN_FILES; ++i)
    {
        if (g_open_fds[i].in_use && g_open_fds[i].fd == fd)
        {
            return i;
        }
    }
    return -1;
}

static int alloc_slot(void)
{
    for (int i = 0; i < OSAL_MAX_OPEN_FILES; ++i)
    {
        if (!g_open_fds[i].in_use)
        {
            return i;
        }
    }
    return -1;
}

static int access_to_posix(os_file_access_t access_mode)
{
    switch (access_mode)
    {
        case OSAL_WRITE_ONLY:
            return O_WRONLY;
        case OSAL_READ_WRITE:
            return O_RDWR;
        case OSAL_READ_ONLY:
        default:
            return O_RDONLY;
    }
}

static int seek_to_posix(osal_file_seek_t whence)
{
    switch (whence)
    {
        case OSAL_SEEK_CUR:
            return SEEK_CUR;
        case OSAL_SEEK_END:
            return SEEK_END;
        case OSAL_SEEK_SET:
        default:
            return SEEK_SET;
    }
}

static uint32_t posix_mode_to_osal(mode_t mode)
{
    uint32_t osal_mode = 0;

    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
    {
        osal_mode |= OSAL_FILESTAT_MODE_EXEC;
    }
    if (mode & (S_IWUSR | S_IWGRP | S_IWOTH))
    {
        osal_mode |= OSAL_FILESTAT_MODE_WRITE;
    }
    if (mode & (S_IRUSR | S_IRGRP | S_IROTH))
    {
        osal_mode |= OSAL_FILESTAT_MODE_READ;
    }
    if (S_ISDIR(mode))
    {
        osal_mode |= OSAL_FILESTAT_MODE_DIR;
    }
    return osal_mode;
}

osal_file_id_t osal_open_create(const char *path, osal_file_flag_t flags, os_file_access_t access_mode)
{
    int32_t rc = validate_path(path);
    if (rc != OSAL_SUCCESS)
    {
        return (osal_file_id_t)rc;
    }

    char vfs_path[OSAL_MAX_PATH_LEN];
    rc = osal_lfs_build_vfs_path(path, vfs_path, sizeof(vfs_path));
    if (rc != OSAL_SUCCESS)
    {
        return (osal_file_id_t)rc;
    }

    int posix_flags = access_to_posix(access_mode);
    if (flags & OSAL_FILE_FLAG_CREATE)
    {
        posix_flags |= O_CREAT;
    }
    if (flags & OSAL_FILE_FLAG_TRUNCATE)
    {
        posix_flags |= O_TRUNC;
    }

    int fd = open(vfs_path, posix_flags, 0664);
    if (fd < 0)
    {
        return (osal_file_id_t)OSAL_ERROR;
    }

    int slot = alloc_slot();
    if (slot >= 0)
    {
        g_open_fds[slot].in_use = true;
        g_open_fds[slot].fd = fd;
        strncpy(g_open_fds[slot].path, vfs_path, sizeof(g_open_fds[slot].path) - 1);
        g_open_fds[slot].path[sizeof(g_open_fds[slot].path) - 1] = '\0';
    }

    return (osal_file_id_t)fd;
}

int32_t osal_close(osal_file_id_t filedes)
{
    if (filedes < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    int slot = find_slot_by_fd(filedes);
    int rc = close(filedes);
    if (slot >= 0)
    {
        g_open_fds[slot].in_use = false;
        g_open_fds[slot].fd = -1;
        g_open_fds[slot].path[0] = '\0';
    }

    return (rc == 0) ? OSAL_SUCCESS : OSAL_ERROR;
}

int32_t osal_read(osal_file_id_t filedes, void *buffer, size_t nbytes)
{
    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0)
    {
        return OSAL_ERR_INVALID_SIZE;
    }

    ssize_t result = read(filedes, buffer, nbytes);
    if (result < 0)
    {
        return (errno == EBADF) ? OSAL_ERR_INVALID_ID : OSAL_ERROR;
    }

    return (int32_t)result;
}

int32_t osal_write(osal_file_id_t filedes, const void *buffer, size_t nbytes)
{
    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0)
    {
        return OSAL_ERR_INVALID_SIZE;
    }

    ssize_t result = write(filedes, buffer, nbytes);
    if (result < 0)
    {
        return (errno == EBADF) ? OSAL_ERR_INVALID_ID : OSAL_ERROR;
    }

    return (int32_t)result;
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
    int rc = ftruncate(filedes, (off_t)len);
    if (rc != 0)
    {
        return (errno == EBADF) ? OSAL_ERR_INVALID_ID : OSAL_ERROR;
    }
    return OSAL_SUCCESS;
}

int32_t osal_chmod(const char *path, os_file_access_t access_mode)
{
    int32_t rc = validate_path(path);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    char vfs_path[OSAL_MAX_PATH_LEN];
    rc = osal_lfs_build_vfs_path(path, vfs_path, sizeof(vfs_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    mode_t posix_mode = 0;
    switch (access_mode)
    {
        case OSAL_READ_ONLY:
            posix_mode = S_IRUSR | S_IRGRP | S_IROTH;
            break;
        case OSAL_WRITE_ONLY:
            posix_mode = S_IWUSR | S_IWGRP | S_IWOTH;
            break;
        case OSAL_READ_WRITE:
            posix_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
            break;
        default:
            return OSAL_ERR_INVALID_ARGUMENT;
    }

    return (chmod(vfs_path, posix_mode) == 0) ? OSAL_SUCCESS : OSAL_ERROR;
}

int32_t osal_stat(const char *path, osal_fstat_t *filestats)
{
    if (filestats == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    int32_t rc = validate_path(path);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    char vfs_path[OSAL_MAX_PATH_LEN];
    rc = osal_lfs_build_vfs_path(path, vfs_path, sizeof(vfs_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    struct stat posix_st;
    if (stat(vfs_path, &posix_st) != 0)
    {
        return OSAL_ERROR;
    }

    filestats->file_mode_bits = posix_mode_to_osal(posix_st.st_mode);
    filestats->file_size = (size_t)posix_st.st_size;
    filestats->file_time.tv_sec = (int64_t)posix_st.st_mtime;
    filestats->file_time.tv_nsec = 0;
    return OSAL_SUCCESS;
}

int32_t osal_lseek(osal_file_id_t filedes, uint32_t offset, osal_file_seek_t whence)
{
    off_t result = lseek(filedes, (off_t)offset, seek_to_posix(whence));
    if (result < 0)
    {
        return (errno == EBADF) ? OSAL_ERR_INVALID_ID : OSAL_ERROR;
    }
    return (int32_t)result;
}

int32_t osal_remove(const char *path)
{
    int32_t rc = validate_path(path);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    char vfs_path[OSAL_MAX_PATH_LEN];
    rc = osal_lfs_build_vfs_path(path, vfs_path, sizeof(vfs_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    return (remove(vfs_path) == 0) ? OSAL_SUCCESS : OSAL_ERROR;
}

int32_t osal_rename(const char *old_filename, const char *new_filename)
{
    int32_t rc = validate_path(old_filename);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    rc = validate_path(new_filename);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    char old_path[OSAL_MAX_PATH_LEN];
    char new_path[OSAL_MAX_PATH_LEN];

    rc = osal_lfs_build_vfs_path(old_filename, old_path, sizeof(old_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    rc = osal_lfs_build_vfs_path(new_filename, new_path, sizeof(new_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    return (rename(old_path, new_path) == 0) ? OSAL_SUCCESS : OSAL_ERROR;
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

    int slot = find_slot_by_fd(filedes);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    strncpy(fd_prop->path, g_open_fds[slot].path, sizeof(fd_prop->path) - 1);
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

    char vfs_path[OSAL_MAX_PATH_LEN];
    int32_t rc = osal_lfs_build_vfs_path(filename, vfs_path, sizeof(vfs_path));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    for (int i = 0; i < OSAL_MAX_OPEN_FILES; ++i)
    {
        if (g_open_fds[i].in_use && strcmp(g_open_fds[i].path, vfs_path) == 0)
        {
            return OSAL_SUCCESS;
        }
    }

    return OSAL_ERROR;
}
