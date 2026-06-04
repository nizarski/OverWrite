#ifndef OVERWRITE_HIDDEN_AREA_H
#define OVERWRITE_HIDDEN_AREA_H

#include "platform.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool probed;
    bool hpa_detected;
    bool dco_detected;
    uint64_t visible_bytes;
    uint64_t native_bytes;
    uint64_t hidden_bytes;
    char detail[256];
} hidden_area_info_t;

int  hidden_area_probe(platform_device_t *dev, hidden_area_info_t *info);
void hidden_area_print_warnings(const hidden_area_info_t *info);

#endif
