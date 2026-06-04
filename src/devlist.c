#include "devlist.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void format_size(uint64_t bytes, char *buf, size_t buflen)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int i = 0;

    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    snprintf(buf, buflen, "%.1f %s", v, u[i]);
}

static void print_row(FILE *out, const char *type, const char *path,
                      uint64_t size, const char *extra)
{
    char sz[32];
    format_size(size, sz, sizeof(sz));
    fprintf(out, "%-12s %-32s %10s  %s\n", type, path, sz,
            extra != NULL ? extra : "");
}

int devlist_parse_filter(const char *s, devlist_filter_t *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    if (strcmp(s, "all") == 0 || s[0] == '\0') {
        *out = DEVLIST_ALL;
    } else if (strcmp(s, "disks") == 0 || strcmp(s, "disk") == 0) {
        *out = DEVLIST_DISKS;
    } else if (strcmp(s, "partitions") == 0 || strcmp(s, "partition") == 0) {
        *out = DEVLIST_PARTITIONS;
    } else if (strcmp(s, "volumes") == 0 || strcmp(s, "volume") == 0) {
        *out = DEVLIST_VOLUMES;
    } else {
        return -1;
    }
    return 0;
}

#ifndef _WIN32

#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mntent.h>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/sysmacros.h>
#endif

static int read_sysfs_string(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    if (fgets(buf, (int)len, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
            buf[--n] = '\0';
        }
    }
    return 0;
}

static int read_sysfs_u64(const char *path, uint64_t *out)
{
    char buf[64];
    if (read_sysfs_string(path, buf, sizeof(buf)) != 0) {
        return -1;
    }
    return ow_parse_u64(buf, out) == 0 ? 0 : -1;
}

static bool linux_name_is_partition(const char *name)
{
    size_t n = strlen(name);

    if (strncmp(name, "nvme", 4) == 0 && n > 4) {
        return strchr(name + 4, 'p') != NULL;
    }
    if (strncmp(name, "mmcblk", 6) == 0 && n > 6) {
        return name[n - 1] != 'k' && strchr(name, 'p') != NULL;
    }
    if (strncmp(name, "loop", 4) == 0) {
        return false;
    }
    if (n >= 4 && strncmp(name, "sd", 2) == 0) {
        return isdigit((unsigned char)name[n - 1]);
    }
    if (n >= 4 && strncmp(name, "vd", 2) == 0) {
        return isdigit((unsigned char)name[n - 1]);
    }
    return false;
}

static bool linux_name_is_disk(const char *name)
{
    if (linux_name_is_partition(name)) {
        return false;
    }
    if (strncmp(name, "loop", 4) == 0) {
        return true;
    }
    if (strncmp(name, "ram", 3) == 0) {
        return false;
    }
    if (strncmp(name, "fd", 2) == 0) {
        return false;
    }
    if (strncmp(name, "dm-", 3) == 0) {
        return false;
    }
    return strncmp(name, "sd", 2) == 0 ||
           strncmp(name, "vd", 2) == 0 ||
           strncmp(name, "nvme", 4) == 0 ||
           strncmp(name, "mmcblk", 6) == 0;
}

static void linux_mount_lookup(const char *devpath, char *mount_out, size_t mlen,
                               char *fstype_out, size_t flen)
{
    FILE *mtab = setmntent("/proc/mounts", "r");
    struct mntent *ent;

    if (mount_out && mlen > 0) {
        mount_out[0] = '\0';
    }
    if (fstype_out && flen > 0) {
        fstype_out[0] = '\0';
    }
    if (mtab == NULL) {
        return;
    }

    while ((ent = getmntent(mtab)) != NULL) {
        if (strcmp(ent->mnt_fsname, devpath) == 0) {
            if (mount_out && mlen > 0) {
                strncpy(mount_out, ent->mnt_dir, mlen - 1);
                mount_out[mlen - 1] = '\0';
            }
            if (fstype_out && flen > 0) {
                strncpy(fstype_out, ent->mnt_type, flen - 1);
                fstype_out[flen - 1] = '\0';
            }
            break;
        }
    }
    endmntent(mtab);
}

static uint64_t linux_block_size_bytes(const char *name)
{
    char path[512];
    uint64_t logical = 512;

    snprintf(path, sizeof(path), "/sys/block/%s/queue/logical_block_size", name);
    (void)read_sysfs_u64(path, &logical);
    if (logical == 0) {
        logical = 512;
    }
    return logical;
}

static uint64_t linux_block_capacity(const char *name)
{
    char path[512];
    uint64_t sectors = 0;
    uint64_t bs;

    snprintf(path, sizeof(path), "/sys/block/%s/size", name);
    if (read_sysfs_u64(path, &sectors) != 0) {
        return 0;
    }
    bs = linux_block_size_bytes(name);
    return sectors * bs;
}

static void linux_block_extra(const char *name, char *extra, size_t elen)
{
    char path[512];
    char model[128];
    char rot[16];

    extra[0] = '\0';
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", name);
    if (read_sysfs_string(path, model, sizeof(model)) == 0 && model[0] != '\0') {
        snprintf(extra, elen, "%s", model);
    }

    snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", name);
    if (read_sysfs_string(path, rot, sizeof(rot)) == 0) {
        if (extra[0] != '\0') {
            strncat(extra, " | ", elen - strlen(extra) - 1);
        }
        strncat(extra, rot[0] == '0' ? "SSD" : "HDD",
                elen - strlen(extra) - 1);
    }
}

static int linux_print_block(FILE *out, devlist_filter_t filter)
{
    DIR *dir = opendir("/sys/block");
    struct dirent *ent;
    char devpath[320];
    char extra[256];
    char mount[256];
    char fstype[64];

    if (dir == NULL) {
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        bool is_part;
        bool is_disk;

        if (ent->d_name[0] == '.') {
            continue;
        }

        is_part = linux_name_is_partition(ent->d_name);
        is_disk = linux_name_is_disk(ent->d_name);

        if (is_disk && filter == DEVLIST_PARTITIONS) {
            continue;
        }
        if (is_part && filter == DEVLIST_DISKS) {
            continue;
        }
        if (!is_disk && !is_part) {
            continue;
        }
        if (filter == DEVLIST_VOLUMES) {
            continue;
        }

        snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);
        linux_block_extra(ent->d_name, extra, sizeof(extra));
        linux_mount_lookup(devpath, mount, sizeof(mount), fstype, sizeof(fstype));

        if (mount[0] != '\0') {
            char mextra[512];
            snprintf(mextra, sizeof(mextra), "%s%s%s%s",
                     extra[0] != '\0' ? extra : "",
                     extra[0] != '\0' ? " | " : "",
                     "mount ", mount);
            if (fstype[0] != '\0') {
                strncat(mextra, " (", sizeof(mextra) - strlen(mextra) - 1);
                strncat(mextra, fstype, sizeof(mextra) - strlen(mextra) - 1);
                strncat(mextra, ")", sizeof(mextra) - strlen(mextra) - 1);
            }
            print_row(out, is_disk ? "disk" : "partition", devpath,
                      linux_block_capacity(ent->d_name), mextra);
        } else {
            print_row(out, is_disk ? "disk" : "partition", devpath,
                      linux_block_capacity(ent->d_name),
                      extra[0] != '\0' ? extra : NULL);
        }
    }

    closedir(dir);
    return 0;
}

static int linux_print_mounts(FILE *out, devlist_filter_t filter)
{
    FILE *mtab;
    struct mntent *ent;

    if (filter != DEVLIST_ALL && filter != DEVLIST_VOLUMES) {
        return 0;
    }

    mtab = setmntent("/proc/mounts", "r");
    if (mtab == NULL) {
        return -1;
    }

    while ((ent = getmntent(mtab)) != NULL) {
        struct stat st;

        if (strncmp(ent->mnt_fsname, "/dev/", 5) != 0) {
            continue;
        }
        if (stat(ent->mnt_fsname, &st) != 0 || !S_ISBLK(st.st_mode)) {
            continue;
        }

        {
            char extra[256];
            snprintf(extra, sizeof(extra), "mount %s (%s)",
                     ent->mnt_dir, ent->mnt_type);
            print_row(out, "volume", ent->mnt_fsname, 0, extra);
        }
    }
    endmntent(mtab);
    return 0;
}

int devlist_print(FILE *out, devlist_filter_t filter)
{
    fprintf(out, "\nStorage devices (Linux)\n");
    fprintf(out, "%-12s %-32s %10s  %s\n",
            "TYPE", "PATH", "SIZE", "INFO");
    fprintf(out, "%-12s %-32s %10s  %s\n",
            "----", "----", "----", "----");

    if (filter == DEVLIST_ALL || filter == DEVLIST_DISKS ||
        filter == DEVLIST_PARTITIONS) {
        if (linux_print_block(out, filter) != 0) {
            fprintf(out, "(could not read /sys/block)\n");
        }
    }

    if (filter == DEVLIST_ALL || filter == DEVLIST_VOLUMES) {
        linux_print_mounts(out, filter);
    }

    fprintf(out, "\nUse path as overwrite target. Block devices need root.\n");
    return 0;
}

#else /* _WIN32 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <fileapi.h>

static int win_open_drive(int index, HANDLE *out, uint64_t *size_bytes,
                          char *model, size_t model_len)
{
    char path[64];
    HANDLE h;
    DISK_GEOMETRY_EX geom;
    GET_LENGTH_INFORMATION len;
    DWORD bytes = 0;

    snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", index);
    h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    *size_bytes = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        NULL, 0, &geom, sizeof(geom), &bytes, NULL)) {
        *size_bytes = (uint64_t)geom.DiskSize.QuadPart;
    } else if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                               NULL, 0, &len, sizeof(len), &bytes, NULL)) {
        *size_bytes = (uint64_t)len.Length.QuadPart;
    }

    if (model != NULL && model_len > 0) {
        STORAGE_PROPERTY_QUERY query;
        BYTE buf[1024];

        model[0] = '\0';
        memset(&query, 0, sizeof(query));
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &query, sizeof(query), buf, sizeof(buf),
                            &bytes, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)buf;
            if (desc->ProductIdOffset != 0 &&
                desc->ProductIdOffset < bytes) {
                const char *pid = (const char *)buf + desc->ProductIdOffset;
                strncpy(model, pid, model_len - 1);
                model[model_len - 1] = '\0';
            }
        }
    }

    *out = h;
    return 0;
}

static void win_print_physical(FILE *out, devlist_filter_t filter)
{
    int i;
    char extra[256];

    if (filter != DEVLIST_ALL && filter != DEVLIST_DISKS) {
        return;
    }

    for (i = 0; i < 64; i++) {
        HANDLE h;
        uint64_t size = 0;
        char path[64];
        char model[128];

        if (win_open_drive(i, &h, &size, model, sizeof(model)) != 0) {
            continue;
        }
        CloseHandle(h);

        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", i);
        extra[0] = '\0';
        if (model[0] != '\0') {
            snprintf(extra, sizeof(extra), "%s", model);
        }
        print_row(out, "physical", path, size, extra[0] != '\0' ? extra : NULL);
    }
}

static void win_print_volume_disk_map(FILE *out)
{
    char drives[512];
    char *p;

    if (GetLogicalDriveStringsA(sizeof(drives) - 1, drives) == 0) {
        return;
    }

    fprintf(out, "\nVolume -> physical disk mapping:\n");
    fprintf(out, "%-8s %-28s %s\n", "LETTER", "VOLUME PATH", "PHYSICAL DISK");
    fprintf(out, "%-8s %-28s %s\n", "------", "-----------", "-------------");

    for (p = drives; *p != '\0'; p += strlen(p) + 1) {
        char volpath[16];
        HANDLE h;
        STORAGE_DEVICE_NUMBER num;
        DWORD bytes = 0;

        snprintf(volpath, sizeof(volpath), "\\\\.\\%c:", p[0]);
        h = CreateFileA(volpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        memset(&num, 0, sizeof(num));
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            NULL, 0, &num, sizeof(num), &bytes, NULL)) {
            fprintf(out, "%-8s %-28s PhysicalDrive%u\n",
                    p, volpath, (unsigned)num.DeviceNumber);
        }
        CloseHandle(h);
    }
    fprintf(out, "\n");
}

static void win_print_volumes(FILE *out, devlist_filter_t filter)
{
    char drives[512];
    char *p;

    if (filter != DEVLIST_ALL && filter != DEVLIST_VOLUMES &&
        filter != DEVLIST_PARTITIONS) {
        return;
    }

    if (GetLogicalDriveStringsA(sizeof(drives) - 1, drives) == 0) {
        return;
    }

    for (p = drives; *p != '\0'; p += strlen(p) + 1) {
        char root[16];
        char volpath[16];
        char label[256];
        char fs[64];
        DWORD serial = 0;
        DWORD max_comp = 0;
        DWORD flags = 0;
        ULARGE_INTEGER total = {0};
        char extra[512];

        root[0] = p[0];
        root[1] = p[1];
        root[2] = p[2];
        root[3] = '\0';
        snprintf(volpath, sizeof(volpath), "\\\\.\\%c:", p[0]);

        if (!GetVolumeInformationA(root, label, sizeof(label),
                                   &serial, &max_comp, &flags,
                                   fs, sizeof(fs))) {
            continue;
        }

        if (!GetDiskFreeSpaceExA(root, NULL, &total, NULL)) {
            total.QuadPart = 0;
        }

        extra[0] = '\0';
        if (label[0] != '\0') {
            snprintf(extra, sizeof(extra), "label \"%s\"", label);
        }
        if (fs[0] != '\0') {
            char fsbuf[128];
            snprintf(fsbuf, sizeof(fsbuf), "%s%s%s",
                     extra[0] != '\0' ? extra : "",
                     extra[0] != '\0' ? " | " : "",
                     fs);
            strncpy(extra, fsbuf, sizeof(extra) - 1);
        }
        {
            char mnt[64];
            snprintf(mnt, sizeof(mnt), "mount %s", root);
            if (extra[0] != '\0') {
                strncat(extra, " | ", sizeof(extra) - strlen(extra) - 1);
            }
            strncat(extra, mnt, sizeof(extra) - strlen(extra) - 1);
        }

        print_row(out, "volume", volpath, (uint64_t)total.QuadPart, extra);
    }
}

static void win_print_partitions(FILE *out, devlist_filter_t filter)
{
    int disk;

    if (filter != DEVLIST_ALL && filter != DEVLIST_PARTITIONS) {
        return;
    }

    for (disk = 0; disk < 64; disk++) {
        int part;
        for (part = 1; part < 128; part++) {
            char path[64];
            HANDLE h;
            GET_LENGTH_INFORMATION len;
            DWORD bytes = 0;

            snprintf(path, sizeof(path),
                     "\\\\.\\Harddisk%dPartition%d", disk, part);
            h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                if (part == 1) {
                    break;
                }
                continue;
            }

            len.Length.QuadPart = 0;
            if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                                NULL, 0, &len, sizeof(len), &bytes, NULL)) {
                char extra[64];
                snprintf(extra, sizeof(extra), "disk %d part %d", disk, part);
                print_row(out, "partition", path,
                          (uint64_t)len.Length.QuadPart, extra);
            }
            CloseHandle(h);
        }
    }
}

int devlist_print(FILE *out, devlist_filter_t filter)
{
    fprintf(out, "\nStorage devices (Windows)\n");
    fprintf(out, "%-12s %-32s %10s  %s\n",
            "TYPE", "PATH", "SIZE", "INFO");
    fprintf(out, "%-12s %-32s %10s  %s\n",
            "----", "----", "----", "----");

    win_print_physical(out, filter);
    win_print_partitions(out, filter);
    win_print_volumes(out, filter);
    win_print_volume_disk_map(out);

    fprintf(out, "Use path as overwrite target. Physical drives need Administrator.\n");
    fprintf(out, "Wiping PhysicalDriveN erases that entire disk (all partitions on it).\n");
    fprintf(out, "Your main PC data is usually on PhysicalDrive0/1, not small USB disks.\n");
    return 0;
}

#endif /* _WIN32 */
