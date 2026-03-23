#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osal_mount.h"
#include "osal_file.h"
#include "osal_littlefs_backend.h"

#include "lfs.h"
#include "bd/lfs_filebd.h"

#ifndef OSAL_LFS_DEFAULT_IMAGE
#define OSAL_LFS_DEFAULT_IMAGE "/tmp/osal_littlefs.img"
#endif

#ifndef OSAL_LFS_DEFAULT_BLOCK_SIZE
#define OSAL_LFS_DEFAULT_BLOCK_SIZE 4096U
#endif

#ifndef OSAL_LFS_DEFAULT_BLOCK_COUNT
#define OSAL_LFS_DEFAULT_BLOCK_COUNT 256U
#endif

#ifndef OSAL_LFS_DEFAULT_RW_SIZE
#define OSAL_LFS_DEFAULT_RW_SIZE 16U
#endif

lfs_t g_osal_lfs;
struct lfs_config g_osal_lfs_cfg;
lfs_filebd_t g_osal_lfs_bd;
struct lfs_filebd_config g_osal_lfs_bd_cfg;
bool g_osal_lfs_configured = false;
bool g_osal_lfs_mounted = false;
char g_osal_lfs_image_path[OSAL_MAX_PATH_LEN] = OSAL_LFS_DEFAULT_IMAGE;
char g_osal_lfs_mount_point[OSAL_MAX_PATH_LEN] = "/";
char g_osal_lfs_devname[OSAL_MAX_PATH_LEN] = "littlefs";

static uint8_t g_osal_lfs_read_buffer[OSAL_LFS_DEFAULT_BLOCK_SIZE];
static uint8_t g_osal_lfs_prog_buffer[OSAL_LFS_DEFAULT_BLOCK_SIZE];
static uint8_t g_osal_lfs_lookahead_buffer[128];

int32_t osal_lfs_map_error(int err)
{
    switch (err)
    {
        case 0:
            return OSAL_SUCCESS;
        case LFS_ERR_NOENT:
            return OSAL_FS_ERR_PATH_INVALID;
        case LFS_ERR_NAMETOOLONG:
            return OSAL_FS_ERR_NAME_TOO_LONG;
        case LFS_ERR_NOSPC:
        case LFS_ERR_FBIG:
            return OSAL_ERR_OUTPUT_TOO_LARGE;
        case LFS_ERR_INVAL:
            return OSAL_ERR_INVALID_ARGUMENT;
        default:
            return OSAL_ERROR;
    }
}

int32_t osal_lfs_path_normalize(const char *in_path, char *out_path, size_t out_size)
{
    if (in_path == NULL || out_path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (out_size == 0)
    {
        return OSAL_ERR_INVALID_SIZE;
    }
    if (in_path[0] == '\0')
    {
        return OSAL_FS_ERR_PATH_INVALID;
    }

    while (*in_path == '/')
    {
        ++in_path;
    }

    if (*in_path == '\0')
    {
        in_path = ".";
    }

    size_t len = strlen(in_path);
    if (len >= out_size)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }

    memcpy(out_path, in_path, len + 1);
    return OSAL_SUCCESS;
}

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

static int32_t osal_lfs_configure(const char *devname, const char *mount_point, size_t block_size, size_t num_blocks)
{
    int32_t rc = validate_text(devname);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    if (mount_point != NULL)
    {
        rc = validate_text(mount_point);
        if (rc != OSAL_SUCCESS)
        {
            return rc;
        }
        strncpy(g_osal_lfs_mount_point, mount_point, sizeof(g_osal_lfs_mount_point) - 1);
        g_osal_lfs_mount_point[sizeof(g_osal_lfs_mount_point) - 1] = '\0';
    }

    strncpy(g_osal_lfs_image_path, devname, sizeof(g_osal_lfs_image_path) - 1);
    g_osal_lfs_image_path[sizeof(g_osal_lfs_image_path) - 1] = '\0';

    strncpy(g_osal_lfs_devname, devname, sizeof(g_osal_lfs_devname) - 1);
    g_osal_lfs_devname[sizeof(g_osal_lfs_devname) - 1] = '\0';

    memset(&g_osal_lfs_cfg, 0, sizeof(g_osal_lfs_cfg));
    memset(&g_osal_lfs_bd, 0, sizeof(g_osal_lfs_bd));
    memset(&g_osal_lfs_bd_cfg, 0, sizeof(g_osal_lfs_bd_cfg));

    g_osal_lfs_bd_cfg.read_size = OSAL_LFS_DEFAULT_RW_SIZE;
    g_osal_lfs_bd_cfg.prog_size = OSAL_LFS_DEFAULT_RW_SIZE;
    g_osal_lfs_bd_cfg.erase_size = (block_size > 0U) ? block_size : OSAL_LFS_DEFAULT_BLOCK_SIZE;
    g_osal_lfs_bd_cfg.erase_count = (num_blocks > 0U) ? num_blocks : OSAL_LFS_DEFAULT_BLOCK_COUNT;

    g_osal_lfs_cfg.context = &g_osal_lfs_bd;
    g_osal_lfs_cfg.read = lfs_filebd_read;
    g_osal_lfs_cfg.prog = lfs_filebd_prog;
    g_osal_lfs_cfg.erase = lfs_filebd_erase;
    g_osal_lfs_cfg.sync = lfs_filebd_sync;
    g_osal_lfs_cfg.read_size = g_osal_lfs_bd_cfg.read_size;
    g_osal_lfs_cfg.prog_size = g_osal_lfs_bd_cfg.prog_size;
    g_osal_lfs_cfg.block_size = g_osal_lfs_bd_cfg.erase_size;
    g_osal_lfs_cfg.block_count = g_osal_lfs_bd_cfg.erase_count;
    g_osal_lfs_cfg.block_cycles = 500;
    g_osal_lfs_cfg.cache_size = g_osal_lfs_cfg.block_size;
    g_osal_lfs_cfg.lookahead_size = sizeof(g_osal_lfs_lookahead_buffer);
    g_osal_lfs_cfg.read_buffer = g_osal_lfs_read_buffer;
    g_osal_lfs_cfg.prog_buffer = g_osal_lfs_prog_buffer;
    g_osal_lfs_cfg.lookahead_buffer = g_osal_lfs_lookahead_buffer;

    g_osal_lfs_configured = true;
    return OSAL_SUCCESS;
}

static int32_t osal_lfs_open_bd(void)
{
    int err = lfs_filebd_create(&g_osal_lfs_cfg, g_osal_lfs_image_path, &g_osal_lfs_bd_cfg);
    if (err != 0)
    {
        return OSAL_ERROR;
    }

    return OSAL_SUCCESS;
}

static void osal_lfs_close_bd(void)
{
    (void)lfs_filebd_destroy(&g_osal_lfs_cfg);
}

int32_t osal_mkfs(char *address, const char *devname, const char *volname, size_t block_size, size_t num_blocks)
{
    (void)volname;

    const char *image_path = (address != NULL && address[0] != '\0') ? address : devname;

    int32_t rc = osal_lfs_configure(image_path, g_osal_lfs_mount_point, block_size, num_blocks);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    rc = osal_lfs_open_bd();
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    int err = lfs_format(&g_osal_lfs, &g_osal_lfs_cfg);
    osal_lfs_close_bd();

    return osal_lfs_map_error(err);
}

int32_t osal_initfs(char *address, const char *devname, const char *volname, size_t block_size, size_t num_blocks)
{
    (void)volname;

    const char *image_path = (address != NULL && address[0] != '\0') ? address : devname;
    int32_t rc = osal_lfs_configure(image_path, g_osal_lfs_mount_point, block_size, num_blocks);
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    rc = osal_lfs_open_bd();
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    osal_lfs_close_bd();
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

    if (!g_osal_lfs_configured || strncmp(devname, g_osal_lfs_image_path, OSAL_MAX_PATH_LEN) != 0)
    {
        rc = osal_lfs_configure(devname, mount_point, OSAL_LFS_DEFAULT_BLOCK_SIZE, OSAL_LFS_DEFAULT_BLOCK_COUNT);
        if (rc != OSAL_SUCCESS)
        {
            return rc;
        }
    }

    if (g_osal_lfs_mounted)
    {
        return OSAL_SUCCESS;
    }

    rc = osal_lfs_open_bd();
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    int err = lfs_mount(&g_osal_lfs, &g_osal_lfs_cfg);
    if (err != 0)
    {
        osal_lfs_close_bd();
        return osal_lfs_map_error(err);
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

    if (g_osal_lfs_mounted)
    {
        (void)osal_unmount(g_osal_lfs_mount_point);
    }

    if (remove(devname) != 0 && errno != ENOENT)
    {
        return OSAL_ERROR;
    }

    return OSAL_SUCCESS;
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

    int err = lfs_unmount(&g_osal_lfs);
    osal_lfs_close_bd();
    g_osal_lfs_mounted = false;

    return osal_lfs_map_error(err);
}

int32_t osal_filesys_stat_volume(const char *name, osal_statvfs_t *stat_buf)
{
    (void)name;

    if (stat_buf == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (!g_osal_lfs_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    lfs_ssize_t used_blocks = lfs_fs_size(&g_osal_lfs);
    if (used_blocks < 0)
    {
        return osal_lfs_map_error((int)used_blocks);
    }

    stat_buf->block_size = g_osal_lfs_cfg.block_size;
    stat_buf->total_blocks = g_osal_lfs_cfg.block_count;
    stat_buf->blocks_free = (used_blocks <= (lfs_ssize_t)g_osal_lfs_cfg.block_count)
                                ? (g_osal_lfs_cfg.block_count - (size_t)used_blocks)
                                : 0;

    return OSAL_SUCCESS;
}

int32_t osal_chkfs(const char *name, bool repair)
{
    (void)name;
    (void)repair;
    return OSAL_ERR_NOT_IMPLEMENTED;
}
