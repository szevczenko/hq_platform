#include "hq_storage.h"

#include <string.h>

#include "osal_dir.h"
#include "osal_error.h"
#include "osal_file.h"
#include "osal_mount.h"

static bool s_mounted;
static char s_mount_point[OSAL_MAX_PATH_LEN];

static void hq_storage_notify_failure(const hq_storage_config_t *config,
				      hq_storage_status_t status)
{
	if (config->on_failure != NULL) {
		config->on_failure(status, config->user_data);
	}
}

bool hq_storage_is_mounted(void)
{
	return s_mounted;
}

hq_storage_status_t hq_storage_ensure_dir(const char *logical_path)
{
	if (logical_path == NULL || logical_path[0] == '\0') {
		return HQ_STORAGE_ERR_INVALID_ARGUMENT;
	}
	if (!s_mounted) {
		return HQ_STORAGE_ERR_NOT_MOUNTED;
	}

	int32_t status = osal_mkdir(logical_path);
	return status == OSAL_SUCCESS || status == OSAL_ERR_NAME_TAKEN
		       ? HQ_STORAGE_OK
		       : HQ_STORAGE_ERR_DIRECTORY;
}

hq_storage_status_t hq_storage_init(const hq_storage_config_t *config)
{
	if (config == NULL || config->partition_label == NULL ||
	    config->partition_label[0] == '\0' || config->mount_point == NULL ||
	    config->mount_point[0] == '\0' ||
	    strlen(config->mount_point) >= sizeof(s_mount_point) ||
	    (config->directory_count != 0 && config->directories == NULL)) {
		return HQ_STORAGE_ERR_INVALID_ARGUMENT;
	}
	for (size_t i = 0; i < config->directory_count; ++i) {
		if (config->directories[i] == NULL ||
		    config->directories[i][0] == '\0') {
			return HQ_STORAGE_ERR_INVALID_ARGUMENT;
		}
	}
	if (s_mounted) {
		return HQ_STORAGE_ERR_MOUNT;
	}

	memcpy(s_mount_point, config->mount_point,
	       strlen(config->mount_point) + 1u);
	if (osal_mount(config->partition_label, s_mount_point) != OSAL_SUCCESS) {
		s_mount_point[0] = '\0';
		hq_storage_notify_failure(config, HQ_STORAGE_ERR_MOUNT);
		return HQ_STORAGE_ERR_MOUNT;
	}
	s_mounted = true;

	for (size_t i = 0; i < config->directory_count; ++i) {
		hq_storage_status_t status =
			hq_storage_ensure_dir(config->directories[i]);
		if (status != HQ_STORAGE_OK) {
			hq_storage_notify_failure(config, status);
			if (osal_unmount(s_mount_point) != OSAL_SUCCESS) {
				return HQ_STORAGE_ERR_UNMOUNT;
			}
			s_mounted = false;
			s_mount_point[0] = '\0';
			return status;
		}
	}

	return HQ_STORAGE_OK;
}

hq_storage_status_t hq_storage_deinit(void)
{
	if (!s_mounted) {
		return HQ_STORAGE_OK;
	}
	if (osal_unmount(s_mount_point) != OSAL_SUCCESS) {
		return HQ_STORAGE_ERR_UNMOUNT;
	}
	s_mounted = false;
	s_mount_point[0] = '\0';
	return HQ_STORAGE_OK;
}