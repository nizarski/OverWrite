#include "progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct progress_bar {
    bool enabled;
    time_t last_update;
};

static void format_bytes(uint64_t bytes, char *out, size_t out_len)
{
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;

    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, out_len, "%.1f %s", v, units[u]);
}

static void format_eta(double seconds, char *out, size_t out_len)
{
    if (seconds < 0 || seconds > 86400.0 * 7) {
        snprintf(out, out_len, "--");
        return;
    }
    int s = (int)seconds;
    int m = s / 60;
    int h = m / 60;
    s %= 60;
    m %= 60;
    if (h > 0) {
        snprintf(out, out_len, "%dh%02dm", h, m);
    } else {
        snprintf(out, out_len, "%dm%02ds", m, s);
    }
}

progress_bar_t *progress_create(bool enabled)
{
    progress_bar_t *pb = (progress_bar_t *)calloc(1, sizeof(*pb));
    if (pb != NULL) {
        pb->enabled = enabled;
    }
    return pb;
}

void progress_update(progress_bar_t *pb, const wipe_status_t *st)
{
    time_t now;
    char done_str[32];
    char total_str[32];
    char rate_str[32];
    char eta_str[32];
    double pct = 0.0;
    int bar_width = 24;
    int filled = 0;
    double eta = -1.0;

    if (pb == NULL || st == NULL || !pb->enabled) {
        return;
    }

    now = time(NULL);
    if (pb->last_update != 0 && now == pb->last_update) {
        return;
    }
    pb->last_update = now;

    if (st->bytes_total > 0) {
        pct = (100.0 * (double)st->bytes_done) / (double)st->bytes_total;
    }
    if (st->bytes_per_sec > 0 && st->bytes_done < st->bytes_total) {
        eta = (double)(st->bytes_total - st->bytes_done) / st->bytes_per_sec;
    }

    filled = (int)((pct / 100.0) * bar_width);
    if (filled > bar_width) {
        filled = bar_width;
    }

    format_bytes(st->bytes_done, done_str, sizeof(done_str));
    format_bytes(st->bytes_total, total_str, sizeof(total_str));
    format_bytes((uint64_t)st->bytes_per_sec, rate_str, sizeof(rate_str));
    format_eta(eta, eta_str, sizeof(eta_str));

    fprintf(stderr, "\rOverWrite  [");
    for (int i = 0; i < bar_width; i++) {
        fputc(i < filled ? '#' : '-', stderr);
    }
    fprintf(stderr, "] %5.1f%%  %s / %s  %s/s  ETA %s  pass %d/%d   ",
            pct, done_str, total_str, rate_str, eta_str,
            st->pass_current, st->pass_total);
    fflush(stderr);
}

void progress_destroy(progress_bar_t *pb)
{
    if (pb != NULL) {
        if (pb->enabled) {
            fprintf(stderr, "\n");
        }
        free(pb);
    }
}
