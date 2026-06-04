#ifndef OVERWRITE_PLATFORM_H
#define OVERWRITE_PLATFORM_H

#include "common.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint64_t capacity_bytes;
    uint32_t logical_sector_size;
    uint32_t physical_sector_size;
    bool is_ssd;
    bool is_rotational;
    bool is_removable;
    char model[128];
    char serial[128];
} device_geometry_t;

typedef enum {
    PLATFORM_DEV_RAW,
    PLATFORM_DEV_FILE
} platform_dev_kind_t;

typedef struct platform_device platform_device_t;

int  platform_open(const char *path, platform_dev_kind_t kind, bool need_write,
                   platform_device_t **out);
void platform_print_last_open_error(const char *path);
void platform_print_last_io_error(const char *context);
unsigned long platform_last_io_error(void);
int  platform_get_geometry(platform_device_t *dev, device_geometry_t *geo);
int  platform_pread(platform_device_t *dev, void *buf, size_t len,
                    uint64_t offset);
int  platform_pwrite(platform_device_t *dev, const void *buf, size_t len,
                     uint64_t offset);
int  platform_flush(platform_device_t *dev);
void platform_close(platform_device_t *dev);
bool platform_path_is_block_device(const char *path);
bool platform_path_is_raw_target(const char *path);

void *platform_alloc_aligned(size_t size, size_t alignment);
void  platform_free_aligned(void *ptr);

int  platform_os_random(void *buf, size_t len);
int  platform_ssd_secure_erase(platform_device_t *dev);
int  platform_discard_range(platform_device_t *dev, uint64_t offset,
                            uint64_t length);
int  platform_discard_ranges(platform_device_t *dev, const range_list_t *ranges);
int  platform_raw_fd(platform_device_t *dev);
bool platform_device_is_read_only(platform_device_t *dev);
int  platform_dismount_volumes_on_device(const char *raw_path);
int  platform_set_disk_offline(const char *raw_path, bool offline);
int  platform_mounted_volume_letters(const char *raw_path, char *letters,
                                     size_t letters_len, size_t *count);

#define PLATFORM_MAX_DISABLED_VOLUMES 16

typedef struct {
    bool disk_offlined;
    bool volumes_disabled;
    size_t disabled_count;
    uint32_t disabled_inst[PLATFORM_MAX_DISABLED_VOLUMES];
} platform_disk_detach_t;

void platform_disk_detach_init(platform_disk_detach_t *state);
int  platform_detach_disk_for_wipe(const char *raw_path,
                                   platform_disk_detach_t *state);
int  platform_reattach_disk_after_wipe(const char *raw_path,
                                     const platform_disk_detach_t *state);

#endif
