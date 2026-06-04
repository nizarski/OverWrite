# Architecture

## Flow

```text
main → target_resolve → safety → wipe_execute → [format_after_wipe] → cleanup
```

**wipe_execute:** dismount/offline → threads or sequential (USB) → rng → pwrite → flush → [trim]

## Modules

| File | Role |
|------|------|
| main.c | CLI |
| target.c | Paths, ranges |
| wipe.c | Threads, passes |
| platform_*.c | I/O, dismount, erase |
| format.c | Post-wipe mkfs/diskpart |
| rng.c, chacha20.c | Random data |
| partition.c | GPT/MBR gaps |
| slack.c | NTFS/ext4 slack |
| free_space.c | Filler file |
| safety.c | Block system disk, confirm |
| progress.c | stderr bar |
| android.c, devlist.c | Phone + --list |

## Threads

- Default: `min(cores, 16)`  
- USB/removable: 1 thread  
- Writes sector-aligned (512/4096)

## Build split

```text
WIN32  → platform_win.c + bcrypt
else   → platform_linux.c + secure_erase_linux.c (Linux)
```
