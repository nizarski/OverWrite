#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>

#ifndef FSCTL_TRIM
#define FSCTL_TRIM CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 255, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif

#define WIN_PHYSICAL_DRIVE_PREFIX "\\\\.\\PhysicalDrive"
#define WIN_PHYSICAL_DRIVE_PREFIX_LEN (sizeof(WIN_PHYSICAL_DRIVE_PREFIX) - 1)

struct platform_device {
    HANDLE handle;
    platform_dev_kind_t kind;
    char path[4096];
    bool direct_io;
    bool read_only;
    uint32_t sector_size;
    LARGE_INTEGER size;
};

static bool path_looks_like_physical_drive(const char *path)
{
    return _strnicmp(path, "\\\\.\\", 4) == 0;
}

bool platform_path_is_block_device(const char *path)
{
    return path_looks_like_physical_drive(path);
}

bool platform_path_is_raw_target(const char *path)
{
    return path_looks_like_physical_drive(path);
}

void *platform_alloc_aligned(size_t size, size_t alignment)
{
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    return _aligned_malloc(size, alignment);
}

void platform_free_aligned(void *ptr)
{
    _aligned_free(ptr);
}

int platform_os_random(void *buf, size_t len)
{
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? 0 : -1;
}

static DWORD g_last_open_error;
static DWORD g_last_io_error;

void platform_print_last_open_error(const char *path)
{
    DWORD err = g_last_open_error;

    fprintf(stderr, "error: cannot open %s", path != NULL ? path : "");
    if (err != 0) {
        LPSTR msg = NULL;
        fprintf(stderr, " (Win32 %lu", (unsigned long)err);
        if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, err, 0, (LPSTR)&msg, 0, NULL) &&
            msg != NULL) {
            size_t n = strlen(msg);
            while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) {
                msg[--n] = '\0';
            }
            fprintf(stderr, ": %s", msg);
            LocalFree(msg);
        }
        fprintf(stderr, ")");
        if (err == ERROR_ACCESS_DENIED) {
            fprintf(stderr, "\nhint: run PowerShell or cmd as Administrator");
        } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "\nhint: check path (overwrite --list)");
        } else if (err == ERROR_SHARING_VIOLATION) {
            fprintf(stderr, "\nhint: close programs using the disk");
        }
    }
    fprintf(stderr, "\n");
}

unsigned long platform_last_io_error(void)
{
    return (unsigned long)g_last_io_error;
}

void platform_print_last_io_error(const char *context)
{
    DWORD err = g_last_io_error;

    if (err == 0) {
        return;
    }
    fprintf(stderr, "error: %s", context != NULL ? context : "I/O");
    fprintf(stderr, " (Win32 %lu", (unsigned long)err);
    {
        LPSTR msg = NULL;

        if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, err, 0, (LPSTR)&msg, 0, NULL) &&
            msg != NULL) {
            size_t n = strlen(msg);

            while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) {
                msg[--n] = '\0';
            }
            fprintf(stderr, ": %s", msg);
            LocalFree(msg);
        }
    }
    fprintf(stderr, ")");
    if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
        fprintf(stderr,
                "\nhint: close Explorer and apps on this drive; "
                "dismount may have failed");
    }
    fprintf(stderr, "\n");
}

int platform_open(const char *path, platform_dev_kind_t kind, bool need_write,
                  platform_device_t **out)
{
    platform_device_t *dev;
    DWORD access = need_write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD flags = FILE_FLAG_OVERLAPPED;
    HANDLE h;

    g_last_open_error = 0;

    if (path == NULL || out == NULL) {
        return -1;
    }

    dev = (platform_device_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return -1;
    }

    if (kind == PLATFORM_DEV_RAW || path_looks_like_physical_drive(path)) {
        kind = PLATFORM_DEV_RAW;
        if (need_write) {
            flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
            /* Exclusive open helps lock out mounted volumes. */
            share = 0;
        }
    } else {
        kind = PLATFORM_DEV_FILE;
    }

    h = CreateFileA(path, access, share, NULL, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE && kind == PLATFORM_DEV_RAW && need_write &&
        share == 0) {
        g_last_open_error = GetLastError();
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        h = CreateFileA(path, access, share, NULL, OPEN_EXISTING, flags, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            fprintf(stderr,
                    "warning: could not open %s exclusively; "
                    "close apps using this disk\n",
                    path);
        }
    }
    if (h == INVALID_HANDLE_VALUE && (flags & FILE_FLAG_NO_BUFFERING)) {
        flags &= ~(FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
        h = CreateFileA(path, access, share, NULL, OPEN_EXISTING, flags, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            fprintf(stderr, "warning: unbuffered I/O unavailable for %s\n", path);
        }
    }
    if (h == INVALID_HANDLE_VALUE && need_write) {
        g_last_open_error = GetLastError();
        h = CreateFileA(path, GENERIC_READ, share, NULL, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            fprintf(stderr,
                    "warning: opened %s read-only (insufficient rights for write)\n",
                    path);
            need_write = false;
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        g_last_open_error = GetLastError();
        free(dev);
        return -1;
    }

    dev->handle = h;
    dev->kind = kind;
    dev->direct_io = (flags & FILE_FLAG_NO_BUFFERING) != 0;
    dev->read_only = !need_write;
    strncpy(dev->path, path, sizeof(dev->path) - 1);
    *out = dev;
    return 0;
}

bool platform_device_is_read_only(platform_device_t *dev)
{
    return dev != NULL && dev->read_only;
}

static int win_parse_physical_drive_index(const char *path, int *index)
{
    unsigned n;

    if (path == NULL || index == NULL) {
        return -1;
    }
    if (_strnicmp(path, WIN_PHYSICAL_DRIVE_PREFIX,
                  WIN_PHYSICAL_DRIVE_PREFIX_LEN) != 0) {
        return -1;
    }
    if (sscanf(path + WIN_PHYSICAL_DRIVE_PREFIX_LEN, "%u", &n) != 1) {
        return -1;
    }
    *index = (int)n;
    return 0;
}

static int win_volume_disk_index(char letter, int *disk_out)
{
    char path[8];
    HANDLE h;
    STORAGE_DEVICE_NUMBER num;
    DWORD bytes = 0;

    snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
    h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    memset(&num, 0, sizeof(num));
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                         NULL, 0, &num, sizeof(num), &bytes, NULL)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    *disk_out = (int)num.DeviceNumber;
    return 0;
}

int platform_mounted_volume_letters(const char *raw_path, char *letters,
                                    size_t letters_len, size_t *count)
{
    char drives[512];
    char *p;
    int disk;
    size_t n = 0;

    if (raw_path == NULL || letters == NULL || letters_len == 0 ||
        count == NULL) {
        return -1;
    }

    *count = 0;
    letters[0] = '\0';

    if (win_parse_physical_drive_index(raw_path, &disk) != 0) {
        return 0;
    }

    if (GetLogicalDriveStringsA(sizeof(drives) - 1, drives) == 0) {
        return -1;
    }

    for (p = drives; *p != '\0'; p += strlen(p) + 1) {
        int vol_disk;
        char letter = (char)toupper((unsigned char)p[0]);

        if (win_volume_disk_index(letter, &vol_disk) != 0) {
            continue;
        }
        if (vol_disk != disk) {
            continue;
        }
        if (n + 2 < letters_len) {
            if (n > 0) {
                letters[n++] = ',';
            }
            letters[n++] = letter;
            letters[n++] = ':';
            letters[n] = '\0';
            (*count)++;
        }
    }
    return 0;
}

#ifndef FSCTL_FORCE_VOLUME_DISMOUNT
#define FSCTL_FORCE_VOLUME_DISMOUNT \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 24, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static int win_dismount_handle(HANDLE h)
{
    DWORD bytes = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytes, NULL)) {
        return -1;
    }
    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytes, NULL)) {
        if (!DeviceIoControl(h, FSCTL_FORCE_VOLUME_DISMOUNT,
                             NULL, 0, NULL, 0, &bytes, NULL)) {
            return -1;
        }
    }
    return 0;
}

static int win_dismount_letter(char letter)
{
    char path[8];
    HANDLE h;

    snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (win_dismount_handle(h) != 0) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    return 0;
}

static int win_volume_belongs_to_disk(HANDLE vol, int disk)
{
    BYTE buf[512];
    VOLUME_DISK_EXTENTS *ext = (VOLUME_DISK_EXTENTS *)buf;
    DWORD bytes = 0;
    DWORD i;

    if (!DeviceIoControl(vol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         NULL, 0, buf, sizeof(buf), &bytes, NULL)) {
        return 0;
    }
    if (bytes < sizeof(VOLUME_DISK_EXTENTS)) {
        return 0;
    }
    for (i = 0; i < ext->NumberOfDiskExtents; i++) {
        if ((int)ext->Extents[i].DiskNumber == disk) {
            return 1;
        }
    }
    return 0;
}

static int win_open_volume_on_disk(const WCHAR *vol_name, int disk, HANDLE *out)
{
    WCHAR open_path[256];
    HANDLE h;
    size_t len;
    const WCHAR *suffix;

    *out = INVALID_HANDLE_VALUE;
    if (vol_name == NULL) {
        return -1;
    }

    len = wcslen(vol_name);
    if (len < 12 || _wcsnicmp(vol_name, L"\\\\?\\", 4) != 0) {
        return -1;
    }

    suffix = vol_name + 4;
    if (len > 0 && vol_name[len - 1] == L'\\') {
        _snwprintf(open_path, sizeof(open_path) / sizeof(WCHAR),
                   L"\\\\.\\%.*s", (int)(len - 5), suffix);
    } else {
        _snwprintf(open_path, sizeof(open_path) / sizeof(WCHAR),
                   L"\\\\.\\%s", suffix);
    }

    h = CreateFileW(open_path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE || !win_volume_belongs_to_disk(h, disk)) {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
        return -1;
    }

    *out = h;
    return 0;
}

static int win_dismount_volumes_findfirst(int disk, int *attempted, int *dismounted)
{
    WCHAR vol_name[MAX_PATH];
    HANDLE find;
    BOOL ok;

    find = FindFirstVolumeW(vol_name, ARRAYSIZE(vol_name));
    if (find == INVALID_HANDLE_VALUE) {
        fprintf(stderr,
                "  warning: volume enumeration failed (Win32 %lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    do {
        HANDLE h = INVALID_HANDLE_VALUE;
        char narrow[MAX_PATH];
        char display[80];

        if (win_open_volume_on_disk(vol_name, disk, &h) == 0) {
            (*attempted)++;
            if (WideCharToMultiByte(CP_UTF8, 0, vol_name, -1, narrow,
                                    sizeof(narrow), NULL, NULL) > 0) {
                snprintf(display, sizeof(display), "%.70s", narrow);
            } else {
                display[0] = '\0';
            }
            fprintf(stderr, "  locking volume %s ...\n",
                    display[0] != '\0' ? display : "(guid)");
            if (win_dismount_handle(h) == 0) {
                fprintf(stderr, "  volume dismounted\n");
                (*dismounted)++;
            } else {
                fprintf(stderr,
                        "  volume failed (close apps using this disk)\n");
            }
            CloseHandle(h);
        }
        ok = FindNextVolumeW(find, vol_name, ARRAYSIZE(vol_name));
    } while (ok);

    FindVolumeClose(find);
    return 0;
}

static void win_remove_letters_on_disk(int disk)
{
    char drives[512];
    char *p;

    if (GetLogicalDriveStringsA(sizeof(drives) - 1, drives) == 0) {
        return;
    }

    for (p = drives; *p != '\0'; p += strlen(p) + 1) {
        int vol_disk;
        char letter = (char)toupper((unsigned char)p[0]);
        char mount_point[4];

        if (win_volume_disk_index(letter, &vol_disk) != 0 || vol_disk != disk) {
            continue;
        }
        snprintf(mount_point, sizeof(mount_point), "%c:\\", letter);
        if (DeleteVolumeMountPointA(mount_point)) {
            fprintf(stderr, "  removed drive letter %c:\n", letter);
        }
    }
}

static int win_dismount_gpt_partitions(const char *raw_path, int disk,
                                       int *attempted, int *dismounted)
{
    HANDLE h_disk;
    BYTE buf[262144];
    PDRIVE_LAYOUT_INFORMATION_EX layout = (PDRIVE_LAYOUT_INFORMATION_EX)buf;
    DWORD returned = 0;
    DWORD i;

    h_disk = CreateFileA(raw_path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (h_disk == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!DeviceIoControl(h_disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                         NULL, 0, buf, sizeof(buf), &returned, NULL)) {
        CloseHandle(h_disk);
        return -1;
    }
    CloseHandle(h_disk);

    fprintf(stderr, "  dismounting GPT partitions on disk %d...\n", disk);

    for (i = 0; i < layout->PartitionCount; i++) {
        PARTITION_INFORMATION_EX *pe = &layout->PartitionEntry[i];
        WCHAR vol_path[128];
        HANDLE h_vol;
        const GUID *g;

        if (pe->PartitionLength.QuadPart == 0) {
            continue;
        }
        if (pe->PartitionStyle != PARTITION_STYLE_GPT) {
            continue;
        }

        g = &pe->Gpt.PartitionId;
        _snwprintf(vol_path, sizeof(vol_path) / sizeof(WCHAR),
                   L"\\\\.\\Volume{%08lx-%04x-%04x-%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x}\\",
                   (unsigned long)g->Data1, g->Data2, g->Data3,
                   g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
                   g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);

        h_vol = CreateFileW(vol_path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h_vol == INVALID_HANDLE_VALUE) {
            continue;
        }

        (*attempted)++;
        fprintf(stderr, "  partition %lu (%llu bytes): locking...\n",
                (unsigned long)pe->PartitionNumber,
                (unsigned long long)pe->PartitionLength.QuadPart);
        if (win_dismount_handle(h_vol) == 0) {
            fprintf(stderr, "  partition %lu: dismounted\n",
                    (unsigned long)pe->PartitionNumber);
            (*dismounted)++;
        } else {
            fprintf(stderr,
                    "  partition %lu: failed (close apps using this disk)\n",
                    (unsigned long)pe->PartitionNumber);
        }
        CloseHandle(h_vol);
    }

    return 0;
}

static int win_dismount_pass(const char *raw_path, int disk,
                             const char drives[512],
                             int *attempted, int *dismounted)
{
    const char *p;
    int prev_attempted = *attempted;

    for (p = drives; *p != '\0'; p += strlen(p) + 1) {
        int vol_disk;
        char letter = (char)toupper((unsigned char)p[0]);

        if (win_volume_disk_index(letter, &vol_disk) != 0 || vol_disk != disk) {
            continue;
        }
        (*attempted)++;
        fprintf(stderr, "  locking and dismounting %c: ...\n", letter);
        if (win_dismount_letter(letter) == 0) {
            fprintf(stderr, "  %c: dismounted\n", letter);
            (*dismounted)++;
        } else {
            fprintf(stderr,
                    "  %c: failed (close Explorer/apps using this drive)\n",
                    letter);
        }
    }

    win_dismount_volumes_findfirst(disk, attempted, dismounted);
    win_dismount_gpt_partitions(raw_path, disk, attempted, dismounted);

    return *attempted > prev_attempted ? 1 : 0;
}

int platform_dismount_volumes_on_device(const char *raw_path)
{
    char drives[512];
    int disk;
    int dismounted = 0;
    int attempted = 0;
    int pass;

    if (win_parse_physical_drive_index(raw_path, &disk) != 0) {
        return 0;
    }

    if (GetLogicalDriveStringsA(sizeof(drives) - 1, drives) == 0) {
        fprintf(stderr, "error: could not list drive letters (Win32 %lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }

    fprintf(stderr, "dismounting volumes on %s (disk %d)...\n", raw_path, disk);
    fflush(stderr);

    /* Lock/dismount before removing letters - removing G: first breaks letter open. */
    for (pass = 0; pass < 3; pass++) {
        if (pass > 0) {
            fprintf(stderr, "  retrying volume dismount (pass %d)...\n", pass + 1);
            Sleep(500);
        }
        (void)win_dismount_pass(raw_path, disk, drives, &attempted, &dismounted);
        if (attempted > 0 && dismounted >= attempted) {
            break;
        }
    }

    win_remove_letters_on_disk(disk);

    if (attempted == 0) {
        fprintf(stderr,
                "  warning: no volumes dismounted (close Explorer and retry)\n");
        return -1;
    }
    if (dismounted < attempted) {
        return -1;
    }
    return 0;
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
    int disk;

    if (raw_path == NULL || state == NULL) {
        return -1;
    }
    platform_disk_detach_init(state);

    if (win_parse_physical_drive_index(raw_path, &disk) != 0) {
        fprintf(stderr, "error: invalid physical disk path %s\n", raw_path);
        return -1;
    }

    if (platform_dismount_volumes_on_device(raw_path) != 0) {
        fprintf(stderr,
                "error: could not dismount all volumes on disk %d\n", disk);
        fprintf(stderr,
                "hint: run as Administrator; close Explorer on this USB\n");
        return -1;
    }

    fprintf(stderr, "taking %s offline for wipe...\n", raw_path);
    fflush(stderr);
    if (platform_set_disk_offline(raw_path, true) == 0) {
        state->disk_offlined = true;
        fprintf(stderr,
                "disk is offline (Windows will not mount its volumes)\n");
    } else {
        unsigned long err = platform_last_io_error();

        if (err == 50UL || err == 1UL) {
            fprintf(stderr,
                    "warning: offline disk not supported on this USB "
                    "(Win32 %lu); continuing after volume dismount\n",
                    err);
        } else {
            fprintf(stderr, "error: could not take disk offline\n");
            platform_print_last_io_error("offline disk");
            return -1;
        }
    }

    return 0;
}

int platform_reattach_disk_after_wipe(const char *raw_path,
                                      const platform_disk_detach_t *state)
{
    if (raw_path == NULL || state == NULL) {
        return -1;
    }

    if (state->disk_offlined) {
        fprintf(stderr, "bringing %s back online...\n", raw_path);
        if (platform_set_disk_offline(raw_path, false) != 0) {
            platform_print_last_io_error("online disk");
            fprintf(stderr,
                    "hint: diskpart -> select disk N -> online disk\n");
            return -1;
        }
        fprintf(stderr, "disk is online again\n");
    }

    return 0;
}

static int win_get_disk_geometry(HANDLE h, device_geometry_t *geo)
{
    DISK_GEOMETRY_EX geom;
    DWORD bytes = 0;

    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                         NULL, 0, &geom, sizeof(geom), &bytes, NULL)) {
        return -1;
    }

    geo->logical_sector_size = geom.Geometry.BytesPerSector;
    geo->physical_sector_size = geom.Geometry.BytesPerSector;
    geo->capacity_bytes = (uint64_t)geom.DiskSize.QuadPart;
    return 0;
}

static int win_get_length(HANDLE h, device_geometry_t *geo)
{
    GET_LENGTH_INFORMATION len;
    DWORD bytes = 0;

    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                         NULL, 0, &len, sizeof(len), &bytes, NULL)) {
        return -1;
    }
    geo->capacity_bytes = (uint64_t)len.Length.QuadPart;
    return 0;
}

static void win_detect_ssd(HANDLE h, device_geometry_t *geo)
{
    STORAGE_PROPERTY_QUERY query;
    BYTE buf[4096];
    DWORD bytes = 0;

    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query), buf, sizeof(buf),
                        &bytes, NULL)) {
        DEVICE_SEEK_PENALTY_DESCRIPTOR *desc =
            (DEVICE_SEEK_PENALTY_DESCRIPTOR *)buf;
        if (desc->Version >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR)) {
            geo->is_ssd = !desc->IncursSeekPenalty;
            geo->is_rotational = desc->IncursSeekPenalty;
            return;
        }
    }
    geo->is_ssd = false;
    geo->is_rotational = true;
}

static void win_detect_removable(HANDLE h, device_geometry_t *geo)
{
    STORAGE_PROPERTY_QUERY query;
    BYTE buf[4096];
    DWORD bytes = 0;
    STORAGE_DEVICE_DESCRIPTOR *desc;

    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query), buf, sizeof(buf),
                         &bytes, NULL)) {
        return;
    }

    desc = (STORAGE_DEVICE_DESCRIPTOR *)buf;
    if (desc->Version < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return;
    }
    if (desc->RemovableMedia) {
        geo->is_removable = true;
    }
    if (desc->BusType == BusTypeUsb) {
        geo->is_removable = true;
    }
}

int platform_get_geometry(platform_device_t *dev, device_geometry_t *geo)
{
    LARGE_INTEGER file_size;

    if (dev == NULL || geo == NULL) {
        return -1;
    }

    memset(geo, 0, sizeof(*geo));
    geo->logical_sector_size = 512;
    geo->physical_sector_size = 512;

    if (dev->kind == PLATFORM_DEV_RAW) {
        if (win_get_disk_geometry(dev->handle, geo) != 0) {
            (void)win_get_length(dev->handle, geo);
        }
        win_detect_ssd(dev->handle, geo);
        win_detect_removable(dev->handle, geo);
    } else {
        if (GetFileSizeEx(dev->handle, &file_size)) {
            geo->capacity_bytes = (uint64_t)file_size.QuadPart;
        }
    }

    dev->sector_size = geo->logical_sector_size;
    if (dev->sector_size == 0 || !ow_is_power_of_two(dev->sector_size)) {
        dev->sector_size = 512;
        geo->logical_sector_size = 512;
    }
    dev->size.QuadPart = (LONGLONG)geo->capacity_bytes;
    return 0;
}

int platform_pread(platform_device_t *dev, void *buf, size_t len, uint64_t offset)
{
    OVERLAPPED ov;
    DWORD read_bytes = 0;

    if (dev == NULL) {
        return -1;
    }

    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    if (!ReadFile(dev->handle, buf, (DWORD)len, NULL, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            return -1;
        }
        if (!GetOverlappedResult(dev->handle, &ov, &read_bytes, TRUE)) {
            return -1;
        }
    } else {
        read_bytes = (DWORD)len;
    }
    return read_bytes == len ? 0 : -1;
}

#ifndef DISK_ATTRIBUTE_OFFLINE
#define DISK_ATTRIBUTE_OFFLINE 0x00000001
#endif

#ifndef IOCTL_DISK_SET_DISK_ATTRIBUTES
#define IOCTL_DISK_SET_DISK_ATTRIBUTES \
    CTL_CODE(IOCTL_DISK_BASE, 13, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

typedef struct _SET_DISK_ATTRIBUTES {
    ULONG Version;
    BOOLEAN Removable;
    BOOLEAN MediaHotplug;
    BOOLEAN DeviceOffline;
    BOOLEAN ReadOnly;
    ULONG Attributes;
    ULONG AttributesMask;
} SET_DISK_ATTRIBUTES;

int platform_set_disk_offline(const char *raw_path, bool offline)
{
    SET_DISK_ATTRIBUTES attrs;
    HANDLE h;
    DWORD bytes = 0;

    if (raw_path == NULL) {
        return -1;
    }

    h = CreateFileA(raw_path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_last_io_error = GetLastError();
        return -1;
    }

    memset(&attrs, 0, sizeof(attrs));
    attrs.Version = sizeof(attrs);
    attrs.DeviceOffline = offline ? TRUE : FALSE;
    attrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    attrs.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;

    if (!DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES,
                         &attrs, sizeof(attrs), NULL, 0, &bytes, NULL)) {
        g_last_io_error = GetLastError();
        CloseHandle(h);
        return -1;
    }
    (void)DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES,
                          NULL, 0, NULL, 0, &bytes, NULL);
    CloseHandle(h);
    return 0;
}

static int win_write_chunk(platform_device_t *dev, const void *buf, DWORD len,
                           uint64_t offset, DWORD *written_out)
{
    OVERLAPPED ov;
    DWORD written = 0;

    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    if (!WriteFile(dev->handle, buf, len, NULL, &ov)) {
        DWORD err = GetLastError();

        if (err != ERROR_IO_PENDING) {
            g_last_io_error = err;
            return -1;
        }
        if (!GetOverlappedResult(dev->handle, &ov, &written, TRUE)) {
            g_last_io_error = GetLastError();
            return -1;
        }
    } else {
        written = len;
    }
    *written_out = written;
    return 0;
}

int platform_pwrite(platform_device_t *dev, const void *buf, size_t len,
                    uint64_t offset)
{
    const uint8_t *cursor = (const uint8_t *)buf;
    size_t remaining = len;
    uint64_t pos = offset;
    uint32_t sector = dev != NULL && dev->sector_size > 0
        ? dev->sector_size : 512;

    if (dev == NULL || buf == NULL || len == 0) {
        return -1;
    }

    g_last_io_error = 0;

    while (remaining > 0) {
        DWORD chunk = remaining > 0x7fffffff ? 0x7fffffff : (DWORD)remaining;
        DWORD written = 0;
        int tries = 8;

        if (dev->direct_io) {
            chunk = (DWORD)ow_align_down((uint64_t)chunk, sector);
            if (chunk == 0) {
                break;
            }
        }

        while (tries-- > 0) {
            if (win_write_chunk(dev, cursor, chunk, pos, &written) != 0) {
                DWORD err = g_last_io_error;

                if (err == ERROR_ACCESS_DENIED || err == ERROR_WRITE_PROTECT) {
                    return -1;
                }
                if (chunk <= sector || !dev->direct_io) {
                    return -1;
                }
                chunk = (DWORD)ow_align_down((uint64_t)(chunk / 2), sector);
                if (chunk == 0) {
                    return -1;
                }
                continue;
            }
            if (written == 0) {
                g_last_io_error = ERROR_PARTIAL_COPY;
                if (chunk <= sector) {
                    return -1;
                }
                chunk = (DWORD)ow_align_down((uint64_t)(chunk / 2), sector);
                continue;
            }
            break;
        }

        if (written == 0) {
            g_last_io_error = ERROR_PARTIAL_COPY;
            return -1;
        }

        cursor += written;
        pos += written;
        remaining -= written;
    }

    return remaining == 0 ? 0 : -1;
}

int platform_flush(platform_device_t *dev)
{
    if (dev == NULL) {
        return -1;
    }
    return FlushFileBuffers(dev->handle) ? 0 : -1;
}

void platform_close(platform_device_t *dev)
{
    if (dev == NULL) {
        return;
    }
    CloseHandle(dev->handle);
    free(dev);
}

int platform_raw_fd(platform_device_t *dev)
{
    (void)dev;
    return -1;
}

#ifndef STORAGE_PROTOCOL_STRUCTURE_VERSION
#define STORAGE_PROTOCOL_STRUCTURE_VERSION 1
#endif

#ifndef STORAGE_PROTOCOL_COMMAND_TYPE_NVME
#define STORAGE_PROTOCOL_COMMAND_TYPE_NVME 1
#endif

#ifndef STORAGE_PROTOCOL_COMMAND_FLAG_NON_DATA
#define STORAGE_PROTOCOL_COMMAND_FLAG_NON_DATA 0x1
#endif

typedef struct _OW_STORAGE_PROTOCOL_COMMAND {
    ULONG   Version;
    ULONG   Length;
    ULONG   ProtocolType;
    ULONG   Flags;
    ULONG   ReturnStatus;
    ULONG   ErrorCode;
    ULONG   CommandLength;
    ULONG   ErrorInfoLength;
    ULONG   DataToDeviceTransferLength;
    ULONG   DataFromDeviceTransferLength;
    ULONG   TimeOutValue;
    ULONG   ErrorInfoOffset;
    ULONG   DataToDeviceBufferOffset;
    ULONG   DataFromDeviceBufferOffset;
    ULONG   CommandSpecific;
    ULONG   Reserved0;
    ULONG   FixedProtocolReturnData;
    ULONG   FixedProtocolReturnData2;
    UCHAR   Command[64];
} OW_STORAGE_PROTOCOL_COMMAND;

#ifndef IOCTL_STORAGE_PROTOCOL_COMMAND
#define IOCTL_STORAGE_PROTOCOL_COMMAND CTL_CODE(0x0000002d, 0x0400, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static int win_nvme_format_nvm(HANDLE h)
{
    OW_STORAGE_PROTOCOL_COMMAND cmd;
    DWORD bytes = 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.Version = STORAGE_PROTOCOL_STRUCTURE_VERSION;
    cmd.Length = sizeof(cmd);
    cmd.ProtocolType = STORAGE_PROTOCOL_COMMAND_TYPE_NVME;
    cmd.Flags = STORAGE_PROTOCOL_COMMAND_FLAG_NON_DATA;
    cmd.TimeOutValue = 60;
    cmd.CommandLength = 64;
    cmd.Command[0] = 0x80; /* Format NVM */
    cmd.Command[32] = 1;   /* NSID = 1 */
    cmd.Command[40] = (UCHAR)(1u << 1); /* SES=1 in CDW10 bits 11:9 approx */

    if (DeviceIoControl(h, IOCTL_STORAGE_PROTOCOL_COMMAND,
                        &cmd, sizeof(cmd), &cmd, sizeof(cmd),
                        &bytes, NULL)) {
        if (cmd.ReturnStatus == 0) {
            return 0;
        }
    }
    return -1;
}

static int win_storage_dsm_trim(HANDLE h, uint64_t offset, uint64_t length)
{
#ifdef _MSC_VER
    STORAGE_MANAGE_DATA_SET_ATTRIBUTES *attrs;
    DEVICE_DATA_SET_RANGE *range;
    DWORD in_size;
    DWORD bytes = 0;
    BOOL ok;

    in_size = (DWORD)(sizeof(STORAGE_MANAGE_DATA_SET_ATTRIBUTES) +
                      sizeof(DEVICE_DATA_SET_RANGE));
    attrs = (STORAGE_MANAGE_DATA_SET_ATTRIBUTES *)calloc(1, in_size);
    if (attrs == NULL) {
        return -1;
    }

    attrs->Size = in_size;
    attrs->Action = DeviceDsmAction_Trim;
    attrs->ParameterBlockOffset = sizeof(STORAGE_MANAGE_DATA_SET_ATTRIBUTES);
    attrs->ParameterBlockLength = 0;
    attrs->DataSetRangesOffset = sizeof(STORAGE_MANAGE_DATA_SET_ATTRIBUTES);
    attrs->DataSetRangesLength = sizeof(DEVICE_DATA_SET_RANGE);

    range = (DEVICE_DATA_SET_RANGE *)((uint8_t *)attrs + attrs->DataSetRangesOffset);
    range->StartingOffset.QuadPart = (LONGLONG)offset;
    range->LengthInBytes.QuadPart = (LONGLONG)length;

    ok = DeviceIoControl(h, IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES,
                         attrs, in_size, NULL, 0, &bytes, NULL);
    free(attrs);
    return ok ? 0 : -1;
#else
    (void)h;
    (void)offset;
    (void)length;
    return -1;
#endif
}

static int win_fsctl_trim(HANDLE h, uint64_t offset, uint64_t length)
{
    struct {
        DWORD Key;
        DWORD NumRanges;
        struct {
            LARGE_INTEGER Offset;
            LARGE_INTEGER Length;
        } Ranges[1];
    } trim;
    DWORD bytes = 0;

    memset(&trim, 0, sizeof(trim));
    trim.NumRanges = 1;
    trim.Ranges[0].Offset.QuadPart = (LONGLONG)offset;
    trim.Ranges[0].Length.QuadPart = (LONGLONG)length;

    if (DeviceIoControl(h, FSCTL_TRIM, &trim, sizeof(trim),
                        NULL, 0, &bytes, NULL)) {
        return 0;
    }
    return -1;
}

int platform_discard_range(platform_device_t *dev, uint64_t offset,
                           uint64_t length)
{
    if (dev == NULL || length == 0) {
        return -1;
    }

    if (dev->kind == PLATFORM_DEV_RAW) {
        if (win_storage_dsm_trim(dev->handle, offset, length) == 0) {
            return 0;
        }
    }
    return win_fsctl_trim(dev->handle, offset, length);
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

int platform_ssd_secure_erase(platform_device_t *dev)
{
    device_geometry_t geo;
    bool is_nvme_path;

    if (dev == NULL || dev->kind != PLATFORM_DEV_RAW) {
        return -1;
    }

    if (platform_get_geometry(dev, &geo) != 0) {
        return -1;
    }

    is_nvme_path = strstr(dev->path, "NVMe") != NULL ||
                   strstr(dev->path, "nvme") != NULL;

    if (is_nvme_path || geo.is_ssd) {
        fprintf(stderr, "attempting NVMe Format NVM via storage pass-through...\n");
        if (win_nvme_format_nvm(dev->handle) == 0) {
            return 0;
        }
        fprintf(stderr, "warning: NVMe format pass-through failed\n");
    }

    fprintf(stderr, "attempting Windows storage secure deallocate...\n");
    if (geo.capacity_bytes > 0 &&
        platform_discard_range(dev, 0, geo.capacity_bytes) == 0) {
        return 0;
    }

    return -1;
}

#endif /* _WIN32 */
