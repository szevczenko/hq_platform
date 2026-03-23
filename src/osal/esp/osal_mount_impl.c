#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osal_mount.h"
#include "osal_file.h"
#include "osal_littlefs_backend.h"

#include "esp_littlefs.h"

bool g_osal_lfs_mounted = false;
char g_osal_lfs_mount_point[OSAL_MAX_PATH_LEN] = "/littlefs";
char g_osal_lfs_partition_label[OSAL_MAX_PATH_LEN] = "storage";

static int32_t validate_text(const char *value)
{
    if (value == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (value[0] == '\0')
    {
        return OSAL_FS_ERR_PATH_INVALID;
    }
    if (strlen(value) >= OSAL_MAX_PATH_LEN)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }

    return OSAL_SUCCESS;
}

int32_t osal_lfs_build_vfs_path(const char *in_path, char *out_path, size_t out_size)
{
    if (in_path == NULL || out_path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    if (strlen(g_osal_lfs_mount_point) == 0U)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    if (strncmp(in_path, g_osal_lfs_mount_point, strlen(g_osal_lfs_mount_point)) == 0)
    {
        if (strlen(in_path) >= out_size)
        {
            return OSAL_FS_ERR_PATH_TOO_LONG;
        }
        strcpy(out_path, in_path);
        return OSAL_SUCCESS;
    }

    int n = snprintf(out_path, out_size, "%s/%s",
                     g_osal_lfs_mount_point,
                     (in_path[0] == '/') ? (in_path + 1) : in_path);
    if (n < 0 || (size_t)n >= out_size)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }

    return OSAL_SUCCESS;
}

int32_t osal_mkfs(char *address, const char *devname, const char *volname, size_t block_size, size_t num_blocks)
{
    (void)address;
    (void)block_size;
    (void)num_blocks;

    int32_t rc = validate_text(devname);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    if (volname != NULL && volname[0] != '\0')
    {
        rc = validate_text(volname);
        if (rc != OSAL_SUCCESS)
        {
            return rc;
        }
        strncpy(g_osal_lfs_mount_point, volname, sizeof(g_osal_lfs_mount_point) - 1);
        g_osal_lfs_mount_point[sizeof(g_osal_lfs_mount_point) - 1] = '\0';
    }

    strncpy(g_osal_lfs_partition_label, devname, sizeof(g_osal_lfs_partition_label) - 1);
    g_osal_lfs_partition_label[sizeof(g_osal_lfs_partition_label) - 1] = '\0';

    return (esp_littlefs_format(g_osal_lfs_partition_label) == 0) ? OSAL_SUCCESS : OSAL_ERROR;
}

int32_t osal_initfs(char *address, const char *devname, const char *volname, size_t block_size, size_t num_blocks)
{
    (void)address;
    (void)block_size;
    (void)num_blocks;

    int32_t rc = validate_text(devname);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    strncpy(g_osal_lfs_partition_label, devname, sizeof(g_osal_lfs_partition_label) - 1);
    g_osal_lfs_partition_label[sizeof(g_osal_lfs_partition_label) - 1] = '\0';

    if (volname != NULL && volname[0] != '\0')
    {
        rc = validate_text(volname);
        if (rc != OSAL_SUCCESS)
        {
            return rc;
        }
        strncpy(g_osal_lfs_mount_point, volname, sizeof(g_osal_lfs_mount_point) - 1);
        g_osal_lfs_mount_point[sizeof(g_osal_lfs_mount_point) - 1] = '\0';
    }

    return OSAL_SUCCESS;
}

int32_t osal_mount(const char *devname, const char *mount_point)
{
    int32_t rc = validate_text(devname);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    rc = validate_text(mount_point);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    strncpy(g_osal_lfs_partition_label, devname, sizeof(g_osal_lfs_partition_label) - 1);
    g_osal_lfs_partition_label[sizeof(g_osal_lfs_partition_label) - 1] = '\0';

    strncpy(g_osal_lfs_mount_point, mount_point, sizeof(g_osal_lfs_mount_point) - 1);
    g_osal_lfs_mount_point[sizeof(g_osal_lfs_mount_point) - 1] = '\0';

    esp_vfs_littlefs_conf_t conf = {
        .base_path = g_osal_lfs_mount_point,
        .partition_label = g_osal_lfs_partition_label,
        .partition = NULL,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    if (esp_vfs_littlefs_register(&conf) != 0)
    {
        return OSAL_ERROR;
    }

    g_osal_lfs_mounted = true;
    return OSAL_SUCCESS;
}

int32_t osal_rmfs(const char *devname)
{
    int32_t rc = validate_text(devname);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    return (esp_littlefs_format(devname) == 0) ? OSAL_SUCCESS : OSAL_ERROR;
}

int32_t osal_unmount(const char *mount_point)
{
    int32_t rc = validate_text(mount_point);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    if (!g_osal_lfs_mounted)
    {
        return OSAL_SUCCESS;
    }

    if (esp_vfs_littlefs_unregister(g_osal_lfs_partition_label) != 0)
    {
        return OSAL_ERROR;
    }

    g_osal_lfs_mounted = false;
    return OSAL_SUCCESS;
}

int32_t osal_filesys_stat_volume(const char *name, osal_statvfs_t *stat_buf)
{
    (void)name;

    if (stat_buf == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    size_t total_bytes = 0;
    size_t used_bytes = 0;

    if (esp_littlefs_info(g_osal_lfs_partition_label, &total_bytes, &used_bytes) != 0)
    {
        return OSAL_ERROR;
    }

    stat_buf->block_size = 4096U;
    stat_buf->total_blocks = total_bytes / stat_buf->block_size;
    stat_buf->blocks_free = (total_bytes >= used_bytes) ? ((total_bytes - used_bytes) / stat_buf->block_size) : 0U;
    return OSAL_SUCCESS;
}

int32_t osal_chkfs(const char *name, bool repair)
{
    (void)name;
    (void)repair;
    return OSAL_ERR_NOT_IMPLEMENTED;
}
