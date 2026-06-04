#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __linux__

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>

#ifndef HDIO_DRIVE_CMD
#define HDIO_DRIVE_CMD 0x031f
#endif

int linux_nvme_format_nvm(int fd)
{
    struct nvme_admin_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x80;
    cmd.nsid = 1;
    cmd.cdw10 = (1u << 9);

    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0 ? 0 : -1;
}

static int ata_security_erase_prepare(int fd)
{
    unsigned char args[4];

    memset(args, 0, sizeof(args));
    args[0] = 0xF3;
    return ioctl(fd, HDIO_DRIVE_CMD, args) == 0 ? 0 : -1;
}

static int ata_security_erase_unit(int fd)
{
    unsigned char args[4 + 512];
    int rc;

    memset(args, 0, sizeof(args));
    args[0] = 0xF4;
    args[1] = 0x00;

    rc = ioctl(fd, HDIO_DRIVE_CMD, args);
    if (rc != 0) {
        return -1;
    }

    sleep(2);
    return 0;
}

int linux_ata_security_erase(int fd)
{
    fprintf(stderr, "attempting ATA SECURITY ERASE UNIT...\n");

    if (ata_security_erase_prepare(fd) != 0) {
        fprintf(stderr, "warning: ATA security erase prepare failed\n");
        return -1;
    }

    sleep(1);

    if (ata_security_erase_unit(fd) != 0) {
        fprintf(stderr, "warning: ATA security erase unit failed\n");
        return -1;
    }

    fprintf(stderr, "ATA security erase command issued (drive may still be busy)\n");
    return 0;
}

int linux_blk_secure_discard(int fd, uint64_t length)
{
    uint64_t range[2] = { 0, length };

#ifdef BLKSECDISCARD
    if (ioctl(fd, BLKSECDISCARD, range) == 0) {
        return 0;
    }
#endif
    if (ioctl(fd, BLKDISCARD, range) == 0) {
        return 0;
    }
    return -1;
}

#endif /* __linux__ */
