# OverWrite

Secure data wipe for disks, volumes, and files. Linux + Windows. **root** or **Administrator** for raw targets. Authorized systems only.

**v1.0.0** · nizarski · [CHANGELOG](CHANGELOG.md)

## Features

whole-disk / volume / file · free-space · unallocated gaps · profiles + RNG · dry-run · `--list` · SSD secure erase · `--format-after` · Android fastboot · Python GUI wizard · USB single-thread

## Docs

| | |
|--|--|
| [docs/README.md](docs/README.md) | Index |
| [docs/PLATFORMS.md](docs/PLATFORMS.md) | Paths, privileges, format-after |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules, data flow |
| [docs/PROFILES.md](docs/PROFILES.md) | Profiles, RNG, limits |
| [docs/GUI.md](docs/GUI.md) | **GUI wizard** |
| [docs/ANDROID.md](docs/ANDROID.md) | adb / fastboot wipe |

## Requirements

- Linux: glibc/musl, CMake ≥ 3.14, GCC or Clang (C11)
- Windows 10 / 11 / Server 2019+
- **Python 3.8+ + tkinter** - GUI only; see [docs/GUI.md](docs/GUI.md)
- **platform-tools** on PATH - Android wipe only

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j    # Linux
```

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release                                  # Windows
```

Output: `build/overwrite`, `build/overwrite.exe`

## Run

GUI: `python gui\overwrite_gui.py` (Admin on Windows for physical disks)  
CLI: `build/overwrite` or `build/overwrite.exe`  
Env: `OVERWRITE_BIN` if binary not in `build/`

```bash
./build/overwrite --list
sudo ./build/overwrite --dry-run /dev/sdb
sudo ./build/overwrite --profile ghost /dev/sdb
```

```powershell
.\build\overwrite.exe --list
.\build\overwrite.exe --dry-run \\.\PhysicalDrive2
.\build\overwrite.exe --profile ghost \\.\PhysicalDrive2
.\build\overwrite.exe --format-after \\.\PhysicalDrive2
```

GUI steps: Welcome · Target · Options · Review · Confirm · Execute

## Safety

Does not skip confirmation unless `--yes` or GUI after confirm. Does not wipe without `--force` on blocked system disks. Does not guarantee SSD remapped cells or hidden HPA are cleared. Windows: dismounts volumes before whole-disk wipe; USB may not go offline (Win32 50) - close Explorer.

## License

[LICENSE](LICENSE) · [CONTRIBUTING](CONTRIBUTING.md) · [SECURITY](SECURITY.md)
