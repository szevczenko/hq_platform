#ifndef OSAL_LITTLEFS_BACKEND_H
#define OSAL_LITTLEFS_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lfs.h"
#include "bd/lfs_filebd.h"

extern lfs_t g_osal_lfs;
extern struct lfs_config g_osal_lfs_cfg;
extern lfs_filebd_t g_osal_lfs_bd;
extern struct lfs_filebd_config g_osal_lfs_bd_cfg;
extern bool g_osal_lfs_configured;
extern bool g_osal_lfs_mounted;
extern char g_osal_lfs_image_path[];
extern char g_osal_lfs_mount_point[];
extern char g_osal_lfs_devname[];

int32_t osal_lfs_path_normalize(const char *in_path, char *out_path, size_t out_size);
int32_t osal_lfs_map_error(int err);

#endif /* OSAL_LITTLEFS_BACKEND_H */
