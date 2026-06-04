#ifndef OVERWRITE_PARTITION_H
#define OVERWRITE_PARTITION_H

#include "common.h"
#include "platform.h"

typedef enum {
    PART_TABLE_NONE,
    PART_TABLE_MBR,
    PART_TABLE_GPT
} part_table_kind_t;

typedef struct {
    uint64_t start;
    uint64_t length;
    char name[64];
    int index;
} partition_entry_t;

typedef struct {
    part_table_kind_t kind;
    partition_entry_t *partitions;
    size_t partition_count;
    byte_range_t *unallocated;
    size_t unallocated_count;
    uint32_t sector_size;
} partition_map_t;

int  partition_map_read(platform_device_t *dev, partition_map_t *map);
void partition_map_free(partition_map_t *map);
int  partition_map_unallocated(const partition_map_t *map, uint64_t capacity,
                               range_list_t *out);

#endif
