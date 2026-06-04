#include "common.h"
#include "target.h"
#include "wipe.h"
#include "safety.h"
#include "progress.h"
#include "slack.h"
#include "free_space.h"
#include "devlist.h"
#include "android.h"
#include "format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "OverWrite %s - secure high-speed data destruction\n\n"
        "Usage: %s [options] <target>\n"
        "       %s --free-space <mountpoint>\n"
        "       %s --unallocated <disk>\n\n"
        "Targets:\n"
        "  /dev/sda, \\\\.\\PhysicalDrive0   whole disk\n"
        "  /dev/sda2, \\\\.\\C:               partition/volume\n"
        "  /path/to/file                    regular file\n"
        "  --free-space /mnt                wipe free space via filler file\n"
        "  --unallocated /dev/sda           wipe partition gaps only\n\n"
        "Android (fastboot; install platform-tools):\n"
        "  --android-list                   list adb/fastboot devices\n"
        "  --android-wipe [serial]          factory wipe via fastboot\n"
        "  --android-mode MODE              factory|userdata|full (default: factory)\n"
        "  --android-reboot-bootloader      adb reboot bootloader [serial]\n\n"
        "Options:\n"
        "  --list [FILTER]   list disks/partitions/volumes (no wipe)\n"
        "                    FILTER: all, disks, partitions, volumes\n"
        "  -l [FILTER]       same as --list\n"
        "  --profile NAME    ghost|chameleon|spectrum|flash-realist|\n"
        "                    filesystem-shadow|block-cartographer|slack-hunter\n"
        "                    (default: ghost)\n"
        "  --rng MODE        turbo|vault|hybrid|os-chunk (default: vault)\n"
        "  --nonce HEX       optional nonce for chameleon profile\n"
        "  --dry-run         show plan without writing\n"
        "  -t N              override thread count\n"
        "  -c SIZE            chunk size (e.g. 1M)\n"
        "  --passes N        number of passes (overrides profile default)\n"
        "  --force           skip system disk safety block\n"
        "  --ssd-secure-erase  attempt SSD secure erase (flash-realist)\n"
        "  --allow-trim      allow TRIM/discard (off by default)\n"
        "  --normalize-meta  normalize filler file metadata on delete\n"
        "  --format-after [FS]  create filesystem after wipe (exfat|ntfs|fat32|ext4)\n"
        "                       default FS: exfat; whole disk / partition only\n"
        "  -y, --yes         skip typing path to confirm (dangerous)\n"
        "  -q, --quiet       disable progress bar\n"
        "  -h, --help        show this help\n",
        OVERWRITE_VERSION, prog, prog, prog);
}

static void progress_bridge(const wipe_status_t *st, void *userdata)
{
    progress_bar_t *pb = (progress_bar_t *)userdata;
    progress_update(pb, st);
}

static target_kind_t infer_kind(const char *path, bool free_space, bool unalloc)
{
    if (free_space) {
        return TARGET_FREE_SPACE;
    }
    if (unalloc) {
        return TARGET_UNALLOCATED;
    }
    if (platform_path_is_raw_target(path)) {
        return TARGET_WHOLE_DISK;
    }
#ifdef _WIN32
    {
        const char *p = path;
        if (p != NULL && strncmp(p, "\\\\.\\", 4) == 0) {
            p += 4;
        }
        if (p != NULL && p[0] != '\0' && p[1] == ':' &&
            (p[2] == '\0' || p[2] == '\\') &&
            isalpha((unsigned char)p[0])) {
            return TARGET_PARTITION;
        }
    }
#endif
#ifndef _WIN32
    if (platform_path_is_block_device(path)) {
        return TARGET_PARTITION;
    }
#endif
    return TARGET_FILE;
}

int main(int argc, char **argv)
{
    overwrite_config_t cfg;
    wipe_target_t target;
    safety_result_t safety;
    progress_bar_t *progress;
    const char *prog = argv[0] != NULL ? argv[0] : "overwrite";
    const char *target_path = NULL;
    bool free_space = false;
    bool unallocated = false;
    bool list_devices = false;
    devlist_filter_t list_filter = DEVLIST_ALL;
    bool android_list = false;
    bool do_android_wipe = false;
    bool android_reboot_bl = false;
    char android_serial[128];
    android_wipe_mode_t android_mode = ANDROID_MODE_FACTORY;
    int i;

    android_serial[0] = '\0';

    memset(&cfg, 0, sizeof(cfg));
    cfg.profile = PROFILE_GHOST;
    cfg.rng = RNG_VAULT;
    cfg.chunk_size = OVERWRITE_DEFAULT_CHUNK;
    cfg.passes = 0;
    cfg.format_after = false;
    if (format_parse_fstype("exfat", cfg.format_fs, sizeof(cfg.format_fs)) != 0) {
        strncpy(cfg.format_fs, "exfat", sizeof(cfg.format_fs) - 1);
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(prog);
            return 0;
        } else if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
            list_devices = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (devlist_parse_filter(argv[i + 1], &list_filter) != 0) {
                    fprintf(stderr, "unknown list filter: %s\n", argv[i + 1]);
                    fprintf(stderr, "use: all, disks, partitions, volumes\n");
                    return 1;
                }
                i++;
            }
        } else if (strcmp(argv[i], "--android-list") == 0) {
            android_list = true;
        } else if (strcmp(argv[i], "--android-wipe") == 0) {
            do_android_wipe = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(android_serial, argv[++i], sizeof(android_serial) - 1);
            }
        } else if (strcmp(argv[i], "--android-mode") == 0 && i + 1 < argc) {
            if (android_parse_mode(argv[++i], &android_mode) != 0) {
                fprintf(stderr, "unknown android mode (factory|userdata|full)\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--android-reboot-bootloader") == 0) {
            android_reboot_bl = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(android_serial, argv[++i], sizeof(android_serial) - 1);
            }
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            cfg.dry_run = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            cfg.force = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cfg.quiet = true;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            cfg.yes = true;
        } else if (strcmp(argv[i], "--free-space") == 0 && i + 1 < argc) {
            free_space = true;
            target_path = argv[++i];
        } else if (strcmp(argv[i], "--unallocated") == 0 && i + 1 < argc) {
            unallocated = true;
            target_path = argv[++i];
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            if (target_parse_profile(argv[++i], &cfg.profile) != 0) {
                fprintf(stderr, "unknown profile: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--rng") == 0 && i + 1 < argc) {
            if (target_parse_rng(argv[++i], &cfg.rng) != 0) {
                fprintf(stderr, "unknown rng mode: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--nonce") == 0 && i + 1 < argc) {
            strncpy(cfg.nonce_hex, argv[++i], sizeof(cfg.nonce_hex) - 1);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg.thread_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            uint64_t sz = 0;
            if (ow_parse_size(argv[++i], &sz) != 0 || sz == 0) {
                fprintf(stderr, "invalid chunk size\n");
                return 1;
            }
            cfg.chunk_size = (uint32_t)sz;
        } else if (strcmp(argv[i], "--passes") == 0 && i + 1 < argc) {
            cfg.passes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ssd-secure-erase") == 0) {
            cfg.ssd_secure_erase = true;
        } else if (strcmp(argv[i], "--allow-trim") == 0) {
            cfg.allow_trim = true;
        } else if (strcmp(argv[i], "--normalize-meta") == 0) {
            cfg.normalize_meta = true;
        } else if (strcmp(argv[i], "--format-after") == 0) {
            cfg.format_after = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (format_parse_fstype(argv[i + 1], cfg.format_fs,
                                        sizeof(cfg.format_fs)) != 0) {
                    fprintf(stderr, "unknown format (use exfat|ntfs|fat32|ext4)\n");
                    return 1;
                }
                i++;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage(prog);
            return 1;
        } else if (target_path == NULL) {
            target_path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (list_devices) {
        return devlist_print(stdout, list_filter) == 0 ? 0 : 1;
    }

    if (android_list) {
        return android_list_devices(stdout) == 0 ? 0 : 1;
    }

    if (android_reboot_bl) {
        fprintf(stderr, "rebooting to bootloader via adb...\n");
        return android_reboot_bootloader(
            android_serial[0] != '\0' ? android_serial : NULL) == 0 ? 0 : 1;
    }

    if (do_android_wipe) {
        if (android_print_wipe_plan(stderr, android_serial[0] != '\0' ? android_serial : NULL,
                                    android_mode, cfg.dry_run) != 0) {
            return 1;
        }
        if (cfg.dry_run) {
            fprintf(stderr, "dry-run complete; no fastboot commands sent\n");
            return 0;
        }
        if (android_confirm_wipe(android_serial[0] != '\0' ? android_serial : NULL,
                                 cfg.yes) != 0) {
            return 1;
        }
        return android_wipe(android_serial[0] != '\0' ? android_serial : NULL,
                            android_mode, false) == 0 ? 0 : 1;
    }

    if (target_path == NULL) {
        print_usage(prog);
        return 1;
    }

    cfg.kind = infer_kind(target_path, free_space, unallocated);
    strncpy(cfg.path, target_path, sizeof(cfg.path) - 1);

    if (cfg.profile == PROFILE_FILESYSTEM_SHADOW &&
        cfg.kind != TARGET_FILE && cfg.kind != TARGET_FREE_SPACE) {
        fprintf(stderr,
                "note: filesystem-shadow applies to files/free-space; continuing\n");
    }

    if (cfg.profile == PROFILE_SLACK_HUNTER &&
        cfg.kind == TARGET_FILE && !cfg.dry_run) {
        slack_info_t slack;
        if (slack_hunter_analyze(cfg.path, &slack) == 0 &&
            slack.wipe_size > slack.logical_size) {
            fprintf(stderr,
                    "slack-hunter will extend file to %llu bytes before wipe\n",
                    (unsigned long long)slack.wipe_size);
        }
    }

    if (cfg.format_after && !format_can_apply(cfg.path, cfg.kind)) {
        fprintf(stderr,
                "note: --format-after applies to whole disk or partition targets only\n");
        cfg.format_after = false;
    }

    memset(&target, 0, sizeof(target));
    if (target_resolve(&cfg, &target) != 0) {
        return 1;
    }

    if (cfg.format_after && !format_can_apply(target.resolved_path, target.kind)) {
        fprintf(stderr, "note: --format-after not applicable to this target\n");
        cfg.format_after = false;
    }

    if (safety_check(&target, &cfg, &safety) != 0) {
        target_cleanup(&target);
        return 1;
    }

    safety_print_plan(&target, &cfg);

    if (safety_confirm(&target, &cfg, &safety) != 0) {
        target_cleanup(&target);
        return 1;
    }

    if (cfg.dry_run) {
        fprintf(stderr, "dry-run complete; no data was written\n");
        target_cleanup(&target);
        return 0;
    }

    progress = progress_create(!cfg.quiet);
    if (wipe_execute(&target, &cfg, progress_bridge, progress) != 0) {
        fprintf(stderr, "OverWrite failed\n");
        progress_destroy(progress);
        target_cleanup(&target);
        return 1;
    }

    progress_destroy(progress);

    if (cfg.format_after && format_can_apply(target.resolved_path, target.kind)) {
        if (target.device != NULL && target.owns_device) {
            platform_close(target.device);
            target.device = NULL;
        }
        if (format_after_wipe(target.resolved_path, target.kind,
                              cfg.format_fs) != 0) {
            fprintf(stderr, "warning: post-wipe format failed\n");
        }
    }

    if (cfg.profile == PROFILE_FILESYSTEM_SHADOW) {
        if (cfg.kind == TARGET_FREE_SPACE && target.filler_path[0] != '\0') {
            if (fs_shadow_secure_delete(target.filler_path, cfg.normalize_meta,
                                        true) != 0) {
                fprintf(stderr, "warning: secure delete of filler failed\n");
            }
            target.filler_path[0] = '\0';
        } else if (cfg.kind == TARGET_FILE) {
            if (fs_shadow_secure_delete(cfg.path, cfg.normalize_meta, true) != 0) {
                fprintf(stderr, "warning: secure delete of file failed\n");
            }
        }
    } else if (cfg.kind == TARGET_FREE_SPACE && target.filler_path[0] != '\0') {
        free_space_remove_filler(target.filler_path, cfg.normalize_meta);
        target.filler_path[0] = '\0';
    }

    target_cleanup(&target);
    fprintf(stderr, "OverWrite completed successfully\n");
    return 0;
}
