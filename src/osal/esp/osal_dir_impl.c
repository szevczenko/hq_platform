#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osal_dir.h"
#include "osal_littlefs_backend.h"

#define OSAL_MAX_OPEN_DIRS 16

typedef struct
{
    bool in_use;
    DIR *dir;
    char path[OSAL_MAX_PATH_LEN];
} osal_open_dir_t;

static osal_open_dir_t g_open_dirs[OSAL_MAX_OPEN_DIRS];

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

static int dir_id_to_slot(osal_dir_id_t dir_id)
{
    int slot = (int)dir_id - 1;
    if (slot < 0 || slot >= OSAL_MAX_OPEN_DIRS)
    {
        return -1;
    }
    if (!g_open_dirs[slot].in_use)
    {
        return -1;
    }
    return slot;
}

static int alloc_slot(void)
{
    for (int i = 0; i < OSAL_MAX_OPEN_DIRS; ++i)
    {
        if (!g_open_dirs[i].in_use)
        {
            return i;
        }
    }
    return -1;
}

static int32_t build_child_path(const char *base, const char *name, char *out_path, size_t out_size)
{
    if (base == NULL || name == NULL || out_path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    int n = 0;
    if (strcmp(base, "/") == 0)
    {
        n = snprintf(out_path, out_size, "/%s", name);
    }
    else
    {
        n = snprintf(out_path, out_size, "%s/%s", base, name);
    }

    if (n < 0 || (size_t)n >= out_size)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }
    return OSAL_SUCCESS;
}

osal_dir_id_t osal_dir_open(const char *path)
{
    int32_t rc = validate_path(path);
    if (rc != OSAL_SUCCESS)
    {
        return (osal_dir_id_t)rc;
    }

    char vfs_path[OSAL_MAX_PATH_LEN];
    rc = osal_lfs_build_vfs_path(path, vfs_path, sizeof(vfs_path));
    if (rc != OSAL_SUCCESS)
    {
        return (osal_dir_id_t)rc;
    }

    DIR *dir = opendir(vfs_path);
    if (dir == NULL)
    {
        return (errno == ENOENT) ? (osal_dir_id_t)OSAL_FS_ERR_PATH_INVALID : (osal_dir_id_t)OSAL_ERROR;
    }

    int slot = alloc_slot();
    if (slot < 0)
    {
        (void)closedir(dir);
        return (osal_dir_id_t)OSAL_ERR_NO_FREE_IDS;
    }

    g_open_dirs[slot].in_use = true;
    g_open_dirs[slot].dir = dir;
    strncpy(g_open_dirs[slot].path, path, sizeof(g_open_dirs[slot].path) - 1);
    g_open_dirs[slot].path[sizeof(g_open_dirs[slot].path) - 1] = '\0';

    return (osal_dir_id_t)(slot + 1);
}

int32_t osal_dir_read(osal_dir_id_t dir_id, osal_dirent_t *entry)
{
    if (entry == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    int slot = dir_id_to_slot(dir_id);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    struct dirent *de = NULL;
    while (true)
    {
        errno = 0;
        de = readdir(g_open_dirs[slot].dir);
        if (de == NULL)
        {
            return (errno == 0) ? OSAL_ERR_EMPTY_SET : OSAL_ERROR;
        }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        {
            continue;
        }
        break;
    }

    memset(entry, 0, sizeof(*entry));
    strncpy(entry->name, de->d_name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';

    char child_rel[OSAL_MAX_PATH_LEN];
    char child_vfs[OSAL_MAX_PATH_LEN];
    if (build_child_path(g_open_dirs[slot].path, de->d_name, child_rel, sizeof(child_rel)) == OSAL_SUCCESS &&
        osal_lfs_build_vfs_path(child_rel, child_vfs, sizeof(child_vfs)) == OSAL_SUCCESS)
    {
        struct stat st;
        if (stat(child_vfs, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                entry->type = OSAL_DIRENT_TYPE_DIR;
                entry->mode_bits = OSAL_FILESTAT_MODE_DIR;
            }
            else
            {
                entry->type = OSAL_DIRENT_TYPE_FILE;
                entry->mode_bits = OSAL_FILESTAT_MODE_READ | OSAL_FILESTAT_MODE_WRITE;
            }
            entry->size = (size_t)st.st_size;
            return OSAL_SUCCESS;
        }
    }

    entry->type = OSAL_DIRENT_TYPE_UNKNOWN;
    entry->size = 0U;
    entry->mode_bits = 0U;
    return OSAL_SUCCESS;
}

int32_t osal_dir_rewind(osal_dir_id_t dir_id)
{
    int slot = dir_id_to_slot(dir_id);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    rewinddir(g_open_dirs[slot].dir);
    return OSAL_SUCCESS;
}

int32_t osal_dir_close(osal_dir_id_t dir_id)
{
    int slot = dir_id_to_slot(dir_id);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    int rc = closedir(g_open_dirs[slot].dir);
    g_open_dirs[slot].in_use = false;
    g_open_dirs[slot].dir = NULL;
    g_open_dirs[slot].path[0] = '\0';

    return (rc == 0) ? OSAL_SUCCESS : OSAL_ERROR;
}

int32_t osal_mkdir(const char *path)
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

    if (mkdir(vfs_path, 0777) == 0)
    {
        return OSAL_SUCCESS;
    }
    if (errno == EEXIST)
    {
        return OSAL_ERR_NAME_TAKEN;
    }
    return OSAL_ERROR;
}

int32_t osal_rmdir(const char *path)
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

    if (rmdir(vfs_path) == 0)
    {
        return OSAL_SUCCESS;
    }
    if (errno == ENOTEMPTY || errno == EEXIST)
    {
        return OSAL_ERR_OBJECT_IN_USE;
    }
    if (errno == ENOENT)
    {
        return OSAL_FS_ERR_PATH_INVALID;
    }
    return OSAL_ERROR;
}
