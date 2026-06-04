#ifndef OVERWRITE_ANDROID_H
#define OVERWRITE_ANDROID_H

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    ANDROID_MODE_FACTORY,
    ANDROID_MODE_USERDATA,
    ANDROID_MODE_FULL
} android_wipe_mode_t;

typedef struct {
    char serial[128];
    char state[32];
    char product[128];
    char variant[128];
} android_device_t;

int  android_parse_mode(const char *s, android_wipe_mode_t *out);
int  android_tool_available(const char *tool);
int  android_list_devices(FILE *out);
int  android_print_device_info(FILE *out, const char *serial);
int  android_print_wipe_plan(FILE *out, const char *serial,
                             android_wipe_mode_t mode, bool dry_run);
int  android_confirm_wipe(const char *serial, bool skip_confirm);
int  android_wipe(const char *serial, android_wipe_mode_t mode, bool dry_run);
int  android_reboot_bootloader(const char *serial);

#endif
