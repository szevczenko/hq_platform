#ifndef ESP_LITTLEFS_H_
#define ESP_LITTLEFS_H_

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* VFS mount point (for example: "/lfs"). */
    const char *base_path;

    /* Optional partition label used for lookup in the partition table. */
    const char *partition_label;

    /* Optional direct partition handle; takes precedence over label when set. */
    const esp_partition_t *partition;

    /* Format the filesystem if mount fails due to on-disk corruption/absence. */
    bool format_if_mount_failed;

    /* Mount filesystem as read-only. */
    bool read_only;

    /* Register VFS but skip immediate mount; caller mounts later. */
    bool dont_mount;

    /* Allow filesystem to grow to fill available partition space on mount. */
    bool grow_on_mount;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
esp_err_t esp_vfs_littlefs_unregister_partition(const esp_partition_t *partition);

esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
esp_err_t esp_littlefs_partition_info(const esp_partition_t *partition, size_t *total_bytes, size_t *used_bytes);
esp_err_t esp_littlefs_format(const char *partition_label);
esp_err_t esp_littlefs_format_partition(const esp_partition_t *partition);

#ifdef __cplusplus
}
#endif

#endif /* ESP_LITTLEFS_H_ */
