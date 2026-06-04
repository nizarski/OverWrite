#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

uint64_t ow_min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

uint64_t ow_align_down(uint64_t value, uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    return value - (value % alignment);
}

uint64_t ow_align_up(uint64_t value, uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    uint64_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

bool ow_is_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

int ow_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    int n = (int)info.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) {
        n = 1;
    }
    if (n > OVERWRITE_MAX_THREADS) {
        n = OVERWRITE_MAX_THREADS;
    }
    return (int)n;
}

int ow_thread_count(int thread_override)
{
    int n = thread_override > 0 ? thread_override : ow_cpu_count();

    if (n < 1) {
        n = 1;
    }
    if (n > OVERWRITE_MAX_THREADS) {
        n = OVERWRITE_MAX_THREADS;
    }
    return n;
}

char *ow_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

int ow_parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || out == NULL || *s == '\0') {
        return -1;
    }
    v = strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

int ow_parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    uint64_t mul = 1;

    if (s == NULL || out == NULL || *s == '\0') {
        return -1;
    }

    v = strtoull(s, &end, 10);
    if (end == s) {
        return -1;
    }

    if (*end != '\0') {
        switch (tolower((unsigned char)*end)) {
        case 'k': mul = 1024ULL; break;
        case 'm': mul = 1024ULL * 1024ULL; break;
        case 'g': mul = 1024ULL * 1024ULL * 1024ULL; break;
        default: return -1;
        }
        end++;
        if (*end == 'b' || *end == 'B') {
            end++;
        }
        if (*end != '\0') {
            return -1;
        }
    }

    *out = (uint64_t)v * mul;
    return 0;
}
