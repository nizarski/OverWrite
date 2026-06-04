#ifndef OVERWRITE_DEVLIST_H
#define OVERWRITE_DEVLIST_H

#include <stdio.h>

typedef enum {
    DEVLIST_ALL,
    DEVLIST_DISKS,
    DEVLIST_PARTITIONS,
    DEVLIST_VOLUMES
} devlist_filter_t;

int devlist_parse_filter(const char *s, devlist_filter_t *out);
int devlist_print(FILE *out, devlist_filter_t filter);

#endif
