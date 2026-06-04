#include "partition.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GPT_SIGNATURE 0x5452415020494645ULL /* EFI PART */
#define MBR_SIGNATURE 0xAA55

#pragma pack(push, 1)
typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} mbr_part_t;

typedef struct {
    uint8_t boot[446];
    mbr_part_t parts[4];
    uint16_t signature;
} mbr_t;

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t partition_array_crc32;
} gpt_header_t;

typedef struct {
    uint8_t type_guid[16];
    uint8_t partition_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} gpt_entry_t;
#pragma pack(pop)

static int range_list_push(range_list_t *list, uint64_t start, uint64_t length)
{
    byte_range_t *nr;

    if (length == 0) {
        return 0;
    }
    if (list->count == list->capacity) {
        size_t cap = list->capacity == 0 ? 8 : list->capacity * 2;
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

static void utf16le_to_ascii(const uint16_t *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (i + 1 < dst_len && src[i] != 0) {
        dst[i] = (char)(src[i] & 0xFF);
        i++;
    }
    dst[i] = '\0';
}

static int read_sectors(platform_device_t *dev, uint64_t lba, uint32_t sector_size,
                        void *buf, size_t sectors)
{
    return platform_pread(dev, buf, (size_t)sectors * sector_size,
                          lba * sector_size);
}

static int parse_gpt(platform_device_t *dev, uint32_t sector_size,
                     partition_map_t *map)
{
    gpt_header_t hdr;
    gpt_entry_t *entries;
    size_t i;
    uint8_t *sector_buf;
    size_t entry_bytes;

    if (read_sectors(dev, 1, sector_size, &hdr, 1) != 0) {
        return -1;
    }
    if (hdr.signature != GPT_SIGNATURE || hdr.entry_size < sizeof(gpt_entry_t)) {
        return -1;
    }

    map->kind = PART_TABLE_MBR;
    map->kind = PART_TABLE_GPT;
    entry_bytes = (size_t)hdr.num_entries * hdr.entry_size;
    sector_buf = (uint8_t *)malloc(entry_bytes);
    if (sector_buf == NULL) {
        return -1;
    }
    if (platform_pread(dev, sector_buf, entry_bytes,
                       hdr.partition_entry_lba * sector_size) != 0) {
        free(sector_buf);
        return -1;
    }

    entries = (gpt_entry_t *)sector_buf;
    for (i = 0; i < hdr.num_entries; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(sector_buf + i * hdr.entry_size);
        uint64_t start = e->first_lba * sector_size;
        uint64_t length = (e->last_lba >= e->first_lba)
            ? (e->last_lba - e->first_lba + 1) * sector_size : 0;
        int all_zero = 1;
        size_t j;

        for (j = 0; j < 16; j++) {
            if (e->type_guid[j] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero || length == 0) {
            continue;
        }

        map->partitions = (partition_entry_t *)realloc(
            map->partitions,
            (map->partition_count + 1) * sizeof(partition_entry_t));
        if (map->partitions == NULL) {
            free(sector_buf);
            return -1;
        }
        map->partitions[map->partition_count].start = start;
        map->partitions[map->partition_count].length = length;
        map->partitions[map->partition_count].index = (int)map->partition_count + 1;
        utf16le_to_ascii(e->name, map->partitions[map->partition_count].name,
                         sizeof(map->partitions[map->partition_count].name));
        map->partition_count++;
    }
    (void)entries;
    free(sector_buf);
    return 0;
}

static int parse_mbr(platform_device_t *dev, uint32_t sector_size,
                     partition_map_t *map)
{
    mbr_t mbr;
    int i;

    if (read_sectors(dev, 0, sector_size, &mbr, 1) != 0) {
        return -1;
    }
    if (mbr.signature != MBR_SIGNATURE) {
        return -1;
    }

    map->kind = PART_TABLE_MBR;
    for (i = 0; i < 4; i++) {
        uint64_t start = (uint64_t)mbr.parts[i].lba_start * sector_size;
        uint64_t length = (uint64_t)mbr.parts[i].sector_count * sector_size;

        if (mbr.parts[i].type == 0 || length == 0) {
            continue;
        }

        map->partitions = (partition_entry_t *)realloc(
            map->partitions,
            (map->partition_count + 1) * sizeof(partition_entry_t));
        if (map->partitions == NULL) {
            return -1;
        }
        map->partitions[map->partition_count].start = start;
        map->partitions[map->partition_count].length = length;
        map->partitions[map->partition_count].index = i + 1;
        snprintf(map->partitions[map->partition_count].name,
                 sizeof(map->partitions[map->partition_count].name),
                 "MBR-%d", i + 1);
        map->partition_count++;
    }
    return 0;
}

void partition_map_free(partition_map_t *map)
{
    if (map == NULL) {
        return;
    }
    free(map->partitions);
    free(map->unallocated);
    memset(map, 0, sizeof(*map));
}

int partition_map_read(platform_device_t *dev, partition_map_t *map)
{
    mbr_t mbr;

    if (dev == NULL || map == NULL) {
        return -1;
    }

    memset(map, 0, sizeof(*map));

    {
        device_geometry_t geo;
        if (platform_get_geometry(dev, &geo) == 0 && geo.logical_sector_size > 0) {
            map->sector_size = geo.logical_sector_size;
        } else {
            map->sector_size = 512;
        }
    }

    if (read_sectors(dev, 0, map->sector_size, &mbr, 1) != 0) {
        return -1;
    }
    if (mbr.signature != MBR_SIGNATURE) {
        map->kind = PART_TABLE_NONE;
        return 0;
    }

    if (mbr.parts[0].type == 0xEE) {
        if (parse_gpt(dev, map->sector_size, map) == 0) {
            return 0;
        }
    }
    return parse_mbr(dev, map->sector_size, map);
}

static int cmp_partition(const void *a, const void *b)
{
    const partition_entry_t *pa = (const partition_entry_t *)a;
    const partition_entry_t *pb = (const partition_entry_t *)b;
    if (pa->start < pb->start) return -1;
    if (pa->start > pb->start) return 1;
    return 0;
}

int partition_map_unallocated(const partition_map_t *map, uint64_t capacity,
                              range_list_t *out)
{
    uint64_t cursor = 0;
    size_t i;

    if (map == NULL || out == NULL) {
        return -1;
    }

    out->count = 0;
    if (map->partition_count == 0) {
        return range_list_push(out, 0, capacity);
    }

    partition_entry_t *sorted = (partition_entry_t *)malloc(
        map->partition_count * sizeof(partition_entry_t));
    if (sorted == NULL) {
        return -1;
    }
    memcpy(sorted, map->partitions,
           map->partition_count * sizeof(partition_entry_t));
    qsort(sorted, map->partition_count, sizeof(partition_entry_t), cmp_partition);

    for (i = 0; i < map->partition_count; i++) {
        if (sorted[i].start > cursor) {
            if (range_list_push(out, cursor, sorted[i].start - cursor) != 0) {
                free(sorted);
                return -1;
            }
        }
        uint64_t end = sorted[i].start + sorted[i].length;
        if (end > cursor) {
            cursor = end;
        }
    }
    if (cursor < capacity) {
        if (range_list_push(out, cursor, capacity - cursor) != 0) {
            free(sorted);
            return -1;
        }
    }
    free(sorted);
    return 0;
}
