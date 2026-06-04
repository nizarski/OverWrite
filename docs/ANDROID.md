# Android

Uses **fastboot** / **adb** - not USB mass storage.

## Need

- [platform-tools](https://developer.android.com/studio/releases/platform-tools) on PATH  
- Phone in **fastboot** for wipe  
- Unlocked bootloader on many models  

## Steps

```bash
overwrite --android-list
overwrite --android-reboot-bootloader [serial]   # or manual fastboot
overwrite --android-wipe [serial] --dry-run
overwrite --android-wipe [serial] --android-mode factory
# confirm: WIPE ANDROID [serial]
```

## Flags

| Flag | Action |
|------|--------|
| `--android-list` | List devices |
| `--android-wipe [serial]` | fastboot erase/format |
| `--android-mode factory` | `fastboot -w` (default) |
| `--android-mode userdata` | userdata only |
| `--android-mode full` | userdata + cache + metadata |
| `--android-reboot-bootloader` | adb reboot bootloader |

## Limits

- Not chip-level secure erase  
- FRP/vendor partitions may remain  
- Locked bootloader → may fail  
- Dynamic partitions: `format:` / `erase:` fallbacks  

**Linux:** `apt install android-sdk-platform-tools`  
**Windows:** add platform-tools to PATH + USB driver
