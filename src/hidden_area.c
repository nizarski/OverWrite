#include "hidden_area.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/fs.h>

#ifndef HDIO_DRIVE_CMD
#define HDIO_DRIVE_CMD 0x031f
#endif

static int ata_cmd_inout(int fd, uint8_t cmd, void *data, size_t data_len,
                         bool from_dev)
{
    unsigned char *buf;
    size_t total;
    int rc;

    total = 4 + data_len;
    buf = (unsigned char *)calloc(1, total);
    if (buf == NULL) {
        return -1;
    }

    buf[0] = cmd;
    if (data_len > 0) {
        buf[3] = 1;
    }

    rc = ioctl(fd, HDIO_DRIVE_CMD, buf);
    if (rc == 0 && from_dev && data != NULL && data_len > 0) {
        memcpy(data, buf + 4, data_len);
    }

    free(buf);
    return rc == 0 ? 0 : -1;
}

static int ata_read_native_max_sectors(int fd, uint64_t *native_sectors)
{
    unsigned char out[512];
    uint64_t lba;

    if (ata_cmd_inout(fd, 0x27, out, sizeof(out), true) != 0) {
        return -1;
    }

    lba = ((uint64_t)out[8] << 40) | ((uint64_t)out[10] << 32) |
          ((uint64_t)out[12] << 24) | ((uint64_t)out[14] << 16) |
          ((uint64_t)out[16] << 8)  | (uint64_t)out[18];
    *native_sectors = lba + 1;
    return 0;
}

static int ata_dco_identify_active(int fd, bool *dco_active)
{
    unsigned char out[512];

    *dco_active = false;
    if (ata_cmd_inout(fd, 0xB1, out, sizeof(out), true) != 0) {
        return -1;
    }

    if (out[8] & 0x10) {
        *dco_active = true;
    }
    return 0;
}
#endif /* __linux__ */
#endif /* !_WIN32 */

int hidden_area_probe(platform_device_t *dev, hidden_area_info_t *info)
{
    device_geometry_t geo;
    int fd;

    if (dev == NULL || info == NULL) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    if (platform_get_geometry(dev, &geo) != 0) {
        return -1;
    }

    info->probed = true;
    info->visible_bytes = geo.capacity_bytes;
    info->native_bytes = geo.capacity_bytes;

    fd = platform_raw_fd(dev);
    if (fd < 0) {
        return 0;
    }

#ifndef _WIN32
#ifdef __linux__
    if (geo.logical_sector_size > 0) {
        uint64_t visible_sectors = geo.capacity_bytes / geo.logical_sector_size;
        uint64_t native_sectors = 0;

        if (ata_read_native_max_sectors(fd, &native_sectors) == 0 &&
            native_sectors > visible_sectors) {
            info->hpa_detected = true;
            info->native_bytes = native_sectors * geo.logical_sector_size;
            info->hidden_bytes = info->native_bytes - info->visible_bytes;
            snprintf(info->detail, sizeof(info->detail),
                     "HPA: native %llu sectors > visible %llu sectors",
                     (unsigned long long)native_sectors,
                     (unsigned long long)visible_sectors);
        } else if (native_sectors > 0) {
            info->native_bytes = native_sectors * geo.logical_sector_size;
        }

        {
            bool dco = false;
            if (ata_dco_identify_active(fd, &dco) == 0 && dco) {
                info->dco_detected = true;
                if (info->detail[0] != '\0') {
                    strncat(info->detail, "; ", sizeof(info->detail) -
                            strlen(info->detail) - 1);
                }
                strncat(info->detail, "DCO configuration overlay may be active",
                        sizeof(info->detail) - strlen(info->detail) - 1);
            }
        }
    }
#endif
#else
    if (geo.capacity_bytes > 0) {
        snprintf(info->detail, sizeof(info->detail),
                 "HPA/DCO deep detection limited on Windows; verify capacity "
                 "with disk management tools");
    }
    (void)fd;
#endif

    return 0;
}

void hidden_area_print_warnings(const hidden_area_info_t *info)
{
    if (info == NULL || !info->probed) {
        return;
    }

    if (info->hpa_detected) {
        fprintf(stderr,
                "warning: HPA detected - %llu bytes may be hidden from the OS\n",
                (unsigned long long)info->hidden_bytes);
        fprintf(stderr,
                "         wipe covers visible area only; restore native "
                "capacity with host tools first\n");
    }

    if (info->dco_detected) {
        fprintf(stderr,
                "warning: DCO may be active - drive capacity could be "
                "modified below factory limits\n");
    }

    if (info->detail[0] != '\0' &&
        !info->hpa_detected && !info->dco_detected) {
        fprintf(stderr, "note: %s\n", info->detail);
    }
}
