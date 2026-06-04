#ifndef OVERWRITE_WIPE_H
#define OVERWRITE_WIPE_H

#include "target.h"

typedef struct {
    uint64_t bytes_total;
    uint64_t bytes_done;
    double bytes_per_sec;
    int pass_current;
    int pass_total;
    bool failed;
    char error[256];
} wipe_status_t;

typedef void (*wipe_progress_fn)(const wipe_status_t *st, void *userdata);

int wipe_execute(wipe_target_t *target, const overwrite_config_t *cfg,
                 wipe_progress_fn progress_cb, void *userdata);

#endif
