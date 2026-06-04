#include "safety.h"
#include "hidden_area.h"
#include "format.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <mntent.h>
#else
#include <windows.h>
#endif

static bool path_is_probably_system(const char *path)
{
#ifdef _WIN32
    if (_strnicmp(path, "\\\\.\\C:", 6) == 0 ||
        _strnicmp(path, "\\\\.\\PhysicalDrive0",
                  sizeof("\\\\.\\PhysicalDrive0") - 1) == 0) {
        return true;
    }
#else
    if (strcmp(path, "/") == 0 ||
        strncmp(path, "/dev/sda", 8) == 0 ||
        strncmp(path, "/dev/nvme0n1", 12) == 0) {
        return true;
    }
    {
        FILE *mtab = setmntent("/proc/mounts", "r");
        struct mntent *ent;
        if (mtab != NULL) {
            while ((ent = getmntent(mtab)) != NULL) {
                if (strcmp(ent->mnt_fsname, path) == 0) {
                    if (strcmp(ent->mnt_dir, "/") == 0 ||
                        strcmp(ent->mnt_dir, "/boot") == 0) {
                        endmntent(mtab);
                        return true;
                    }
                }
            }
            endmntent(mtab);
        }
    }
#endif
    (void)path;
    return false;
}

void safety_print_plan(const wipe_target_t *target,
                       const overwrite_config_t *cfg)
{
    int threads = ow_thread_count(cfg->thread_override);
    size_t i;

    fprintf(stderr, "\n=== OverWrite Plan ===\n");
    fprintf(stderr, "Version:      %s\n", OVERWRITE_VERSION);
    fprintf(stderr, "Target:       %s\n", target->resolved_path);
    fprintf(stderr, "Kind:         %s\n", target_kind_name(target->kind));
    fprintf(stderr, "Profile:      %s\n", profile_name(cfg->profile));
    fprintf(stderr, "RNG:          %s\n", rng_mode_name(cfg->rng));
    if (cfg->thread_override > OVERWRITE_MAX_THREADS) {
        fprintf(stderr, "Threads:      %d (capped from %d, max %d)\n",
                threads, cfg->thread_override, OVERWRITE_MAX_THREADS);
    } else if (cfg->thread_override > 0) {
        fprintf(stderr, "Threads:      %d (override)\n", threads);
    } else {
        fprintf(stderr, "Threads:      %d (auto)\n", threads);
    }
    fprintf(stderr, "Chunk size:   %u bytes\n", cfg->chunk_size);
    fprintf(stderr, "Sector size:  %u bytes\n", target->geo.logical_sector_size);
    fprintf(stderr, "Capacity:     %llu bytes\n",
            (unsigned long long)target->geo.capacity_bytes);
    fprintf(stderr, "Total wipe:   %llu bytes\n",
            (unsigned long long)target->total_bytes);
    if (target->geo.model[0] != '\0') {
        fprintf(stderr, "Model:        %s\n", target->geo.model);
    }
    fprintf(stderr, "Media:        %s%s\n",
            target->geo.is_ssd ? "SSD/NVMe" :
            target->geo.is_rotational ? "rotational" : "unknown",
            target->geo.is_removable ? " (removable)" : "");
    fprintf(stderr, "Dry run:      %s\n", cfg->dry_run ? "yes" : "no");
    fprintf(stderr, "Allow TRIM:   %s\n", cfg->allow_trim ? "yes" : "no");
    fprintf(stderr, "SSD erase:    %s\n",
            cfg->ssd_secure_erase ? "yes" : "no");
    if (cfg->format_after && format_can_apply(target->resolved_path, target->kind)) {
        fprintf(stderr, "Format after: %s\n", cfg->format_fs);
    }
    if (cfg->yes) {
        fprintf(stderr, "Confirm:      skipped (--yes)\n");
    }
    if (target->hidden.probed) {
        fprintf(stderr, "Native size:  %llu bytes\n",
                (unsigned long long)target->hidden.native_bytes);
        if (target->hidden.hpa_detected) {
            fprintf(stderr, "HPA hidden:   %llu bytes\n",
                    (unsigned long long)target->hidden.hidden_bytes);
        }
        if (target->hidden.dco_detected) {
            fprintf(stderr, "DCO:          possibly active\n");
        }
    }
    if (target->slack.cluster_size > 0 &&
        cfg->profile == PROFILE_SLACK_HUNTER) {
        fprintf(stderr, "Slack wipe:   %llu bytes (cluster %u)\n",
                (unsigned long long)target->slack.wipe_size,
                target->slack.cluster_size);
    }
    fprintf(stderr, "Ranges (%zu):\n", target->ranges.count);
    for (i = 0; i < target->ranges.count; i++) {
        fprintf(stderr, "  [%zu] offset %llu  length %llu\n", i,
                (unsigned long long)target->ranges.ranges[i].offset,
                (unsigned long long)target->ranges.ranges[i].length);
        if (i >= 15 && target->ranges.count > 16) {
            fprintf(stderr, "  ... (%zu more ranges)\n",
                    target->ranges.count - i - 1);
            break;
        }
    }
    fprintf(stderr, "======================\n\n");
}

int safety_check(const wipe_target_t *target, const overwrite_config_t *cfg,
                 safety_result_t *result)
{
    if (target == NULL || cfg == NULL || result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));
    snprintf(result->prompt, sizeof(result->prompt), "TYPE EXACTLY: %s",
             target->resolved_path);

    if (!cfg->force && path_is_probably_system(cfg->path)) {
        result->blocked = true;
        snprintf(result->reason, sizeof(result->reason),
                 "target appears to be a system or boot device");
    }

    if (target->total_bytes == 0) {
        result->blocked = true;
        snprintf(result->reason, sizeof(result->reason), "nothing to wipe");
    }

    if (cfg->profile == PROFILE_FLASH_REALIST && target->geo.is_ssd &&
        cfg->ssd_secure_erase) {
        fprintf(stderr,
                "warning: SSD secure erase will issue drive-level erase commands\n");
    }

    if (target->dev_kind == PLATFORM_DEV_RAW) {
        char letters[64];
        size_t vol_count = 0;

        if (platform_mounted_volume_letters(target->resolved_path, letters,
                                          sizeof(letters), &vol_count) == 0 &&
            vol_count > 0) {
            fprintf(stderr,
                    "warning: these drive letters are on this physical disk: %s\n",
                    letters);
            fprintf(stderr,
                    "         they will be dismounted before wipe (close open files)\n");
        }
    }

    if (target->dev_kind == PLATFORM_DEV_RAW) {
        hidden_area_print_warnings(&target->hidden);
    }

    return 0;
}

int safety_confirm(const wipe_target_t *target, const overwrite_config_t *cfg,
                   const safety_result_t *check)
{
    char line[4096];
    size_t len;

    if (cfg->dry_run) {
        return 0;
    }

    if (cfg->yes) {
        return 0;
    }

    if (check != NULL && check->blocked) {
        fprintf(stderr, "blocked: %s (use --force to override)\n", check->reason);
        return -1;
    }

    fprintf(stderr, "%s\n> ", check != NULL ? check->prompt : "Type YES to confirm");
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return -1;
    }
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    if (check != NULL && strcmp(line, target->resolved_path) != 0) {
        fprintf(stderr, "confirmation failed\n");
        return -1;
    }
    return 0;
}
