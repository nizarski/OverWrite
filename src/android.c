#include "android.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#define OW_POPEN _popen
#define OW_PCLOSE _pclose
#else
#include <unistd.h>
#define OW_POPEN popen
#define OW_PCLOSE pclose
#endif

#define ANDROID_CMD_MAX 4096
#define ANDROID_LINE_MAX 512

static int run_command(const char *cmd, char *output, size_t output_len,
                       int *exit_code)
{
    FILE *fp;
    size_t total = 0;

    if (cmd == NULL) {
        return -1;
    }

    fp = OW_POPEN(cmd, "r");
    if (fp == NULL) {
        return -1;
    }

    if (output != NULL && output_len > 0) {
        output[0] = '\0';
        while (total + 1 < output_len) {
            size_t n;
            if (fgets(output + total, (int)(output_len - total), fp) == NULL) {
                break;
            }
            n = strlen(output + total);
            total += n;
        }
    } else {
        char discard[256];
        while (fgets(discard, sizeof(discard), fp) != NULL) {
            /* drain */
        }
    }

    if (exit_code != NULL) {
#ifdef _WIN32
        *exit_code = _pclose(fp);
        if (*exit_code == -1) {
            return -1;
        }
#else
        *exit_code = pclose(fp);
        if (*exit_code == -1) {
            return -1;
        }
        *exit_code = WIFEXITED(*exit_code) ? WEXITSTATUS(*exit_code) : -1;
#endif
    } else {
        OW_PCLOSE(fp);
    }
    return 0;
}

static void append_serial_arg(char *cmd, size_t cmd_len, const char *dev_serial)
{
    if (dev_serial != NULL && dev_serial[0] != '\0' &&
        strcmp(dev_serial, "*") != 0) {
        char tmp[ANDROID_CMD_MAX];
        snprintf(tmp, sizeof(tmp), " -s \"%s\"", dev_serial);
        strncat(cmd, tmp, cmd_len - strlen(cmd) - 1);
    }
}

static int fastboot_cmd(const char *dev_serial, const char *args, char *out,
                        size_t out_len)
{
    char cmd[ANDROID_CMD_MAX];
    int exit_code = -1;

    snprintf(cmd, sizeof(cmd), "fastboot");
    append_serial_arg(cmd, sizeof(cmd), dev_serial);
    if (args != NULL && args[0] != '\0') {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, args, sizeof(cmd) - strlen(cmd) - 1);
    }

    return run_command(cmd, out, out_len, &exit_code) == 0 ? exit_code : -1;
}

static int adb_cmd(const char *dev_serial, const char *args, char *out,
                   size_t out_len)
{
    char cmd[ANDROID_CMD_MAX];
    int exit_code = -1;

    snprintf(cmd, sizeof(cmd), "adb");
    append_serial_arg(cmd, sizeof(cmd), dev_serial);
    if (args != NULL && args[0] != '\0') {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, args, sizeof(cmd) - strlen(cmd) - 1);
    }

    return run_command(cmd, out, out_len, &exit_code) == 0 ? exit_code : -1;
}

int android_tool_available(const char *tool)
{
    char cmd[ANDROID_CMD_MAX];
    char out[ANDROID_LINE_MAX];
    int exit_code = -1;

#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "where %s 2>nul", tool);
    if (run_command(cmd, out, sizeof(out), &exit_code) != 0) {
        return 0;
    }
    return out[0] != '\0';
#else
    snprintf(cmd, sizeof(cmd), "which %s 2>/dev/null", tool);
    if (run_command(cmd, out, sizeof(out), &exit_code) != 0) {
        return 0;
    }
    return exit_code == 0;
#endif
}

int android_parse_mode(const char *s, android_wipe_mode_t *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    if (strcmp(s, "factory") == 0) {
        *out = ANDROID_MODE_FACTORY;
    } else if (strcmp(s, "userdata") == 0) {
        *out = ANDROID_MODE_USERDATA;
    } else if (strcmp(s, "full") == 0) {
        *out = ANDROID_MODE_FULL;
    } else {
        return -1;
    }
    return 0;
}

int android_list_devices(FILE *out)
{
    char buf[4096];
    int exit_code;

    fprintf(out, "\nAndroid devices\n");
    fprintf(out, "Install platform-tools (fastboot/adb) and add to PATH.\n");
    fprintf(out, "Phone must be in fastboot mode for wipe (adb can reboot it).\n\n");

    if (!android_tool_available("fastboot")) {
        fprintf(out, "fastboot: not found on PATH\n");
    } else {
        fprintf(out, "Fastboot mode:\n");
        fprintf(out, "%-20s %-12s\n", "SERIAL", "STATE");
        fprintf(out, "%-20s %-12s\n", "------", "-----");
        if (run_command("fastboot devices", buf, sizeof(buf), &exit_code) == 0) {
            char *line = buf;
            while (line != NULL && *line != '\0') {
                char serial[128];
                char state[64];
                if (sscanf(line, "%127s %63s", serial, state) >= 1) {
                    fprintf(out, "%-20s %-12s\n", serial,
                            state[0] != '\0' ? state : "fastboot");
                }
                line = strchr(line, '\n');
                if (line != NULL) {
                    line++;
                }
            }
        }
        fprintf(out, "\n");
    }

    if (!android_tool_available("adb")) {
        fprintf(out, "adb: not found on PATH\n");
    } else {
        fprintf(out, "ADB mode (reboot to bootloader before wipe):\n");
        fprintf(out, "%-20s %-12s\n", "SERIAL", "STATE");
        fprintf(out, "%-20s %-12s\n", "------", "-----");
        if (run_command("adb devices", buf, sizeof(buf), &exit_code) == 0) {
            char *line = buf;
            while (line != NULL && *line != '\0') {
                char serial[128];
                char state[64];
                if (strstr(line, "List of devices") != NULL ||
                    strncmp(line, "*", 1) == 0) {
                    line = strchr(line, '\n');
                    if (line != NULL) {
                        line++;
                    }
                    continue;
                }
                if (sscanf(line, "%127s %63s", serial, state) >= 2) {
                    fprintf(out, "%-20s %-12s\n", serial, state);
                }
                line = strchr(line, '\n');
                if (line != NULL) {
                    line++;
                }
            }
        }
        fprintf(out, "\n");
    }

    fprintf(out, "Wipe:  overwrite --android-wipe [serial] [--android-mode factory]\n");
    fprintf(out, "       adb reboot bootloader   (from Android with USB debugging)\n");
    return 0;
}

static void print_getvar(FILE *out, const char *serial, const char *var)
{
    char buf[512];
    int rc = fastboot_cmd(serial, var, buf, sizeof(buf));

    if (rc == 0 && buf[0] != '\0') {
        fprintf(out, "  %s: %s", var + 8, buf);
    }
}

int android_print_device_info(FILE *out, const char *serial)
{
    if (!android_tool_available("fastboot")) {
        fprintf(stderr, "error: fastboot not found on PATH\n");
        fprintf(stderr, "install Android platform-tools:\n");
        fprintf(stderr, "  https://developer.android.com/studio/releases/platform-tools\n");
        return -1;
    }

    fprintf(out, "\nFastboot device: %s\n",
            serial != NULL && serial[0] != '\0' ? serial : "(first device)");
    print_getvar(out, serial, "getvar product");
    print_getvar(out, serial, "getvar variant");
    print_getvar(out, serial, "getvar version-bootloader");
    print_getvar(out, serial, "getvar secure");
    print_getvar(out, serial, "getvar unlocked");
    print_getvar(out, serial, "getvar current-slot");
    fprintf(out, "\n");
    return 0;
}

int android_print_wipe_plan(FILE *out, const char *serial,
                             android_wipe_mode_t mode, bool dry_run)
{
    const char *mode_name;

    switch (mode) {
    case ANDROID_MODE_FACTORY: mode_name = "factory (-w userdata+cache)"; break;
    case ANDROID_MODE_USERDATA: mode_name = "userdata only"; break;
    case ANDROID_MODE_FULL: mode_name = "full (userdata+cache+metadata)"; break;
    default: mode_name = "unknown"; break;
    }

    fprintf(out, "\n=== Android wipe plan ===\n");
    fprintf(out, "Serial:     %s\n",
            serial != NULL && serial[0] != '\0' ? serial : "(auto)");
    fprintf(out, "Mode:       %s\n", mode_name);
    fprintf(out, "Dry run:    %s\n", dry_run ? "yes" : "no");
    fprintf(out, "\nRequirements:\n");
    fprintf(out, "  - Phone in FASTBOOT mode (Bootloader)\n");
    fprintf(out, "  - USB debugging / OEM unlock per device policy\n");
    fprintf(out, "  - Bootloader unlocked on many devices\n");
    fprintf(out, "  - fastboot.exe on PATH\n");
    fprintf(out, "\nCommands to run:\n");

    if (mode == ANDROID_MODE_FACTORY) {
        fprintf(out, "  fastboot -w\n");
    } else if (mode == ANDROID_MODE_USERDATA) {
        fprintf(out, "  fastboot erase userdata\n");
        fprintf(out, "  fastboot format userdata\n");
        fprintf(out, "  (fallback: fastboot format:userdata)\n");
    } else {
        fprintf(out, "  fastboot erase userdata && format userdata\n");
        fprintf(out, "  fastboot erase cache && format cache\n");
        fprintf(out, "  fastboot erase metadata (if present)\n");
    }

    fprintf(out, "\nLimits:\n");
    fprintf(out, "  - Not a hardware secure erase; eMMC/UFS may retain remapped data\n");
    fprintf(out, "  - FRP / persistent partitions may survive on some OEMs\n");
    fprintf(out, "  - Samsung/Odin-only devices may not support standard fastboot\n");
    fprintf(out, "========================\n\n");

    android_print_device_info(out, serial);
    return 0;
}

int android_confirm_wipe(const char *serial, bool skip_confirm)
{
    char line[256];
    char expect[256];
    size_t len;

    if (skip_confirm) {
        return 0;
    }

    if (serial == NULL || serial[0] == '\0') {
        snprintf(expect, sizeof(expect), "WIPE ANDROID");
    } else {
        snprintf(expect, sizeof(expect), "WIPE ANDROID %s", serial);
    }

    fprintf(stderr, "Type exactly: %s\n> ", expect);
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return -1;
    }
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (strcmp(line, expect) != 0) {
        fprintf(stderr, "confirmation failed\n");
        return -1;
    }
    return 0;
}

static int try_fastboot_erase_format(const char *serial, const char *partition)
{
    char args[256];
    char buf[512];
    int rc;

    snprintf(args, sizeof(args), "erase %s", partition);
    rc = fastboot_cmd(serial, args, buf, sizeof(buf));
    if (rc != 0) {
        snprintf(args, sizeof(args), "erase:%s", partition);
        (void)fastboot_cmd(serial, args, buf, sizeof(buf));
    }

    snprintf(args, sizeof(args), "format %s", partition);
    rc = fastboot_cmd(serial, args, buf, sizeof(buf));
    if (rc != 0) {
        snprintf(args, sizeof(args), "format:%s", partition);
        rc = fastboot_cmd(serial, args, buf, sizeof(buf));
    }
    return rc;
}

int android_wipe(const char *serial, android_wipe_mode_t mode, bool dry_run)
{
    char buf[512];
    int rc;

    if (!android_tool_available("fastboot")) {
        fprintf(stderr, "error: fastboot not found on PATH\n");
        return -1;
    }

    if (dry_run) {
        return 0;
    }

    fprintf(stderr, "starting Android wipe via fastboot...\n");

    if (mode == ANDROID_MODE_FACTORY) {
        rc = fastboot_cmd(serial, "-w", buf, sizeof(buf));
        if (rc == 0) {
            fprintf(stderr, "fastboot -w completed\n");
            return 0;
        }
        fprintf(stderr, "warning: fastboot -w failed, trying manual erase...\n");
        mode = ANDROID_MODE_FULL;
    }

    if (mode == ANDROID_MODE_USERDATA) {
        rc = try_fastboot_erase_format(serial, "userdata");
        fprintf(stderr, rc == 0 ? "userdata wiped\n" : "userdata wipe failed\n");
        return rc == 0 ? 0 : -1;
    }

    rc = try_fastboot_erase_format(serial, "userdata");
    if (rc != 0) {
        fprintf(stderr, "error: userdata wipe failed\n");
        return -1;
    }
    fprintf(stderr, "userdata wiped\n");

    if (try_fastboot_erase_format(serial, "cache") == 0) {
        fprintf(stderr, "cache wiped\n");
    }

    (void)try_fastboot_erase_format(serial, "metadata");

    fprintf(stderr, "Android wipe finished. Reboot: fastboot reboot\n");
    return 0;
}

int android_reboot_bootloader(const char *serial)
{
    char buf[512];
    int rc;

    if (!android_tool_available("adb")) {
        fprintf(stderr, "error: adb not found on PATH\n");
        return -1;
    }

    rc = adb_cmd(serial, "reboot bootloader", buf, sizeof(buf));
    return rc == 0 ? 0 : -1;
}
