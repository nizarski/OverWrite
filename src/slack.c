#include "slack.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#endif

static fs_type_t linux_detect_fs(const char *path)
{
#ifdef __linux__
    struct statfs st;

    if (statfs(path, &st) != 0) {
        return FS_TYPE_UNKNOWN;
    }

    switch (st.f_type) {
    case 0xEF53: /* ext2/3/4 */
        return FS_TYPE_EXT2;
#ifdef __linux__
    case 0x53464846: /* NTFS on some systems if mounted */
        return FS_TYPE_NTFS;
#endif
    default:
        return FS_TYPE_OTHER;
    }
#else
    (void)path;
    return FS_TYPE_UNKNOWN;
#endif
}

static uint32_t linux_cluster_size(const char *path)
{
#ifdef __linux__
    struct statfs st;

    if (statfs(path, &st) != 0) {
        return 4096;
    }
    if (st.f_frsize > 0) {
        return (uint32_t)st.f_frsize;
    }
    if (st.f_bsize > 0) {
        return (uint32_t)st.f_bsize;
    }
#endif
    (void)path;
    return 4096;
}

int slack_hunter_analyze(const char *path, slack_info_t *info)
{
    struct stat st;

    if (path == NULL || info == NULL) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }

    info->fs_type = linux_detect_fs(path);
    info->cluster_size = linux_cluster_size(path);
    info->logical_size = (uint64_t)st.st_size;
    info->wipe_size = ow_align_up(info->logical_size, info->cluster_size);

    if (info->cluster_size == 0 || !ow_is_power_of_two(info->cluster_size)) {
        info->cluster_size = 4096;
        info->wipe_size = ow_align_up(info->logical_size, 4096);
    }
    return 0;
}

int slack_hunter_extend_file(const char *path, const slack_info_t *info)
{
    if (path == NULL || info == NULL) {
        return -1;
    }

    if (info->wipe_size <= info->logical_size) {
        return 0;
    }

    if (truncate(path, (off_t)info->wipe_size) != 0) {
        return -1;
    }

#ifdef __linux__
    if (info->fs_type == FS_TYPE_EXT2) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
#ifdef FALLOC_FL_KEEP_SIZE
            (void)fallocate(fd, FALLOC_FL_KEEP_SIZE, (off_t)info->logical_size,
                            (off_t)(info->wipe_size - info->logical_size));
#endif
            close(fd);
        }
    }
#endif

    return 0;
}

#else /* _WIN32 */

#include <windows.h>
#include <fileapi.h>
#include <ctype.h>

static fs_type_t win_detect_fs(const char *path)
{
    char root[4] = { 'A', ':', '\\', '\0' };
    char fs_name[64];
    DWORD serial = 0, max_comp = 0, flags = 0;

    if (path == NULL || path[0] == '\0') {
        return FS_TYPE_UNKNOWN;
    }

    if (path[1] == ':') {
        root[0] = (char)toupper((unsigned char)path[0]);
    } else {
        return FS_TYPE_UNKNOWN;
    }

    if (!GetVolumeInformationA(root, NULL, 0, &serial, &max_comp, &flags,
                               fs_name, sizeof(fs_name))) {
        return FS_TYPE_UNKNOWN;
    }

    if (_stricmp(fs_name, "NTFS") == 0) {
        return FS_TYPE_NTFS;
    }
    if (_stricmp(fs_name, "exFAT") == 0 || _stricmp(fs_name, "FAT32") == 0) {
        return FS_TYPE_OTHER;
    }
    return FS_TYPE_OTHER;
}

static uint32_t win_cluster_size(const char *path)
{
    char root[4] = { 'A', ':', '\\', '\0' };
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;

    if (path == NULL || path[1] != ':') {
        return 4096;
    }
    root[0] = (char)toupper((unsigned char)path[0]);

    if (!GetDiskFreeSpaceA(root, &sectors_per_cluster, &bytes_per_sector,
                           &free_clusters, &total_clusters)) {
        return 4096;
    }
    return sectors_per_cluster * bytes_per_sector;
}

int slack_hunter_analyze(const char *path, slack_info_t *info)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER size;

    if (path == NULL || info == NULL) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        return -1;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return -1;
    }

    info->fs_type = win_detect_fs(path);
    info->cluster_size = win_cluster_size(path);
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = fad.nFileSizeHigh;
    info->logical_size = (uint64_t)size.QuadPart;
    info->wipe_size = ow_align_up(info->logical_size, info->cluster_size);

    if (info->cluster_size == 0) {
        info->cluster_size = 4096;
        info->wipe_size = ow_align_up(info->logical_size, 4096);
    }
    return 0;
}

int slack_hunter_extend_file(const char *path, const slack_info_t *info)
{
    HANDLE h;
    LARGE_INTEGER li;

    if (path == NULL || info == NULL) {
        return -1;
    }

    if (info->wipe_size <= info->logical_size) {
        return 0;
    }

    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    li.QuadPart = (LONGLONG)info->wipe_size;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        return -1;
    }
    if (!SetEndOfFile(h)) {
        CloseHandle(h);
        return -1;
    }

    if (info->fs_type == FS_TYPE_NTFS) {
        FlushFileBuffers(h);
    }

    CloseHandle(h);
    return 0;
}

#endif /* _WIN32 */
