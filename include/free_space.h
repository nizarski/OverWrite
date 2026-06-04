#ifndef OVERWRITE_FREE_SPACE_H
#define OVERWRITE_FREE_SPACE_H

#include "common.h"

int free_space_bytes(const char *mount_path, uint64_t *available);
int free_space_create_filler(const char *mount_path, char *filler_out,
                             size_t filler_out_len, uint64_t *size_out);
int free_space_remove_filler(const char *filler_path, bool normalize_meta);
int fs_shadow_random_rename(const char *path, char *newpath, size_t newpath_len);
int fs_shadow_normalize_timestamps(const char *path);
int fs_shadow_secure_delete(const char *path, bool normalize_meta,
                            bool random_rename);

#endif
