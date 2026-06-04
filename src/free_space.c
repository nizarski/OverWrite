#include "free_space.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifndef _WIN32
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#else
#include <windows.h>
#endif

static int path_parent(const char *path, char *dir, size_t dir_len)
{
    char *slash;

    if (path == NULL || dir == NULL) {
        return -1;
    }

    strncpy(dir, path, dir_len - 1);
    dir[dir_len - 1] = '\0';

    slash = strrchr(dir, '/');
    if (slash == NULL) {
        slash = strrchr(dir, '\\');
    }

    if (slash == NULL) {
        strncpy(dir, ".", dir_len - 1);
    } else if (slash == dir) {
        dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    return 0;
}

int free_space_bytes(const char *mount_path, uint64_t *available)
{
    if (mount_path == NULL || available == NULL) {
        return -1;
    }

#ifndef _WIN32
    struct statvfs st;
    if (statvfs(mount_path, &st) != 0) {
        return -1;
    }
    *available = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
#else
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExA(mount_path, &free_bytes, NULL, NULL)) {
        return -1;
    }
    *available = (uint64_t)free_bytes.QuadPart;
#endif
    return 0;
}

int free_space_create_filler(const char *mount_path, char *filler_out,
                             size_t filler_out_len, uint64_t *size_out)
{
    char path[4096];
    FILE *f;
    uint64_t avail;
    uint64_t reserve = 4ULL * 1024ULL * 1024ULL;
    unsigned char buf[65536];
    size_t n;

    if (mount_path == NULL || filler_out == NULL || size_out == NULL) {
        return -1;
    }

    if (free_space_bytes(mount_path, &avail) != 0) {
        return -1;
    }
    if (avail <= reserve) {
        return -1;
    }
    avail -= reserve;

    snprintf(path, sizeof(path), "%s/.overwrite_filler_%lu.tmp",
             mount_path, (unsigned long)time(NULL));
    if ((size_t)snprintf(filler_out, filler_out_len, "%s", path) >= filler_out_len) {
        return -1;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }

    memset(buf, 0, sizeof(buf));
    for (uint64_t written = 0; written < avail; ) {
        n = sizeof(buf);
        if (avail - written < n) {
            n = (size_t)(avail - written);
        }
        if (fwrite(buf, 1, n, f) != n) {
            fclose(f);
            remove(path);
            return -1;
        }
        written += n;
    }
    fclose(f);
    *size_out = avail;
    return 0;
}

int fs_shadow_random_rename(const char *path, char *newpath, size_t newpath_len)
{
    char dir[4096];
    unsigned char rnd[8];
    char hex[17];
    size_t i;

    if (path == NULL || newpath == NULL) {
        return -1;
    }

    if (path_parent(path, dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (platform_os_random(rnd, sizeof(rnd)) != 0) {
        return -1;
    }

    for (i = 0; i < sizeof(rnd); i++) {
        snprintf(hex + i * 2, 3, "%02x", rnd[i]);
    }

    if ((size_t)snprintf(newpath, newpath_len, "%s/.ow_%s", dir, hex) >= newpath_len) {
        return -1;
    }

#ifndef _WIN32
    if (rename(path, newpath) != 0) {
        return -1;
    }
#else
    if (!MoveFileExA(path, newpath, MOVEFILE_REPLACE_EXISTING)) {
        return -1;
    }
#endif
    return 0;
}

int fs_shadow_normalize_timestamps(const char *path)
{
    if (path == NULL) {
        return -1;
    }

#ifndef _WIN32
    struct timeval tv[2];

    tv[0].tv_sec = 1;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = 1;
    tv[1].tv_usec = 0;
    if (utimes(path, tv) != 0) {
        return -1;
    }
#else
    HANDLE h;
    FILETIME ft;

    h = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    memset(&ft, 0, sizeof(ft));
    if (!SetFileTime(h, &ft, &ft, &ft)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
#endif
    return 0;
}

int fs_shadow_secure_delete(const char *path, bool normalize_meta,
                            bool random_rename)
{
    char renamed[4096];
    const char *final_path = path;

    if (path == NULL) {
        return -1;
    }

    if (random_rename) {
        if (fs_shadow_random_rename(path, renamed, sizeof(renamed)) != 0) {
            return -1;
        }
        final_path = renamed;
    }

    if (normalize_meta) {
        (void)fs_shadow_normalize_timestamps(final_path);
    }

#ifndef _WIN32
    return unlink(final_path) == 0 ? 0 : -1;
#else
    return DeleteFileA(final_path) ? 0 : -1;
#endif
}

int free_space_remove_filler(const char *filler_path, bool normalize_meta)
{
    return fs_shadow_secure_delete(filler_path, normalize_meta, true);
}
