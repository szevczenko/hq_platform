#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "osal_dir.h"
#include "osal_littlefs_backend.h"
#include "lfs.h"

#define OSAL_LFS_MAX_OPEN_DIRS 16

typedef struct
{
    bool in_use;
    lfs_dir_t dir;
    char path[OSAL_MAX_PATH_LEN];
} osal_lfs_open_dir_t;

static osal_lfs_open_dir_t g_open_dirs[OSAL_LFS_MAX_OPEN_DIRS];

static int lfs_dir_to_slot(osal_dir_id_t dir_id)
{
    int slot = (int)dir_id - 1;
    if (slot < 0 || slot >= OSAL_LFS_MAX_OPEN_DIRS)
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
    for (int i = 0; i < OSAL_LFS_MAX_OPEN_DIRS; ++i)
    {
        if (!g_open_dirs[i].in_use)
        {
            return i;
        }
    }
    return -1;
}

static osal_dirent_type_t lfs_type_to_osal(uint8_t type)
{
    switch (type)
    {
        case LFS_TYPE_REG:
            return OSAL_DIRENT_TYPE_FILE;
        case LFS_TYPE_DIR:
            return OSAL_DIRENT_TYPE_DIR;
        default:
            return OSAL_DIRENT_TYPE_UNKNOWN;
    }
}

osal_dir_id_t osal_dir_open(const char *path)
{
    if (path == NULL)
    {
        return (osal_dir_id_t)OSAL_INVALID_POINTER;
    }
    if (!g_osal_lfs_mounted)
    {
        return (osal_dir_id_t)OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    char norm_path[OSAL_MAX_PATH_LEN];
    int32_t rc = osal_lfs_path_normalize(path, norm_path, sizeof(norm_path));
    if (rc != OSAL_SUCCESS)
    {
        return (osal_dir_id_t)rc;
    }

    int slot = alloc_slot();
    if (slot < 0)
    {
        return (osal_dir_id_t)OSAL_ERR_NO_FREE_IDS;
    }

    int err = lfs_dir_open(&g_osal_lfs, &g_open_dirs[slot].dir, norm_path);
    if (err != 0)
    {
        return (osal_dir_id_t)osal_lfs_map_error(err);
    }

    g_open_dirs[slot].in_use = true;
    strncpy(g_open_dirs[slot].path, norm_path, sizeof(g_open_dirs[slot].path) - 1);
    g_open_dirs[slot].path[sizeof(g_open_dirs[slot].path) - 1] = '\0';

    return (osal_dir_id_t)(slot + 1);
}

int32_t osal_dir_read(osal_dir_id_t dir_id, osal_dirent_t *entry)
{
    if (entry == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    int slot = lfs_dir_to_slot(dir_id);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    struct lfs_info info;
    while (true)
    {
        int n = lfs_dir_read(&g_osal_lfs, &g_open_dirs[slot].dir, &info);
        if (n < 0)
        {
            return osal_lfs_map_error(n);
        }
        if (n == 0)
        {
            return OSAL_ERR_EMPTY_SET;
        }
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0)
        {
            continue;
        }
        break;
    }

    memset(entry, 0, sizeof(*entry));
    strncpy(entry->name, info.name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->type = lfs_type_to_osal(info.type);
    entry->size = info.size;
    if (entry->type == OSAL_DIRENT_TYPE_DIR)
    {
        entry->mode_bits = OSAL_FILESTAT_MODE_DIR;
    }
    else
    {
        entry->mode_bits = OSAL_FILESTAT_MODE_READ | OSAL_FILESTAT_MODE_WRITE;
    }

    return OSAL_SUCCESS;
}

int32_t osal_dir_rewind(osal_dir_id_t dir_id)
{
    int slot = lfs_dir_to_slot(dir_id);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    int err = lfs_dir_rewind(&g_osal_lfs, &g_open_dirs[slot].dir);
    return osal_lfs_map_error(err);
}

int32_t osal_dir_close(osal_dir_id_t dir_id)
{
    int slot = lfs_dir_to_slot(dir_id);
    if (slot < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    int err = lfs_dir_close(&g_osal_lfs, &g_open_dirs[slot].dir);
    g_open_dirs[slot].in_use = false;
    g_open_dirs[slot].path[0] = '\0';

    return osal_lfs_map_error(err);
}

int32_t osal_mkdir(const char *path)
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

    int err = lfs_mkdir(&g_osal_lfs, norm_path);
    return osal_lfs_map_error(err);
}

int32_t osal_rmdir(const char *path)
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