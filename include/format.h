#ifndef OVERWRITE_FORMAT_H
#define OVERWRITE_FORMAT_H

#include "common.h"

int  format_parse_fstype(const char *s, char *out, size_t out_len);
bool format_supported_target(target_kind_t kind);
bool format_can_apply(const char *path, target_kind_t kind);
int  format_after_wipe(const char *path, target_kind_t kind, const char *fstype);

#endif
