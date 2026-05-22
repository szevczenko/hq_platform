#ifndef OSAL_DIR_H
#define OSAL_DIR_H

#include <stdint.h>

#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_file.h"

typedef int32_t osal_dir_id_t;

typedef enum
{
    OSAL_DIRENT_TYPE_UNKNOWN = 0,
    OSAL_DIRENT_TYPE_FILE    = 1,
    OSAL_DIRENT_TYPE_DIR     = 2
} osal_dirent_type_t;

typedef struct
{
    char name[OSAL_MAX_PATH_LEN];
    osal_dirent_type_t type;
    size_t size;
    uint32_t mode_bits;
} osal_dirent_t;

osal_dir_id_t osal_dir_open(const char *path);
int32_t osal_dir_read(osal_dir_id_t dir_id, osal_dirent_t *entry);
int32_t osal_dir_rewind(osal_dir_id_t dir_id);
int32_t osal_dir_close(osal_dir_id_t dir_id);
int32_t osal_mkdir(const char *path);
int32_t osal_rmdir(const char *path);

#endif /* OSAL_DIR_H */