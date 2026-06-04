#include "wipe.h"
#include "rng.h"
#include "platform.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

typedef struct {
    wipe_target_t *target;
    const overwrite_config_t *cfg;
    byte_range_t range;
    int thread_id;
    int pass_index;
    uint32_t sector_size;
    uint32_t chunk_size;
    void *buffer;
    rng_ctx_t *rng;
    volatile int *failed;
    char error[256];
    volatile uint64_t *bytes_done;
} worker_args_t;

typedef struct {
    wipe_target_t *target;
    const overwrite_config_t *cfg;
    wipe_progress_fn progress_cb;
    void *userdata;
    volatile int failed;
    char error[256];
    volatile uint64_t bytes_done;
    uint64_t bytes_total;
    int pass_total;
    int pass_current;
    volatile int monitor_running;
    time_t start_time;
} wipe_shared_t;

#ifdef _WIN32
static DWORD WINAPI progress_monitor(LPVOID arg)
{
    wipe_shared_t *shared = (wipe_shared_t *)arg;
    while (shared->monitor_running && !shared->failed) {
        wipe_status_t st;
        time_t now = time(NULL);
        memset(&st, 0, sizeof(st));
        st.bytes_total = shared->bytes_total;
        st.bytes_done = shared->bytes_done;
        st.pass_current = shared->pass_current;
        st.pass_total = shared->pass_total;
        if (now > shared->start_time) {
            st.bytes_per_sec = (double)shared->bytes_done /
                               (double)(now - shared->start_time);
        }
        if (shared->progress_cb) {
            shared->progress_cb(&st, shared->userdata);
        }
        Sleep(1000 / OVERWRITE_PROGRESS_HZ);
    }
    return 0;
}
#else
static void *progress_monitor(void *arg)
{
    wipe_shared_t *shared = (wipe_shared_t *)arg;
    while (shared->monitor_running && !shared->failed) {
        wipe_status_t st;
        time_t now = time(NULL);
        memset(&st, 0, sizeof(st));
        st.bytes_total = shared->bytes_total;
        st.bytes_done = shared->bytes_done;
        st.pass_current = shared->pass_current;
        st.pass_total = shared->pass_total;
        if (now > shared->start_time) {
            st.bytes_per_sec = (double)shared->bytes_done /
                               (double)(now - shared->start_time);
        }
        if (shared->progress_cb) {
            shared->progress_cb(&st, shared->userdata);
        }
        struct timespec ts = { 0, 1000000000L / OVERWRITE_PROGRESS_HZ };
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

static int profile_pass_count(wipe_profile_t profile)
{
    switch (profile) {
    case PROFILE_SPECTRUM:
        return 2;
    default:
        return 1;
    }
}

static rng_mode_t profile_rng(wipe_profile_t profile, rng_mode_t user_rng)
{
    if (profile == PROFILE_CHAMELEON && user_rng == RNG_VAULT) {
        return RNG_VAULT;
    }
    return user_rng;
}

#ifdef _WIN32

static DWORD WINAPI worker_thread(LPVOID arg)
{
    worker_args_t *wa = (worker_args_t *)arg;
    uint64_t offset = wa->range.offset;
    uint64_t end = wa->range.offset + wa->range.length;

    rng_set_thread_salt(wa->rng, (uint32_t)wa->thread_id);

    while (offset < end && !*wa->failed) {
        uint64_t remaining = end - offset;
        size_t to_write = (size_t)ow_min_u64(remaining, wa->chunk_size);
        to_write = (size_t)ow_align_down((uint64_t)to_write, wa->sector_size);
        if (to_write == 0) {
            break;
        }

        rng_fill(wa->rng, wa->buffer, to_write);
        if (platform_pwrite(wa->target->device, wa->buffer, to_write, offset) != 0) {
            if (InterlockedCompareExchange((LONG *)wa->failed, 1, 0) == 0) {
                snprintf(wa->error, sizeof(wa->error),
                         "write failed at offset %llu",
                         (unsigned long long)offset);
            }
            break;
        }
        offset += to_write;
        if (wa->bytes_done != NULL) {
            InterlockedAdd64((LONG64 *)wa->bytes_done, (LONG64)to_write);
        }
    }
    return 0;
}

static int run_workers(wipe_shared_t *shared, int pass_index)
{
    int thread_count = ow_thread_count(shared->cfg->thread_override);
    HANDLE *threads;
    worker_args_t *args;
    uint64_t total_len = 0;
    size_t i;

    for (i = 0; i < shared->target->ranges.count; i++) {
        total_len += shared->target->ranges.ranges[i].length;
    }
    if (total_len == 0) {
        return -1;
    }

    threads = (HANDLE *)calloc((size_t)thread_count, sizeof(HANDLE));
    args = (worker_args_t *)calloc((size_t)thread_count, sizeof(worker_args_t));
    if (threads == NULL || args == NULL) {
        free(threads);
        free(args);
        return -1;
    }

    {
        byte_range_t full = { 0, total_len };
        uint64_t per = total_len / (uint64_t)thread_count;
        uint64_t cursor = 0;

        for (i = 0; i < (size_t)thread_count; i++) {
            uint64_t len = (i == (size_t)thread_count - 1)
                ? (total_len - cursor) : per;
            len = ow_align_down(len, shared->target->geo.logical_sector_size);
            if (len == 0 && i == (size_t)thread_count - 1) {
                len = ow_align_down(total_len - cursor,
                                    shared->target->geo.logical_sector_size);
            }
            args[i].target = shared->target;
            args[i].cfg = shared->cfg;
            args[i].range.offset = cursor;
            args[i].range.length = len;
            args[i].thread_id = (int)i;
            args[i].pass_index = pass_index;
            args[i].sector_size = shared->target->geo.logical_sector_size;
            args[i].chunk_size = shared->cfg->chunk_size;
            args[i].buffer = platform_alloc_aligned(
                args[i].chunk_size, args[i].sector_size);
            args[i].failed = &shared->failed;
            args[i].bytes_done = &shared->bytes_done;
            args[i].error[0] = '\0';
            if (rng_create(profile_rng(shared->cfg->profile, shared->cfg->rng),
                           shared->cfg->nonce_hex, &args[i].rng) != 0) {
                shared->failed = 1;
                break;
            }
            rng_reseed_pass(args[i].rng, (uint32_t)pass_index);
            threads[i] = CreateThread(NULL, 0, worker_thread, &args[i], 0, NULL);
            if (threads[i] == NULL) {
                shared->failed = 1;
                snprintf(shared->error, sizeof(shared->error),
                         "CreateThread failed");
                break;
            }
            cursor += len;
        }
        (void)full;
    }

    {
        DWORD wait_count = 0;
        size_t j;

        for (j = 0; j < (size_t)thread_count; j++) {
            if (threads[j] != NULL) {
                if (wait_count != (DWORD)j) {
                    threads[wait_count] = threads[j];
                }
                wait_count++;
            }
        }
        if (wait_count > 0) {
            WaitForMultipleObjects(wait_count, threads, TRUE, INFINITE);
        }
    }

    for (i = 0; i < (size_t)thread_count; i++) {
        if (threads[i]) {
            CloseHandle(threads[i]);
        }
        if (args[i].error[0] != '\0' && shared->error[0] == '\0') {
            snprintf(shared->error, sizeof(shared->error), "%s", args[i].error);
        }
        rng_destroy(args[i].rng);
        platform_free_aligned(args[i].buffer);
    }
    free(threads);
    free(args);
    if (shared->failed && shared->error[0] == '\0') {
        snprintf(shared->error, sizeof(shared->error), "worker thread failed");
    }
    return shared->failed ? -1 : 0;
}

#else

static void *worker_thread(void *arg)
{
    worker_args_t *wa = (worker_args_t *)arg;
    uint64_t offset = wa->range.offset;
    uint64_t end = wa->range.offset + wa->range.length;

    rng_set_thread_salt(wa->rng, (uint32_t)wa->thread_id);

    while (offset < end && !*wa->failed) {
        uint64_t remaining = end - offset;
        size_t to_write = (size_t)ow_min_u64(remaining, wa->chunk_size);
        to_write = (size_t)ow_align_down((uint64_t)to_write, wa->sector_size);
        if (to_write == 0) {
            break;
        }

        rng_fill(wa->rng, wa->buffer, to_write);
        if (platform_pwrite(wa->target->device, wa->buffer, to_write, offset) != 0) {
            *wa->failed = 1;
            snprintf(wa->error, sizeof(wa->error),
                     "write failed at offset %llu", (unsigned long long)offset);
            break;
        }
        offset += to_write;
        if (wa->bytes_done != NULL) {
            __sync_add_and_fetch(wa->bytes_done, to_write);
        }
    }
    return NULL;
}

static int run_workers(wipe_shared_t *shared, int pass_index)
{
    int thread_count = ow_thread_count(shared->cfg->thread_override);
    pthread_t *threads;
    worker_args_t *args;
    uint64_t total_len = 0;
    size_t i;

    for (i = 0; i < shared->target->ranges.count; i++) {
        total_len += shared->target->ranges.ranges[i].length;
    }
    if (total_len == 0) {
        return -1;
    }

    threads = (pthread_t *)calloc((size_t)thread_count, sizeof(pthread_t));
    args = (worker_args_t *)calloc((size_t)thread_count, sizeof(worker_args_t));
    if (threads == NULL || args == NULL) {
        free(threads);
        free(args);
        return -1;
    }

    {
        uint64_t per = total_len / (uint64_t)thread_count;
        uint64_t cursor = 0;

        for (i = 0; i < (size_t)thread_count; i++) {
            uint64_t len = (i == (size_t)thread_count - 1)
                ? (total_len - cursor) : per;
            len = ow_align_down(len, shared->target->geo.logical_sector_size);
            args[i].target = shared->target;
            args[i].cfg = shared->cfg;
            args[i].range.offset = cursor;
            args[i].range.length = len;
            args[i].thread_id = (int)i;
            args[i].pass_index = pass_index;
            args[i].sector_size = shared->target->geo.logical_sector_size;
            args[i].chunk_size = shared->cfg->chunk_size;
            args[i].buffer = platform_alloc_aligned(
                args[i].chunk_size, args[i].sector_size);
            args[i].failed = &shared->failed;
            args[i].bytes_done = &shared->bytes_done;
            args[i].error[0] = '\0';
            if (rng_create(profile_rng(shared->cfg->profile, shared->cfg->rng),
                           shared->cfg->nonce_hex, &args[i].rng) != 0) {
                shared->failed = 1;
                break;
            }
            rng_reseed_pass(args[i].rng, (uint32_t)pass_index);
            if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0) {
                shared->failed = 1;
                break;
            }
            cursor += len;
        }
    }

    for (i = 0; i < (size_t)thread_count; i++) {
        pthread_join(threads[i], NULL);
        if (args[i].error[0] != '\0' && shared->error[0] == '\0') {
            snprintf(shared->error, sizeof(shared->error), "%s", args[i].error);
        }
        rng_destroy(args[i].rng);
        platform_free_aligned(args[i].buffer);
    }
    free(threads);
    free(args);
    return shared->failed ? -1 : 0;
}

#endif

static bool wipe_use_sequential(const wipe_target_t *target)
{
    if (target->dev_kind != PLATFORM_DEV_RAW) {
        return false;
    }
    if (target->geo.is_removable) {
        return true;
    }
    /* Small rotational devices are usually USB sticks; avoid parallel I/O. */
    if (target->geo.is_rotational &&
        target->geo.capacity_bytes <= (64ULL * 1024ULL * 1024ULL * 1024ULL)) {
        return true;
    }
    return false;
}

static int wipe_reopen_target(wipe_target_t *target)
{
    platform_dev_kind_t kind = target->dev_kind;

    if (!target->owns_device || target->resolved_path[0] == '\0') {
        return -1;
    }
    if (target->device != NULL) {
        platform_close(target->device);
        target->device = NULL;
    }
    return platform_open(target->resolved_path, kind, true, &target->device);
}

static int wipe_prepare_physical_disk(wipe_target_t *target,
                                      platform_disk_detach_t *detach)
{
    char letters[64];
    size_t vol_count = 0;

    if (detach == NULL) {
        return -1;
    }

    if (target->device != NULL && target->owns_device) {
        platform_close(target->device);
        target->device = NULL;
    }

    if (platform_mounted_volume_letters(target->resolved_path, letters,
                                      sizeof(letters), &vol_count) == 0 &&
        vol_count > 0) {
        fprintf(stderr,
                "warning: volumes on this disk: %s\n", letters);
    }

    if (platform_detach_disk_for_wipe(target->resolved_path, detach) != 0) {
        fprintf(stderr, "error: could not prepare disk for wipe\n");
        fflush(stderr);
        return -1;
    }

    if (wipe_reopen_target(target) != 0) {
        fprintf(stderr, "error: could not reopen %s for wipe\n",
                target->resolved_path);
        platform_print_last_open_error(target->resolved_path);
        return -1;
    }
    if (target->device != NULL && platform_device_is_read_only(target->device)) {
        fprintf(stderr,
                "error: device reopened read-only; run as Administrator\n");
        return -1;
    }
    return 0;
}

static void wipe_restore_physical_disk(wipe_target_t *target,
                                       const platform_disk_detach_t *detach)
{
    if (detach == NULL || target->resolved_path[0] == '\0') {
        return;
    }
    if (!detach->disk_offlined) {
        return;
    }

    if (target->device != NULL && target->owns_device) {
        platform_close(target->device);
        target->device = NULL;
    }

    (void)platform_reattach_disk_after_wipe(target->resolved_path, detach);
}

static int wipe_ranges_sequential(wipe_shared_t *shared, int pass_index)
{
    size_t i;
    uint32_t sector = shared->target->geo.logical_sector_size;
    void *buf = platform_alloc_aligned(shared->cfg->chunk_size, sector);
    rng_ctx_t *rng = NULL;

    if (buf == NULL) {
        return -1;
    }
    if (rng_create(profile_rng(shared->cfg->profile, shared->cfg->rng),
                   shared->cfg->nonce_hex, &rng) != 0) {
        platform_free_aligned(buf);
        return -1;
    }
    rng_reseed_pass(rng, (uint32_t)pass_index);

    for (i = 0; i < shared->target->ranges.count && !shared->failed; i++) {
        uint64_t offset = shared->target->ranges.ranges[i].offset;
        uint64_t end = offset + shared->target->ranges.ranges[i].length;

        while (offset < end && !shared->failed) {
            uint64_t remaining = end - offset;
            size_t to_write = (size_t)ow_min_u64(remaining, shared->cfg->chunk_size);
            to_write = (size_t)ow_align_down((uint64_t)to_write, sector);
            if (to_write == 0) {
                break;
            }
            rng_fill(rng, buf, to_write);
            if (platform_pwrite(shared->target->device, buf, to_write, offset) != 0) {
                shared->failed = 1;
                snprintf(shared->error, sizeof(shared->error),
                         "write failed at offset %llu",
                         (unsigned long long)offset);
                break;
            }
            offset += to_write;
            shared->bytes_done += to_write;
        }
    }

    rng_destroy(rng);
    platform_free_aligned(buf);
    return shared->failed ? -1 : 0;
}

int wipe_execute(wipe_target_t *target, const overwrite_config_t *cfg,
                 wipe_progress_fn progress_cb, void *userdata)
{
    wipe_shared_t shared;
    wipe_status_t st;
    int passes;
    int pass;
    time_t now;
    platform_disk_detach_t disk_detach;
    int rc = 0;

    platform_disk_detach_init(&disk_detach);

    if (target == NULL || cfg == NULL) {
        return -1;
    }

    memset(&shared, 0, sizeof(shared));
    shared.target = target;
    shared.cfg = cfg;
    shared.progress_cb = progress_cb;
    shared.userdata = userdata;
    shared.start_time = time(NULL);
    shared.pass_total = cfg->passes > 0 ? cfg->passes : profile_pass_count(cfg->profile);

    for (size_t i = 0; i < target->ranges.count; i++) {
        shared.bytes_total += target->ranges.ranges[i].length;
    }
    shared.bytes_total = ow_align_down(shared.bytes_total,
                                       target->geo.logical_sector_size);

    if (cfg->dry_run) {
        return 0;
    }

    if (target->device != NULL && platform_device_is_read_only(target->device)) {
        fprintf(stderr,
                "error: device opened read-only; run as Administrator for writes\n");
        return -1;
    }

    if (target->dev_kind == PLATFORM_DEV_RAW) {
        if (wipe_prepare_physical_disk(target, &disk_detach) != 0) {
            wipe_restore_physical_disk(target, &disk_detach);
            return -1;
        }
    }

    if (cfg->profile == PROFILE_FLASH_REALIST && cfg->ssd_secure_erase &&
        target->geo.is_ssd) {
        if (platform_ssd_secure_erase(target->device) == 0) {
            fprintf(stderr, "SSD secure erase completed.\n");
            wipe_restore_physical_disk(target, &disk_detach);
            return 0;
        }
        fprintf(stderr, "warning: secure erase unavailable, falling back to overwrite\n");
    }

    for (pass = 1; pass <= shared.pass_total; pass++) {
        shared.pass_current = pass;
        shared.bytes_done = 0;
        shared.monitor_running = 1;
        shared.start_time = time(NULL);

#ifdef _WIN32
        HANDLE mon = CreateThread(NULL, 0, progress_monitor, &shared, 0, NULL);
#else
        pthread_t mon;
        pthread_create(&mon, NULL, progress_monitor, &shared);
#endif

        if (target->ranges.count == 1 &&
            target->ranges.ranges[0].offset == 0 &&
            !wipe_use_sequential(target)) {
            if (run_workers(&shared, pass) != 0) {
                shared.monitor_running = 0;
                break;
            }
        } else {
            if (wipe_use_sequential(target) && pass == 1) {
                fprintf(stderr,
                        "note: USB/removable disk - using single-threaded wipe\n");
            }
            if (wipe_ranges_sequential(&shared, pass) != 0) {
                shared.monitor_running = 0;
                break;
            }
        }

        shared.monitor_running = 0;
#ifdef _WIN32
        if (mon) {
            WaitForSingleObject(mon, INFINITE);
            CloseHandle(mon);
        }
#else
        pthread_join(mon, NULL);
#endif

        if (platform_flush(target->device) != 0) {
            shared.failed = 1;
            snprintf(shared.error, sizeof(shared.error), "flush failed");
            break;
        }

        if (cfg->allow_trim && target->dev_kind == PLATFORM_DEV_RAW) {
            fprintf(stderr, "issuing TRIM/discard for wiped ranges...\n");
            if (platform_discard_ranges(target->device, &target->ranges) != 0) {
                fprintf(stderr,
                        "warning: TRIM/discard partially failed (continuing)\n");
            }
        }
    }

    passes = pass - 1;
    now = time(NULL);
    memset(&st, 0, sizeof(st));
    st.bytes_total = shared.bytes_total * (uint64_t)shared.pass_total;
    st.bytes_done = shared.bytes_total * (uint64_t)passes;
    st.pass_current = passes;
    st.pass_total = shared.pass_total;
    st.failed = shared.failed != 0;
    if (now > shared.start_time) {
        st.bytes_per_sec = (double)st.bytes_done / (double)(now - shared.start_time);
    }
    if (shared.error[0] != '\0') {
        snprintf(st.error, sizeof(st.error), "%s", shared.error);
    }
    if (progress_cb) {
        progress_cb(&st, userdata);
    }

    if (shared.failed) {
        rc = -1;
        if (shared.error[0] != '\0') {
            fprintf(stderr, "error: %s\n", shared.error);
            platform_print_last_io_error(shared.error);
        } else {
            fprintf(stderr, "error: wipe failed\n");
        }
    }

    wipe_restore_physical_disk(target, &disk_detach);
    return rc;
}
