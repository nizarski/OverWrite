#ifndef OVERWRITE_SECURE_ERASE_LINUX_H
#define OVERWRITE_SECURE_ERASE_LINUX_H

int linux_nvme_format_nvm(int fd);
int linux_ata_security_erase(int fd);
int linux_blk_secure_discard(int fd, uint64_t length);

#endif
