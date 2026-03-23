#ifndef OSAL_LITTLEFS_BACKEND_H
#define OSAL_LITTLEFS_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern bool g_osal_lfs_mounted;
extern char g_osal_lfs_mount_point[];
extern char g_osal_lfs_partition_label[];

int32_t osal_lfs_build_vfs_path(const char *in_path, char *out_path, size_t out_size);

#endif /* OSAL_LITTLEFS_BACKEND_H */
