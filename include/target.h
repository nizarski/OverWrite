#ifndef OVERWRITE_TARGET_H
#define OVERWRITE_TARGET_H

#include "common.h"
#include "platform.h"
#include "hidden_area.h"
#include "slack.h"

typedef struct {
    platform_device_t *device;
    platform_dev_kind_t dev_kind;
    device_geometry_t geo;
    range_list_t ranges;
    char resolved_path[4096];
    char filler_path[4096];
    target_kind_t kind;
    bool owns_device;
    uint64_t total_bytes;
    hidden_area_info_t hidden;
    slack_info_t slack;
} wipe_target_t;

int  target_resolve(const overwrite_config_t *cfg, wipe_target_t *out);
void target_cleanup(wipe_target_t *target);
const char *target_kind_name(target_kind_t kind);
const char *profile_name(wipe_profile_t p);
const char *rng_mode_name(rng_mode_t m);

int  target_parse_profile(const char *s, wipe_profile_t *out);
int  target_parse_rng(const char *s, rng_mode_t *out);
int  target_parse_kind_flag(const char *flag, const char *path,
                            overwrite_config_t *cfg);

#endif
