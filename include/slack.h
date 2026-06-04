#ifndef OVERWRITE_SLACK_H
#define OVERWRITE_SLACK_H

#include "common.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FS_TYPE_UNKNOWN,
    FS_TYPE_EXT2,   /* ext2/3/4 family */
    FS_TYPE_NTFS,
    FS_TYPE_OTHER
} fs_type_t;

typedef struct {
    fs_type_t fs_type;
    uint32_t cluster_size;
    uint64_t logical_size;
    uint64_t wipe_size;
    bool extended;
} slack_info_t;

int  slack_hunter_analyze(const char *path, slack_info_t *info);
int  slack_hunter_extend_file(const char *path, const slack_info_t *info);

#endif
