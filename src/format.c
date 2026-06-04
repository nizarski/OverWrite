#include "format.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

static int str_eq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int format_parse_fstype(const char *s, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return -1;
    }
    if (s == NULL || s[0] == '\0') {
        strncpy(out, "exfat", out_len - 1);
        out[out_len - 1] = '\0';
        return 0;
    }
    if (str_eq_ci(s, "exfat") || str_eq_ci(s, "ntfs") ||
        str_eq_ci(s, "fat32") || str_eq_ci(s, "ext4")) {
        strncpy(out, s, out_len - 1);
        out[out_len - 1] = '\0';
        for (size_t i = 0; out[i] != '\0'; i++) {
            out[i] = (char)tolower((unsigned char)out[i]);
        }
        return 0;
    }
    return -1;
}

bool format_supported_target(target_kind_t kind)
{
    return kind == TARGET_WHOLE_DISK || kind == TARGET_PARTITION;
}

#ifdef _WIN32

static int win_disk_number(const char *path, unsigned *disk_out)
{
    const char *p;
    unsigned n;

    if (path == NULL) {
        return -1;
    }
    p = strstr(path, "PhysicalDrive");
    if (p == NULL) {
        return -1;
    }
    p += strlen("PhysicalDrive");
    if (sscanf(p, "%u", &n) != 1) {
        return -1;
    }
    if (disk_out != NULL) {
        *disk_out = n;
    }
    return 0;
}

static int win_volume_letter(const char *path, char *letter_out)
{
    const char *p;

    if (path == NULL || letter_out == NULL) {
        return -1;
    }
    p = path;
    if (strncmp(p, "\\\\.\\", 4) == 0) {
        p += 4;
    }
    if (p[0] != '\0' && p[1] == ':' &&
        (p[2] == '\0' || p[2] == '\\')) {
        *letter_out = (char)toupper((unsigned char)p[0]);
        return 0;
    }
    return -1;
}

#endif

bool format_can_apply(const char *path, target_kind_t kind)
{
    if (path == NULL) {
        return false;
    }
    if (format_supported_target(kind)) {
        return true;
    }
#ifdef _WIN32
    {
        char letter;
        if (win_volume_letter(path, &letter) == 0) {
            return true;
        }
        if (win_disk_number(path, NULL) == 0) {
            return true;
        }
    }
#else
    if (platform_path_is_block_device(path)) {
        return true;
    }
#endif
    (void)path;
    return false;
}

static int run_shell(const char *cmd)
{
    int rc;

    if (cmd == NULL || cmd[0] == '\0') {
        return -1;
    }
    fprintf(stderr, "format: %s\n", cmd);
    fflush(stderr);
#ifdef _WIN32
    rc = system(cmd);
#else
    rc = system(cmd);
#endif
    if (rc != 0) {
        fprintf(stderr, "warning: format command failed (exit %d)\n", rc);
        return -1;
    }
    return 0;
}

#ifdef _WIN32

static const char *win_diskpart_fs(const char *fstype)
{
    if (str_eq_ci(fstype, "fat32")) {
        return "fat32";
    }
    if (str_eq_ci(fstype, "ntfs")) {
        return "ntfs";
    }
    return "exfat";
}

static const char *win_format_fs_arg(const char *fstype)
{
    if (str_eq_ci(fstype, "fat32")) {
        return "FAT32";
    }
    if (str_eq_ci(fstype, "ntfs")) {
        return "NTFS";
    }
    if (str_eq_ci(fstype, "ext4")) {
        return "exFAT";
    }
    return "exFAT";
}

static int win_format_physical_disk(unsigned disk, const char *fstype)
{
    char script_path[MAX_PATH];
    char cmd[512];
    FILE *f;
    const char *dpfs;
    int rc;

    dpfs = win_diskpart_fs(fstype);
    if (GetTempPathA(sizeof(script_path), script_path) == 0) {
        return -1;
    }
    if ((size_t)snprintf(script_path + strlen(script_path),
                         sizeof(script_path) - strlen(script_path),
                         "overwrite_diskpart_%u.txt", disk) >=
        sizeof(script_path) - strlen(script_path)) {
        return -1;
    }

    f = fopen(script_path, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "select disk %u\n", disk);
    fprintf(f, "online disk\n");
    fprintf(f, "attributes disk clear readonly\n");
    fprintf(f, "clean\n");
    fprintf(f, "convert gpt\n");
    fprintf(f, "create partition primary\n");
    fprintf(f, "format fs=%s quick label=OverWrite\n", dpfs);
    fprintf(f, "assign\n");
    fprintf(f, "exit\n");
    fclose(f);

    snprintf(cmd, sizeof(cmd), "diskpart /s \"%s\"", script_path);
    rc = run_shell(cmd);
    remove(script_path);
    return rc;
}

static int win_format_volume(char letter, const char *fstype)
{
    char cmd[256];
    const char *fsarg = win_format_fs_arg(fstype);

    snprintf(cmd, sizeof(cmd),
             "format %c: /FS:%s /Q /Y /V:OverWrite",
             letter, fsarg);
    return run_shell(cmd);
}

int format_after_wipe(const char *path, target_kind_t kind, const char *fstype)
{
    unsigned disk;
    char letter;

    if (path == NULL || fstype == NULL || !format_can_apply(path, kind)) {
        return -1;
    }

    fprintf(stderr, "\nformatting target as %s after wipe...\n", fstype);

    if (win_disk_number(path, &disk) == 0) {
        return win_format_physical_disk(disk, fstype);
    }

    if (win_volume_letter(path, &letter) == 0) {
        return win_format_volume(letter, fstype);
    }

    fprintf(stderr,
            "warning: format not supported for path %s on Windows\n", path);
    return -1;
}

#else /* Linux */

static int linux_mkfs_cmd(const char *fstype, const char *dev, char *cmd, size_t cmd_len)
{
    if (str_eq_ci(fstype, "ext4")) {
        return snprintf(cmd, cmd_len, "mkfs.ext4 -F -L OverWrite '%s'", dev);
    }
    if (str_eq_ci(fstype, "ntfs")) {
        return snprintf(cmd, cmd_len, "mkfs.ntfs -F -L OverWrite '%s'", dev);
    }
    if (str_eq_ci(fstype, "fat32")) {
        return snprintf(cmd, cmd_len, "mkfs.vfat -F 32 -I -n OverWrite '%s'", dev);
    }
    if (str_eq_ci(fstype, "exfat")) {
        return snprintf(cmd, cmd_len, "mkfs.exfat -n OverWrite '%s'", dev);
    }
    return -1;
}

int format_after_wipe(const char *path, target_kind_t kind, const char *fstype)
{
    char cmd[512];
    int n;

    if (path == NULL || fstype == NULL || !format_can_apply(path, kind)) {
        return -1;
    }

    fprintf(stderr, "\nformatting target as %s after wipe...\n", fstype);

    n = linux_mkfs_cmd(fstype, path, cmd, sizeof(cmd));
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        fprintf(stderr, "unknown filesystem: %s\n", fstype);
        return -1;
    }

    if (run_shell(cmd) != 0) {
        if (str_eq_ci(fstype, "exfat")) {
            fprintf(stderr, "note: install exfatprogs for exfat (mkfs.exfat)\n");
        }
        return -1;
    }
    fprintf(stderr, "format completed successfully\n");
    return 0;
}

#endif
