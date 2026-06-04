# GUI

Python 3 + tkinter wizard. Calls `overwrite` CLI - no separate wipe code.

## Run

```powershell
python gui\overwrite_gui.py      # Windows, Admin for PhysicalDrive
.\scripts\overwrite-gui.ps1
```

```bash
sudo apt install python3-tk      # once
python3 gui/overwrite_gui.py
./scripts/overwrite-gui.sh
```

Needs built binary: `build/overwrite` or `build/overwrite.exe`.  
Env: `OVERWRITE_BIN` = path to binary.

## Steps

| # | Screen |
|---|--------|
| 1 | Binary path, admin badge |
| 2 | Operation, `--list`, target path |
| 3 | Profile, RNG, threads, flags |
| 4 | Command preview, dry-run |
| 5 | Type exact confirm string |
| 6 | Run, progress bar, log |

**Nav:** Back / Next / Run (fixed 960×760). Hover **ⓘ** for help.

## Conditional UI

| Field | When shown |
|-------|------------|
| Android serial | Android ops only |
| Android mode | Android wipe only |
| Format after wipe | Whole disk / partition / file op |

## CLI flags in GUI

Profiles, RNG, nonce, `-t`, `-c`, `--passes`, `--dry-run`, `--force`, `-y`/`--yes`, `-q`, `--ssd-secure-erase`, `--allow-trim`, `--normalize-meta`, `--format-after`, `--list [filter]`, Android commands.

## Limits

- Confirm in GUI step 5; run sends `--yes` to CLI  
- Admin on Windows for `\\.\PhysicalDriveN`  
- `adb` / `fastboot` on PATH for Android  
- Progress needs CLI progress (don't use Quiet)
