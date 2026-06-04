#include "platform.h"
#ifdef __linux__
#include "secure_erase_linux.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mntent.h>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/sysmacros.h>
#include <sys/random.h>
#include <linux/nvme_ioctl.h>
#endif

struct platform_device {
    int fd;
    platform_dev_kind_t kind;
    char path[4096];
    bool direct_io;
    bool read_only;
    uint32_t sector_size;
};

static int g_last_open_errno;

void platform_print_last_open_error(const char *path)
{
    int err = g_last_open_errno;

    fprintf(stderr, "error: cannot open %s", path != NULL ? path : "");
    if (err != 0) {
        fprintf(stderr, ": %s", strerror(err));
        if (err == EACCES || err == EPERM) {
            fprintf(stderr, "\nhint: run as root (sudo)");
        } else if (err == ENOENT) {
            fprintf(stderr, "\nhint: check path (overwrite --list)");
        }
    }
    fprintf(stderr, "\n");
}

void platform_print_last_io_error(const char *context)
{
    if (errno != 0) {
        fprintf(stderr, "error: %s: %s\n",
                context != NULL ? context : "I/O", strerror(errno));
    }
}

unsigned long platform_last_io_error(void)
{
    return (unsigned long)errno;
}

int platform_open(const char *path, platform_dev_kind_t kind, bool need_write,
                  platform_device_t **out)
{
    platform_device_t *dev;
    int flags;
    int fd;

    g_last_open_errno = 0;

    if (path == NULL || out == NULL) {
        return -1;
    }

    dev = (platform_device_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return -1;
    }

    flags = need_write ? O_RDWR : O_RDONLY;

    if (kind == PLATFORM_DEV_RAW || platform_path_is_block_device(path)) {
        kind = PLATFORM_DEV_RAW;
#ifdef O_DIRECT
        if (need_write) {
            flags |= O_DIRECT;
        }
#endif
    } else {
        kind = PLATFORM_DEV_FILE;
    }

    fd = open(path, flags);
    if (fd < 0 && (flags & O_DIRECT)) {
        flags &= ~O_DIRECT;
        fd = open(path, flags);
        if (fd >= 0) {
            fprintf(stderr, "warning: O_DIRECT unavailable for %s, using buffered I/O\n", path);
        }
    }
    if (fd < 0 && need_write) {
        g_last_open_errno = errno;
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            fprintf(stderr,
                    "warning: opened %s read-only (insufficient rights for write)\n",
                    path);
            need_write = false;
        }
    }
    if (fd < 0) {
        g_last_open_errno = errno;
        free(dev);
        return -1;
    }

    if (kind == PLATFORM_DEV_RAW && need_write) {
        (void)platform_enable_direct(fd);
    }

    dev->fd = fd;
    dev->kind = kind;
    dev->direct_io = (flags & O_DIRECT) != 0;
    dev->read_only = !need_write;
    strncpy(dev->path, path, sizeof(dev->path) - 1);
    *out = dev;
    return 0;
}

bool platform_device_is_read_only(platform_device_t *dev)
{
    return dev != NULL && dev->read_only;
}

int platform_mounted_volume_letters(const char *raw_path, char *letters,
                                    size_t letters_len, size_t *count)
{
    (void)raw_path;
    (void)letters;
    (void)letters_len;
    if (count != NULL) {
        *count = 0;
    }
    return 0;
}

int platform_dismount_volumes_on_device(const char *raw_path)
{
    FILE *mtab;
    struct mntent *ent;
    int dismounted = 0;
    int attempted = 0;
    char disk_base[128];

    if (raw_path == NULL) {
        return -1;
    }

    strncpy(disk_base, raw_path, sizeof(disk_base) - 1);
    disk_base[sizeof(disk_base) - 1] = '\0';

    mtab = setmntent("/proc/mounts", "r");
    if (mtab == NULL) {
        return -1;
    }

    fprintf(stderr, "dismounting filesystems on %s...\n", raw_path);

    while ((ent = getmntent(mtab)) != NULL) {
        size_t len = strlen(disk_base);

        if (strncmp(ent->mnt_fsname, disk_base, len) != 0) {
            continue;
        }
        if (ent->mnt_fsname[len] != '\0') {
            char c = ent->mnt_fsname[len];
            if (c >= '0' && c <= '9') {
                /* /dev/sdb1 */
            } else if (c == 'p' && len > 4) {
                /* /dev/nvme0n1p1 */
            } else {
                continue;
            }
        }

        attempted++;
        fprintf(stderr, "  umount %s (%s)...\n", ent->mnt_dir, ent->mnt_fsname);
        if (umount(ent->mnt_dir) == 0) {
            dismounted++;
        } else {
            fprintf(stderr, "  failed: %s\n", strerror(errno));
        }
    }
    endmntent(mtab);
    if (attempted > 0 && dismounted < attempted) {
        return -1;
    }
    return 0;
}

int platform_set_disk_offline(const char *raw_path, bool offline)
{
    (void)raw_path;
    (void)offline;
    return -1;
}

void platform_disk_detach_init(platform_disk_detach_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int platform_detach_disk_for_wipe(const char *raw_path,
                                  platform_disk_detach_t *state)
{
    if (raw_path == NULL || state == NULL) {
        return -1;
    }
    platform_disk_detach_init(state);
    return platform_dismount_volumes_on_device(raw_path);
}

int platform_reattach_disk_after_wipe(const char *raw_path,
                                      const platform_disk_detach_t *state)
{
    (void)raw_path;
    (void)state;
    return 0;
}

static int platform_enable_direct(int fd)
{
#ifdef O_DIRECT
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_DIRECT) < 0) {
        return -1;
    }
    return 0;
#else
    (void)fd;
    return -1;
#endif
}

bool platform_path_is_block_device(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISBLK(st.st_mode);
}

bool platform_path_is_raw_target(const char *path)
{
    return platform_path_is_block_device(path);
}

void *platform_alloc_aligned(size_t size, size_t alignment)
{
    void *ptr = NULL;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

void platform_free_aligned(void *ptr)
{
    free(ptr);
}

int platform_os_random(void *buf, size_t len)
{
    size_t done = 0;
    uint8_t *p = (uint8_t *)buf;

    while (done < len) {
        ssize_t n;
#ifdef __linux__
        n = getrandom(p + done, len - done, 0);
#else
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) {
            return -1;
        }
        n = read(fd, p + done, len - done);
        close(fd);
#endif
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int platform_open(const char *path, platform_dev_kind_t kind,
                  platform_device_t **out)
{
    platform_device_t *dev;
    int flags = O_RDWR;
    int fd;

    if (path == NULL || out == NULL) {
        return -1;
    }

    dev = (platform_device_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return -1;
    }

    if (kind == PLATFORM_DEV_RAW || platform_path_is_block_device(path)) {
        kind = PLATFORM_DEV_RAW;
#ifdef O_DIRECT
        flags |= O_DIRECT;
#endif
    } else {
        kind = PLATFORM_DEV_FILE;
    }

    fd = open(path, flags);
    if (fd < 0 && (flags & O_DIRECT)) {
        flags &= ~O_DIRECT;
        fd = open(path, flags);
        if (fd >= 0) {
            fprintf(stderr, "warning: O_DIRECT unavailable for %s, using buffered I/O\n", path);
        }
    }
    if (fd < 0) {
        free(dev);
        return -1;
    }

    if (kind == PLATFORM_DEV_RAW) {
        (void)platform_enable_direct(fd);
    }

    dev->fd = fd;
    dev->kind = kind;
    dev->direct_io = (flags & O_DIRECT) != 0;
    strncpy(dev->path, path, sizeof(dev->path) - 1);
    *out = dev;
    return 0;
}

int platform_get_geometry(platform_device_t *dev, device_geometry_t *geo)
{
    struct stat st;

    if (dev == NULL || geo == NULL) {
        return -1;
    }

    memset(geo, 0, sizeof(*geo));
    geo->logical_sector_size = 512;
    geo->physical_sector_size = 512;

    if (dev->kind == PLATFORM_DEV_RAW) {
#ifdef __linux__
        uint64_t size64 = 0;
        int logical = 512;
        int physical = 512;

        if (fstat(dev->fd, &st) != 0) {
            return -1;
        }

        if (ioctl(dev->fd, BLKGETSIZE64, &size64) == 0) {
            geo->capacity_bytes = size64;
        }
        if (ioctl(dev->fd, BLKSSZGET, &logical) == 0 && logical > 0) {
            geo->logical_sector_size = (uint32_t)logical;
        }
#ifdef BLKPBSZGET
        if (ioctl(dev->fd, BLKPBSZGET, &physical) == 0 && physical > 0) {
            geo->physical_sector_size = (uint32_t)physical;
        } else
#endif
        {
            geo->physical_sector_size = geo->logical_sector_size;
        }

        {
            char ro_path[512];
            FILE *f;
            snprintf(ro_path, sizeof(ro_path),
                     "/sys/dev/block/%u:%u/queue/rotational",
                     major(st.st_rdev), minor(st.st_rdev));
            f = fopen(ro_path, "r");
            if (f != NULL) {
                int rot = 1;
                if (fscanf(f, "%d", &rot) == 1) {
                    geo->is_rotational = rot != 0;
                    geo->is_ssd = rot == 0;
                }
                fclose(f);
            }
        }

        {
            char rem_path[512];
            FILE *f;

            snprintf(rem_path, sizeof(rem_path),
                     "/sys/dev/block/%u:%u/removable",
                     major(st.st_rdev), minor(st.st_rdev));
            f = fopen(rem_path, "r");
            if (f != NULL) {
                int rem = 0;
                if (fscanf(f, "%d", &rem) == 1 && rem != 0) {
                    geo->is_removable = true;
                }
                fclose(f);
            }
        }
#else
        off_t end = lseek(dev->fd, 0, SEEK_END);
        if (end >= 0) {
            geo->capacity_bytes = (uint64_t)end;
            lseek(dev->fd, 0, SEEK_SET);
        }
#endif
    } else {
        if (fstat(dev->fd, &st) == 0) {
            geo->capacity_bytes = (uint64_t)st.st_size;
        }
        geo->is_rotational = false;
        geo->is_ssd = false;
    }

    if (fstat(dev->fd, &st) == 0 && dev->kind == PLATFORM_DEV_RAW) {
        char model_path[512];
        FILE *f;
        snprintf(model_path, sizeof(model_path),
                 "/sys/dev/block/%u:%u/device/model",
                 major(st.st_rdev), minor(st.st_rdev));
        f = fopen(model_path, "r");
        if (f != NULL) {
            if (fgets(geo->model, sizeof(geo->model), f) != NULL) {
                size_t n = strlen(geo->model);
                while (n > 0 && (geo->model[n - 1] == '\n' || geo->model[n - 1] == ' ')) {
                    geo->model[--n] = '\0';
                }
            }
            fclose(f);
        }
    }

    dev->sector_size = geo->logical_sector_size;
    if (!ow_is_power_of_two(dev->sector_size)) {
        dev->sector_size = 512;
        geo->logical_sector_size = 512;
    }
    return 0;
}

int platform_pread(platform_device_t *dev, void *buf, size_t len, uint64_t offset)
{
    ssize_t n;

    if (dev == NULL) {
        return -1;
    }

    n = pread(dev->fd, buf, len, (off_t)offset);
    return n == (ssize_t)len ? 0 : -1;
}

int platform_pwrite(platform_device_t *dev, const void *buf, size_t len,
                    uint64_t offset)
{
    ssize_t n;
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;

    if (dev == NULL) {
        return -1;
    }

    while (done < len) {
        n = pwrite(dev->fd, p + done, len - done, (off_t)(offset + done));
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int platform_flush(platform_device_t *dev)
{
    if (dev == NULL) {
        return -1;
    }
    return fsync(dev->fd) == 0 ? 0 : -1;
}

void platform_close(platform_device_t *dev)
{
    if (dev == NULL) {
        return;
    }
    close(dev->fd);
    free(dev);
}

int platform_raw_fd(platform_device_t *dev)
{
    if (dev == NULL || dev->kind != PLATFORM_DEV_RAW) {
        return -1;
    }
    return dev->fd;
}

int platform_ssd_secure_erase(platform_device_t *dev)
{
    device_geometry_t geo;
    bool is_nvme;

    if (dev == NULL || dev->kind != PLATFORM_DEV_RAW) {
        return -1;
    }

    if (platform_get_geometry(dev, &geo) != 0) {
        return -1;
    }

#ifdef __linux__
    is_nvme = strstr(dev->path, "nvme") != NULL;

    if (is_nvme) {
        fprintf(stderr, "attempting NVMe Format NVM (user data erase)...\n");
        if (linux_nvme_format_nvm(dev->fd) == 0) {
            return 0;
        }
        fprintf(stderr, "warning: NVMe format command failed\n");
    } else {
        if (linux_ata_security_erase(dev->fd) == 0) {
            return 0;
        }
    }

    if (geo.capacity_bytes > 0) {
        fprintf(stderr, "attempting secure discard of full device...\n");
        if (linux_blk_secure_discard(dev->fd, geo.capacity_bytes) == 0) {
            return 0;
        }
    }
#else
    (void)is_nvme;
    if (geo.capacity_bytes > 0) {
        uint64_t range[2] = { 0, geo.capacity_bytes };
        if (ioctl(dev->fd, BLKDISCARD, range) == 0) {
            return 0;
        }
    }
#endif

    return -1;
}

int platform_discard_range(platform_device_t *dev, uint64_t offset,
                           uint64_t length)
{
#ifdef __linux__
    uint64_t range[2];

    if (dev == NULL || length == 0) {
        return -1;
    }
    range[0] = offset;
    range[1] = length;
    return ioctl(dev->fd, BLKDISCARD, range) == 0 ? 0 : -1;
#else
    (void)dev;
    (void)offset;
    (void)length;
    return -1;
#endif
}

int platform_discard_ranges(platform_device_t *dev, const range_list_t *ranges)
{
    size_t i;
    int rc = 0;

    if (dev == NULL || ranges == NULL) {
        return -1;
    }

    for (i = 0; i < ranges->count; i++) {
        if (platform_discard_range(dev, ranges->ranges[i].offset,
                                   ranges->ranges[i].length) != 0) {
            rc = -1;
        }
    }
    return rc;
}

#endif /* !_WIN32 */
