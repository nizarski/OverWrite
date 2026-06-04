#include "target.h"
#include "partition.h"
#include "free_space.h"
#include "hidden_area.h"
#include "slack.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int range_list_push(range_list_t *list, uint64_t start, uint64_t length)
{
    byte_range_t *nr;

    if (length == 0) {
        return 0;
    }
    if (list->count == list->capacity) {
        size_t cap = list->capacity == 0 ? 4 : list->capacity * 2;
        nr = (byte_range_t *)realloc(list->ranges, cap * sizeof(byte_range_t));
        if (nr == NULL) {
            return -1;
        }
        list->ranges = nr;
        list->capacity = cap;
    }
    list->ranges[list->count].offset = start;
    list->ranges[list->count].length = length;
    list->count++;
    return 0;
}

static void range_list_clear(range_list_t *list)
{
    free(list->ranges);
    list->ranges = NULL;
    list->count = 0;
    list->capacity = 0;
}

const char *target_kind_name(target_kind_t kind)
{
    switch (kind) {
    case TARGET_WHOLE_DISK: return "whole disk";
    case TARGET_PARTITION: return "partition";
    case TARGET_FILE: return "file";
    case TARGET_FREE_SPACE: return "free space";
    case TARGET_UNALLOCATED: return "unallocated gaps";
    case TARGET_RANGE_LIST: return "custom ranges";
    default: return "unknown";
    }
}

const char *profile_name(wipe_profile_t p)
{
    switch (p) {
    case PROFILE_GHOST: return "ghost";
    case PROFILE_CHAMELEON: return "chameleon";
    case PROFILE_SPECTRUM: return "spectrum";
    case PROFILE_FLASH_REALIST: return "flash-realist";
    case PROFILE_FILESYSTEM_SHADOW: return "filesystem-shadow";
    case PROFILE_BLOCK_CARTOGRAPHER: return "block-cartographer";
    case PROFILE_SLACK_HUNTER: return "slack-hunter";
    default: return "unknown";
    }
}

const char *rng_mode_name(rng_mode_t m)
{
    switch (m) {
    case RNG_TURBO: return "turbo";
    case RNG_VAULT: return "vault";
    case RNG_HYBRID: return "hybrid";
    case RNG_OS_CHUNK: return "os-chunk";
    default: return "unknown";
    }
}

int target_parse_profile(const char *s, wipe_profile_t *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    if (strcmp(s, "ghost") == 0) { *out = PROFILE_GHOST; return 0; }
    if (strcmp(s, "chameleon") == 0) { *out = PROFILE_CHAMELEON; return 0; }
    if (strcmp(s, "spectrum") == 0) { *out = PROFILE_SPECTRUM; return 0; }
    if (strcmp(s, "flash-realist") == 0) { *out = PROFILE_FLASH_REALIST; return 0; }
    if (strcmp(s, "filesystem-shadow") == 0) { *out = PROFILE_FILESYSTEM_SHADOW; return 0; }
    if (strcmp(s, "block-cartographer") == 0) { *out = PROFILE_BLOCK_CARTOGRAPHER; return 0; }
    if (strcmp(s, "slack-hunter") == 0) { *out = PROFILE_SLACK_HUNTER; return 0; }
    return -1;
}

int target_parse_rng(const char *s, rng_mode_t *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    if (strcmp(s, "turbo") == 0) { *out = RNG_TURBO; return 0; }
    if (strcmp(s, "vault") == 0) { *out = RNG_VAULT; return 0; }
    if (strcmp(s, "hybrid") == 0) { *out = RNG_HYBRID; return 0; }
    if (strcmp(s, "os-chunk") == 0) { *out = RNG_OS_CHUNK; return 0; }
    return -1;
}

int target_parse_kind_flag(const char *flag, const char *path,
                           overwrite_config_t *cfg)
{
    if (flag == NULL || path == NULL || cfg == NULL) {
        return -1;
    }
    if (strcmp(flag, "--free-space") == 0) {
        cfg->kind = TARGET_FREE_SPACE;
    } else if (strcmp(flag, "--unallocated") == 0) {
        cfg->kind = TARGET_UNALLOCATED;
    } else if (strcmp(flag, "--whole-disk") == 0) {
        cfg->kind = TARGET_WHOLE_DISK;
    } else {
        return -1;
    }
    strncpy(cfg->path, path, sizeof(cfg->path) - 1);
    return 0;
}

static platform_dev_kind_t detect_dev_kind(const overwrite_config_t *cfg)
{
    if (cfg->kind == TARGET_FILE || cfg->kind == TARGET_FREE_SPACE) {
        return PLATFORM_DEV_FILE;
    }
    if (platform_path_is_raw_target(cfg->path)) {
        return PLATFORM_DEV_RAW;
    }
#ifndef _WIN32
    if (platform_path_is_block_device(cfg->path)) {
        return PLATFORM_DEV_RAW;
    }
#endif
    return PLATFORM_DEV_FILE;
}

static int apply_slack_hunter(wipe_target_t *target, const char *path)
{
    if (slack_hunter_analyze(path, &target->slack) != 0) {
        uint32_t sector = target->geo.logical_sector_size;
        for (size_t i = 0; i < target->ranges.count; i++) {
            uint64_t end = target->ranges.ranges[i].offset +
                           target->ranges.ranges[i].length;
            end = ow_align_up(end, sector);
            if (end > target->geo.capacity_bytes) {
                end = ow_align_down(target->geo.capacity_bytes, sector);
            }
            if (end > target->ranges.ranges[i].offset) {
                target->ranges.ranges[i].length =
                    end - target->ranges.ranges[i].offset;
            }
        }
        return 0;
    }

    if (slack_hunter_extend_file(path, &target->slack) != 0) {
        fprintf(stderr,
                "warning: could not extend file to cluster boundary\n");
    } else if (target->slack.wipe_size > target->slack.logical_size) {
        target->slack.extended = true;
        fprintf(stderr,
                "slack-hunter: extended %llu -> %llu bytes (cluster %u, fs %s)\n",
                (unsigned long long)target->slack.logical_size,
                (unsigned long long)target->slack.wipe_size,
                target->slack.cluster_size,
                target->slack.fs_type == FS_TYPE_NTFS ? "NTFS" :
                target->slack.fs_type == FS_TYPE_EXT2 ? "ext4" : "other");
    }

    if (target->ranges.count > 0) {
        uint64_t len = ow_align_down(target->slack.wipe_size,
                                     target->geo.logical_sector_size);
        if (len == 0 && target->slack.wipe_size > 0) {
            len = target->geo.logical_sector_size;
        }
        target->ranges.ranges[0].length = len;
        target->total_bytes = 0;
    }
    return 0;
}

int target_resolve(const overwrite_config_t *cfg, wipe_target_t *out)
{
    platform_dev_kind_t kind;
    partition_map_t pmap;
    range_list_t unalloc;
    struct stat st;

    if (cfg == NULL || out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->kind = cfg->kind;
    out->owns_device = true;
    snprintf(out->resolved_path, sizeof(out->resolved_path), "%s", cfg->path);

    if (cfg->kind == TARGET_FREE_SPACE) {
        uint64_t size = 0;
        if (free_space_create_filler(cfg->path, out->filler_path,
                                     sizeof(out->filler_path), &size) != 0) {
            fprintf(stderr, "error: could not create free-space filler on %s\n",
                    cfg->path);
            return -1;
        }
        snprintf(out->resolved_path, sizeof(out->resolved_path), "%s",
                 out->filler_path);
    }

    kind = detect_dev_kind(cfg);
    if (platform_open(out->resolved_path, kind, !cfg->dry_run, &out->device) != 0) {
        if (out->filler_path[0] != '\0') {
            free_space_remove_filler(out->filler_path, cfg->normalize_meta);
        }
        platform_print_last_open_error(out->resolved_path);
        return -1;
    }
    out->dev_kind = kind;

    if (platform_get_geometry(out->device, &out->geo) != 0) {
        target_cleanup(out);
        return -1;
    }

    if (out->geo.logical_sector_size == 0) {
        out->geo.logical_sector_size = 512;
    }

    if (out->dev_kind == PLATFORM_DEV_RAW) {
        (void)hidden_area_probe(out->device, &out->hidden);
    }

    if (cfg->kind == TARGET_UNALLOCATED ||
        cfg->profile == PROFILE_BLOCK_CARTOGRAPHER) {
        memset(&pmap, 0, sizeof(pmap));
        memset(&unalloc, 0, sizeof(unalloc));
        if (partition_map_read(out->device, &pmap) != 0) {
            target_cleanup(out);
            return -1;
        }
        if (partition_map_unallocated(&pmap, out->geo.capacity_bytes,
                                      &unalloc) != 0) {
            partition_map_free(&pmap);
            target_cleanup(out);
            return -1;
        }
        out->ranges = unalloc;
        partition_map_free(&pmap);
    } else if (cfg->kind == TARGET_FREE_SPACE) {
        uint64_t len = ow_align_down(out->geo.capacity_bytes,
                                     out->geo.logical_sector_size);
        if (range_list_push(&out->ranges, 0, len) != 0) {
            target_cleanup(out);
            return -1;
        }
    } else {
        uint64_t len = ow_align_down(out->geo.capacity_bytes,
                                     out->geo.logical_sector_size);
        if (stat(out->resolved_path, &st) == 0 && !platform_path_is_raw_target(out->resolved_path)) {
            len = ow_align_down((uint64_t)st.st_size, out->geo.logical_sector_size);
            if (len == 0 && st.st_size > 0) {
                len = out->geo.logical_sector_size;
            }
        }
        if (range_list_push(&out->ranges, 0, len) != 0) {
            target_cleanup(out);
            return -1;
        }
    }

    if (cfg->profile == PROFILE_SLACK_HUNTER) {
        apply_slack_hunter(out, out->resolved_path);
    }

    for (size_t i = 0; i < out->ranges.count; i++) {
        out->total_bytes += out->ranges.ranges[i].length;
    }
    return 0;
}

void target_cleanup(wipe_target_t *target)
{
    if (target == NULL) {
        return;
    }
    if (target->device != NULL && target->owns_device) {
        platform_close(target->device);
    }
    range_list_clear(&target->ranges);
    if (target->filler_path[0] != '\0') {
        free_space_remove_filler(target->filler_path, false);
        target->filler_path[0] = '\0';
    }
    memset(target, 0, sizeof(*target));
}
