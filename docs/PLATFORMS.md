# Platforms

One C11 tree. OS code: `platform_linux.c`, `platform_win.c`.

## Paths

| Target | Linux | Windows |
|--------|-------|---------|
| Disk | `/dev/sda`, `/dev/nvme0n1` | `\\.\PhysicalDrive0` |
| Partition | `/dev/sda2` | `\\.\Harddisk0Partition2` |
| Volume | `/dev/sda1` | `\\.\C:` |
| File | any path | any path |
| Free space | mount path | `D:\` |

## Privileges

| Op | Linux | Windows |
|----|-------|---------|
| Raw disk / volume | root | Administrator |
| File | user | user |

## Feature parity

| Feature | Linux | Windows |
|---------|-------|---------|
| Direct I/O | O_DIRECT | NO_BUFFERING |
| Thread pool | yes | yes (USB = 1 thread) |
| Max `-t` | 16 | 16 |
| Slack-hunter | ext4 | NTFS |
| HPA/DCO | ATA probe | note only |
| SSD secure erase | NVMe, ATA, discard | NVMe pass-through, trim |
| Whole-disk prep | umount | dismount + offline |
| `--format-after` | mkfs.* | diskpart / format.com |

## Format after wipe

Disk or partition only. Default FS: `exfat`.

| FS | Linux | Windows |
|----|-------|---------|
| exfat | mkfs.exfat | diskpart / format exFAT |
| ntfs | mkfs.ntfs -F | format NTFS |
| fat32 | mkfs.vfat -F 32 -I | format FAT32 |
| ext4 | mkfs.ext4 -F | - |

Whole disk (Win): diskpart `clean`, GPT, partition, quick format, assign.

## Windows whole disk

1. Close handle  
2. Dismount letters + GPT volume GUIDs  
3. Offline disk (USB: Win32 50 → skip)  
4. Reopen, wipe  
5. Online again  

Admin + close Explorer. Removable = single thread.

## Linux whole disk

`umount` matching partitions from `/proc/mounts`. No offline IOCTL.

## Gaps

- Win HPA/DCO: no ATA probe  
- MinGW: limited NVMe DSM vs MSVC  
- ATA security erase: Linux only  
