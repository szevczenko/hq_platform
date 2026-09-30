#ifndef HQ_STORAGE_H
#define HQ_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	HQ_STORAGE_OK = 0,
	HQ_STORAGE_ERR_INVALID_ARGUMENT = -1,
	HQ_STORAGE_ERR_MOUNT = -2,
	HQ_STORAGE_ERR_DIRECTORY = -3,
	HQ_STORAGE_ERR_UNMOUNT = -4,
	HQ_STORAGE_ERR_NOT_MOUNTED = -5
} hq_storage_status_t;

typedef void (*hq_storage_failure_cb_t)(hq_storage_status_t status,
						void *user_data);

typedef struct {
	const char *partition_label;
	const char *mount_point;
	const char *const *directories;
	size_t directory_count;
	hq_storage_failure_cb_t on_failure;
	void *user_data;
} hq_storage_config_t;

/* Mounts without formatting, then creates the requested logical directories. */
hq_storage_status_t hq_storage_init(const hq_storage_config_t *config);
hq_storage_status_t hq_storage_ensure_dir(const char *logical_path);
bool hq_storage_is_mounted(void);
hq_storage_status_t hq_storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif