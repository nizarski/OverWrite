#!/usr/bin/env python3
"""
OverWrite GUI - cross-platform wizard (Windows & Linux).
Requires Python 3.8+ (tkinter). Invokes the overwrite CLI binary.
"""

from __future__ import annotations

import os
import re
import sys
import shutil
import subprocess
import threading
import queue
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
except ImportError as exc:
    print("OverWrite GUI requires Python 3 with tkinter.", file=sys.stderr)
    raise SystemExit(1) from exc


VERSION = "1.1.1"
WIN_W, WIN_H = 960, 760

# ── Theme ────────────────────────────────────────────────────────────────────
class Theme:
    BG = "#eef1f8"
    CARD = "#ffffff"
    HEADER = "#0f2744"
    HEADER2 = "#163a5f"
    ACCENT = "#00a896"
    ACCENT2 = "#028090"
    TEXT = "#1e293b"
    MUTED = "#64748b"
    BORDER = "#cbd5e1"
    DANGER = "#dc2626"
    WARN = "#d97706"
    OK = "#059669"
    STEP_IDLE = "#334155"
    STEP_DONE = "#00a896"
    STEP_ACTIVE = "#38bdf8"
    LOG_BG = "#0f172a"
    LOG_FG = "#e2e8f0"
    HELP = "#6366f1"


PROFILES = [
    "ghost",
    "chameleon",
    "spectrum",
    "flash-realist",
    "filesystem-shadow",
    "block-cartographer",
    "slack-hunter",
]
RNG_MODES = ["vault", "turbo", "hybrid", "os-chunk"]
ANDROID_MODES = ["factory", "userdata", "full"]
LIST_FILTERS = ["all", "disks", "partitions", "volumes"]
FORMAT_FS = ["exfat", "ntfs", "fat32", "ext4"]
ANDROID_OPS = frozenset({"android", "android_list", "android_reboot"})

OPERATIONS: List[Tuple[str, str, str]] = [
    ("whole", "💾  Whole disk / partition / file", "Wipe a physical drive, volume, or single file."),
    ("free_space", "📂  Free space on volume", "Fill unused space with a temp file, wipe it, then delete."),
    ("unallocated", "🗺️  Unallocated gaps", "Wipe only space between partitions on a disk."),
    ("android", "📱  Android wipe", "Factory reset via fastboot (needs adb/platform-tools)."),
    ("list_only", "📋  List devices", "Show disks and volumes without wiping."),
    ("android_list", "🔍  List Android devices", "Show adb/fastboot connected phones."),
    ("android_reboot", "🔄  Reboot to bootloader", "adb reboot bootloader for fastboot wipe."),
]

STEP_NAMES = [
    "Welcome",
    "Target",
    "Options",
    "Review",
    "Confirm",
    "Execute",
]

TOOLTIPS: Dict[str, str] = {
    "binary": "Path to the overwrite executable. Auto-detected from build/ or PATH.",
    "profile": (
        "ghost - 1 random pass (default)\n"
        "chameleon - ghost + custom nonce key\n"
        "spectrum - 2 random passes\n"
        "flash-realist - SSD hardware erase first\n"
        "filesystem-shadow - wipe + random rename + delete\n"
        "block-cartographer - partition-table gaps only\n"
        "slack-hunter - extend file to cluster, then wipe"
    ),
    "rng": (
        "vault - ChaCha20, fast & strong (default)\n"
        "turbo - xoshiro256**, fastest throughput\n"
        "hybrid - ChaCha edges + xoshiro middle\n"
        "os-chunk - OS CSPRNG per chunk (slowest, max entropy)"
    ),
    "nonce": "64 hex chars (32 bytes). Used by chameleon profile to mix the RNG key.",
    "passes": "Override profile pass count. 0 = use profile default (ghost=1, spectrum=2).",
    "threads": "Parallel write workers. 0 = auto (min cores, 16 max). USB/SD forced to 1.",
    "chunk": "I/O chunk size per write. Default 1M. Larger may help on fast internal SSDs.",
    "android_mode": (
        "factory - full factory reset via fastboot\n"
        "userdata - erase userdata partition only\n"
        "full - userdata + cache + metadata"
    ),
    "list_filter": "Filter --list output: all, disks, partitions, or volumes only.",
    "dry_run": "Show the wipe plan without writing any data. Always run this first.",
    "force": "Bypass system-disk safety block. Use only when you know the target is safe.",
    "quiet": "Disable CLI progress bar (GUI progress still works when parseable).",
    "ssd_secure_erase": "Try NVMe Format NVM / ATA SECURITY ERASE before overwrite.",
    "allow_trim": "Send TRIM/discard after wipe (off by default; visible in SSD logs).",
    "normalize_meta": "Randomize filler file timestamps before delete (filesystem-shadow).",
    "format_after": (
        "After wipe, recreate a fresh filesystem on the target.\n"
        "Whole disk: partition + quick format (diskpart on Windows, mkfs on Linux).\n"
        "Volume/partition: quick format in place.\n"
        "Not available for files, free space, or unallocated-only wipes."
    ),
    "format_fs": "exfat - SD/USB default · ntfs - Windows drives · fat32 - small cards · ext4 - Linux",
    "target": "Device path from the list or typed manually, e.g. \\\\.\\PhysicalDrive2",
    "serial": "Optional adb/fastboot serial when multiple Android devices are connected.",
    "confirm": "Must match exactly what the CLI expects - prevents accidental wipes.",
}

PROGRESS_RE = re.compile(
    r"OverWrite\s+\[[#-]+\]\s+([\d.]+)%\s+"
    r"([\d.]+\ \w+)\s+/\s+([\d.]+\ \w+)\s+"
    r"([\d.]+\ \w+)/s\s+ETA\s+(\S+)\s+pass\s+(\d+)/(\d+)"
)


@dataclass
class DeviceRow:
    kind: str
    path: str
    size: str
    info: str


@dataclass
class GuiConfig:
    operation: str = "whole"
    target_path: str = ""
    profile: str = "ghost"
    rng: str = "vault"
    nonce: str = ""
    passes: int = 0
    threads: int = 0
    chunk: str = "1M"
    dry_run: bool = False
    force: bool = False
    quiet: bool = False
    ssd_secure_erase: bool = False
    allow_trim: bool = False
    normalize_meta: bool = False
    format_after: bool = False
    format_fs: str = "exfat"
    cli_yes: bool = False
    android_mode: str = "factory"
    android_serial: str = ""
    list_filter: str = "all"


# ── Tooltip ──────────────────────────────────────────────────────────────────
class ToolTip:
    def __init__(self, widget: tk.Widget, text: str, title: str = "") -> None:
        self.widget = widget
        self.text = text
        self.title = title
        self.tip: Optional[tk.Toplevel] = None
        widget.bind("<Enter>", self._show, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def _show(self, _evt: tk.Event) -> None:
        if self.tip or not self.text.strip():
            return
        x = self.widget.winfo_rootx() + 20
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        self.tip = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry(f"+{x}+{y}")
        tw.configure(bg=Theme.HELP)
        frame = tk.Frame(tw, bg=Theme.CARD, highlightbackground=Theme.HELP,
                         highlightthickness=2, padx=10, pady=8)
        frame.pack()
        if self.title:
            tk.Label(frame, text=self.title, font=("Segoe UI", 9, "bold"),
                     fg=Theme.HELP, bg=Theme.CARD, anchor=tk.W).pack(anchor=tk.W)
        tk.Label(frame, text=self.text, font=("Segoe UI", 9), fg=Theme.TEXT,
                 bg=Theme.CARD, justify=tk.LEFT, wraplength=340).pack(anchor=tk.W)

    def _hide(self, _evt: tk.Event) -> None:
        if self.tip:
            self.tip.destroy()
            self.tip = None


def help_icon(parent: tk.Widget, tip_key: str, title: str = "") -> tk.Label:
    lbl = tk.Label(
        parent, text=" ⓘ", font=("Segoe UI", 10, "bold"),
        fg=Theme.HELP, bg=parent.cget("bg") if parent.cget("bg") else Theme.CARD,
        cursor="question_arrow",
    )
    ToolTip(lbl, TOOLTIPS.get(tip_key, ""), title or tip_key.replace("_", " ").title())
    return lbl


def labeled_row(
    parent: tk.Widget,
    row: int,
    label: str,
    tip_key: str,
    widget_factory: Callable[[tk.Widget], tk.Widget],
    bg: str = Theme.CARD,
) -> tk.Widget:
    lf = tk.Frame(parent, bg=bg)
    lf.grid(row=row, column=0, sticky=tk.EW, pady=3)
    tk.Label(lf, text=label, font=("Segoe UI", 9), fg=Theme.TEXT, bg=bg,
             width=22, anchor=tk.W).pack(side=tk.LEFT)
    help_icon(lf, tip_key).pack(side=tk.LEFT)
    w = widget_factory(lf)
    w.pack(side=tk.LEFT, padx=(6, 0), fill=tk.X, expand=True)
    return w


# ── Helpers ──────────────────────────────────────────────────────────────────
def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_overwrite_binary() -> Path:
    env = os.environ.get("OVERWRITE_BIN")
    if env:
        p = Path(env)
        if p.is_file():
            return p
    root = repo_root()
    candidates = [
        root / "build" / ("overwrite.exe" if sys.platform == "win32" else "overwrite"),
        root / "build" / "Release" / "overwrite.exe",
        root / "build" / "Debug" / "overwrite.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    which = shutil.which("overwrite.exe" if sys.platform == "win32" else "overwrite")
    if which:
        return Path(which)
    return candidates[0]


def relaunch_as_admin() -> None:
    if sys.platform != "win32":
        return
    try:
        import ctypes
        params = " ".join(f'"{a}"' if " " in a else a for a in sys.argv)
        ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, params, None, 1)
        sys.exit(0)
    except Exception as exc:
        messagebox.showerror("Elevation failed", str(exc))


def is_admin() -> bool:
    if sys.platform != "win32":
        return os.geteuid() == 0
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def parse_device_list(text: str) -> List[DeviceRow]:
    rows: List[DeviceRow] = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("Storage devices") or line.startswith("TYPE"):
            continue
        if line.startswith("----") or line.startswith("LETTER") or line.startswith("------"):
            continue
        if line.startswith("Volume ->") or line.startswith("Use path") or line.startswith("Wiping"):
            continue
        if line.startswith("Your main PC"):
            continue
        m = re.match(
            r"^(\S+)\s+(\\\\\.\\\S+|/dev/\S+)\s+(PhysicalDrive\d+|/dev/\S+)$",
            line,
        )
        if m:
            rows.append(DeviceRow("map", m.group(2), "", m.group(3)))
            continue
        parts = line.split(None, 3)
        if len(parts) >= 3 and parts[0] in ("physical", "volume", "partition", "disk", "part"):
            kind, path, size = parts[0], parts[1], parts[2]
            info = parts[3] if len(parts) > 3 else ""
            rows.append(DeviceRow(kind, path, size, info))
    return rows


def parse_progress_line(line: str) -> Optional[dict]:
    m = PROGRESS_RE.search(line.replace("\r", ""))
    if not m:
        return None
    return {
        "pct": float(m.group(1)),
        "done": m.group(2),
        "total": m.group(3),
        "rate": m.group(4),
        "eta": m.group(5),
        "pass_cur": int(m.group(6)),
        "pass_tot": int(m.group(7)),
    }


class OverWriteRunner:
    def __init__(self, binary: Path):
        self.binary = binary

    def build_argv(self, cfg: GuiConfig) -> List[str]:
        cmd = [str(self.binary)]
        if cfg.operation == "list_only":
            cmd.append("--list")
            if cfg.list_filter and cfg.list_filter != "all":
                cmd.append(cfg.list_filter)
            return cmd
        if cfg.operation == "android_list":
            return cmd + ["--android-list"]
        if cfg.operation == "android_reboot":
            cmd.append("--android-reboot-bootloader")
            if cfg.android_serial.strip():
                cmd.append(cfg.android_serial.strip())
            return cmd
        if cfg.operation == "android":
            cmd.append("--android-wipe")
            if cfg.android_serial.strip():
                cmd.append(cfg.android_serial.strip())
            cmd.extend(["--android-mode", cfg.android_mode])
        elif cfg.operation == "free_space":
            cmd.extend(["--free-space", cfg.target_path])
        elif cfg.operation == "unallocated":
            cmd.extend(["--unallocated", cfg.target_path])
        elif cfg.target_path.strip():
            cmd.append(cfg.target_path.strip())

        cmd.extend(["--profile", cfg.profile, "--rng", cfg.rng])
        if cfg.nonce.strip():
            cmd.extend(["--nonce", cfg.nonce.strip()])
        if cfg.passes > 0:
            cmd.extend(["--passes", str(cfg.passes)])
        if cfg.threads > 0:
            cmd.extend(["-t", str(cfg.threads)])
        if cfg.chunk.strip():
            cmd.extend(["-c", cfg.chunk.strip()])
        if cfg.dry_run:
            cmd.append("--dry-run")
        if cfg.force:
            cmd.append("--force")
        if cfg.quiet:
            cmd.append("-q")
        if cfg.ssd_secure_erase:
            cmd.append("--ssd-secure-erase")
        if cfg.allow_trim:
            cmd.append("--allow-trim")
        if cfg.normalize_meta:
            cmd.append("--normalize-meta")
        if cfg.format_after and cfg.operation == "whole":
            cmd.append("--format-after")
            if cfg.format_fs and cfg.format_fs != "exfat":
                cmd.append(cfg.format_fs)
        if cfg.cli_yes and not cfg.dry_run:
            cmd.append("--yes")
        return cmd

    def run_interactive(
        self,
        argv: List[str],
        confirm_text: Optional[str],
        on_line: Callable[[str], None],
        on_done: Callable[[int], None],
    ) -> None:
        def worker() -> None:
            code = 1
            try:
                proc = subprocess.Popen(
                    argv,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    cwd=str(repo_root()),
                )
                assert proc.stdin is not None and proc.stdout is not None
                sent_confirm = False
                for line in proc.stdout:
                    on_line(line)
                    if confirm_text and not sent_confirm:
                        if "TYPE EXACTLY" in line or "Type exactly:" in line:
                            proc.stdin.write(confirm_text + "\n")
                            proc.stdin.flush()
                            sent_confirm = True
                proc.wait()
                code = proc.returncode if proc.returncode is not None else 1
            except FileNotFoundError:
                on_line(f"error: binary not found: {self.binary}\n")
            except Exception as exc:
                on_line(f"error: {exc}\n")
            on_done(code)

        threading.Thread(target=worker, daemon=True).start()


class WizardApp(tk.Tk):
    CONTENT_H = 340
    LOG_H = 96

    def __init__(self) -> None:
        super().__init__()
        self.title(f"OverWrite {VERSION}")
        self.geometry(f"{WIN_W}x{WIN_H}")
        self.resizable(False, False)
        self.configure(bg=Theme.BG)
        self._center_window()

        self.binary = find_overwrite_binary()
        self.runner = OverWriteRunner(self.binary)
        self.cfg = GuiConfig()
        self.devices: List[DeviceRow] = []
        self.step = 0
        self.output_queue: queue.Queue = queue.Queue()
        self.confirm_text = ""
        self._running = False
        self.step_dots: List[tk.Canvas] = []

        self._style()
        self._build_chrome()
        self._show_step(0)
        self.after(80, self._drain_output)

        if sys.platform == "win32" and not is_admin():
            self.after(400, lambda: messagebox.showwarning(
                "Administrator recommended",
                "Run as Administrator to wipe physical disks (\\\\.\\PhysicalDriveN).",
            ))

    def _center_window(self) -> None:
        self.update_idletasks()
        sw = self.winfo_screenwidth()
        sh = self.winfo_screenheight()
        x = (sw - WIN_W) // 2
        y = max(0, (sh - WIN_H) // 2 - 20)
        self.geometry(f"{WIN_W}x{WIN_H}+{x}+{y}")

    def _style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TCombobox", fieldbackground=Theme.CARD)
        style.configure("Accent.TButton", font=("Segoe UI", 10, "bold"))
        style.configure("Nav.TButton", font=("Segoe UI", 10))
        style.map(
            "Accent.TButton",
            background=[("active", Theme.ACCENT2), ("!disabled", Theme.ACCENT)],
            foreground=[("!disabled", "#ffffff")],
        )
        style.map(
            "Nav.TButton",
            background=[("active", "#e2e8f0"), ("!disabled", "#ffffff")],
        )
        style.configure("Treeview", rowheight=22, font=("Segoe UI", 9))
        style.configure("Treeview.Heading", font=("Segoe UI", 9, "bold"))
        style.configure("green.Horizontal.TProgressbar", troughcolor=Theme.BG,
                        background=Theme.ACCENT, thickness=14)

    def _build_chrome(self) -> None:
        # ── Header ──
        header = tk.Frame(self, bg=Theme.HEADER, height=72)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        tk.Label(header, text="🛡️  OverWrite", font=("Segoe UI", 16, "bold"),
                 fg="#ffffff", bg=Theme.HEADER).pack(side=tk.LEFT, padx=16, pady=8)
        tk.Label(header, text="Secure data destruction wizard",
                 font=("Segoe UI", 10), fg="#94a3b8", bg=Theme.HEADER).pack(
            side=tk.LEFT, padx=4, pady=12)
        self.admin_badge = tk.Label(header, text="", font=("Segoe UI", 8, "bold"),
                                      bg=Theme.WARN if not is_admin() else Theme.OK,
                                      fg="#ffffff", padx=8, pady=2)
        self.admin_badge.pack(side=tk.RIGHT, padx=12, pady=8)
        self._update_admin_badge()

        # ── Step pills ──
        pill_bar = tk.Frame(self, bg=Theme.HEADER2, height=44)
        pill_bar.pack(fill=tk.X)
        pill_bar.pack_propagate(False)
        self.pill_frame = tk.Frame(pill_bar, bg=Theme.HEADER2)
        self.pill_frame.pack(expand=True)
        for i, name in enumerate(STEP_NAMES):
            cv = tk.Canvas(self.pill_frame, width=130, height=36, bg=Theme.HEADER2,
                             highlightthickness=0)
            cv.pack(side=tk.LEFT, padx=2, pady=4)
            self.step_dots.append(cv)

        self.step_title = tk.Label(self, text="", font=("Segoe UI", 11, "bold"),
                                   fg=Theme.TEXT, bg=Theme.BG)
        self.step_title.pack(anchor=tk.W, padx=16, pady=(10, 2))

        # ── Content card (fixed height) ──
        card_outer = tk.Frame(self, bg=Theme.BG, height=self.CONTENT_H + 16)
        card_outer.pack(fill=tk.X, padx=12, pady=0)
        card_outer.pack_propagate(False)
        self.content = tk.Frame(card_outer, bg=Theme.CARD, highlightbackground=Theme.BORDER,
                                highlightthickness=1)
        self.content.pack(fill=tk.BOTH, expand=True, padx=0, pady=8)
        inner = tk.Frame(self.content, bg=Theme.CARD)
        inner.pack(fill=tk.BOTH, expand=True, padx=14, pady=10)
        self.content_inner = inner

        # ── Navigation (always visible, above progress/log) ──
        nav_outer = tk.Frame(self, bg=Theme.BG, height=56)
        nav_outer.pack(fill=tk.X, padx=12, pady=(4, 0))
        nav_outer.pack_propagate(False)
        nav = tk.Frame(nav_outer, bg=Theme.CARD, highlightbackground=Theme.BORDER,
                       highlightthickness=1)
        nav.pack(fill=tk.BOTH, expand=True, padx=0, pady=0)

        self.back_btn = tk.Button(
            nav, text="←  Back", font=("Segoe UI", 10),
            bg="#ffffff", fg=Theme.TEXT, activebackground="#e2e8f0",
            relief=tk.GROOVE, bd=1, padx=18, pady=6, cursor="hand2",
            command=self._back,
        )
        self.back_btn.pack(side=tk.LEFT, padx=12, pady=8)

        tk.Label(nav, text="Navigate steps", font=("Segoe UI", 9),
                 fg=Theme.MUTED, bg=Theme.CARD).pack(side=tk.LEFT, padx=8)

        self.next_btn = tk.Button(
            nav, text="Next  →", font=("Segoe UI", 10, "bold"),
            bg=Theme.ACCENT, fg="#ffffff", activebackground=Theme.ACCENT2,
            activeforeground="#ffffff", relief=tk.FLAT, bd=0, padx=22, pady=6,
            cursor="hand2", command=self._next,
        )
        self.next_btn.pack(side=tk.RIGHT, padx=8, pady=8)

        self.run_btn = tk.Button(
            nav, text="▶  Run", font=("Segoe UI", 10, "bold"),
            bg=Theme.HEADER, fg="#ffffff", activebackground=Theme.HEADER2,
            activeforeground="#ffffff", relief=tk.FLAT, bd=0, padx=18, pady=6,
            cursor="hand2", command=self._run_wipe,
        )
        self.run_btn.pack(side=tk.RIGHT, padx=(0, 12), pady=8)

        # ── Progress strip ──
        prog_frame = tk.Frame(self, bg=Theme.BG, height=52)
        prog_frame.pack(fill=tk.X, padx=12)
        prog_frame.pack_propagate(False)
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress = ttk.Progressbar(
            prog_frame, variable=self.progress_var, maximum=100,
            style="green.Horizontal.TProgressbar", mode="determinate",
        )
        self.progress.pack(fill=tk.X, pady=(4, 2))
        self.progress_status = tk.Label(
            prog_frame, text="Ready", font=("Segoe UI", 9),
            fg=Theme.MUTED, bg=Theme.BG, anchor=tk.W,
        )
        self.progress_status.pack(fill=tk.X)

        # ── Log ──
        log_frame = tk.Frame(self, bg=Theme.LOG_BG, height=self.LOG_H + 8)
        log_frame.pack(fill=tk.X, padx=12, pady=(4, 0))
        log_frame.pack_propagate(False)
        tk.Label(log_frame, text=" Output ", font=("Consolas", 8),
                 fg="#64748b", bg=Theme.LOG_BG).pack(anchor=tk.W, padx=4)
        self.log = tk.Text(log_frame, height=6, state=tk.DISABLED, font=("Consolas", 9),
                           bg=Theme.LOG_BG, fg=Theme.LOG_FG, relief=tk.FLAT,
                           wrap=tk.WORD, padx=6, pady=4)
        log_sb = ttk.Scrollbar(log_frame, orient=tk.VERTICAL, command=self.log.yview)
        self.log.configure(yscrollcommand=log_sb.set)
        self.log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        log_sb.pack(side=tk.RIGHT, fill=tk.Y)

    def _set_nav_state(self) -> None:
        """Update Back / Next / Run enabled state and appearance."""
        on = {"state": tk.NORMAL}
        off = {"state": tk.DISABLED}
        if self.step > 0:
            self.back_btn.config(**on, bg="#ffffff", fg=Theme.TEXT)
        else:
            self.back_btn.config(**off, bg="#f1f5f9", fg=Theme.MUTED)
        if self.step < 5:
            self.next_btn.config(**on, bg=Theme.ACCENT, fg="#ffffff")
        else:
            self.next_btn.config(**off, bg="#94a3b8", fg="#e2e8f0")
        if self.step >= 3:
            self.run_btn.config(**on, bg=Theme.HEADER, fg="#ffffff")
        else:
            self.run_btn.config(**off, bg="#cbd5e1", fg="#64748b")

    def _update_admin_badge(self) -> None:
        if is_admin():
            self.admin_badge.config(text=" ✓ ADMIN ", bg=Theme.OK)
        else:
            self.admin_badge.config(text=" ⚠ NOT ADMIN ", bg=Theme.WARN)

    def _draw_pills(self) -> None:
        icons = ["🏠", "🎯", "⚙️", "📝", "✋", "🚀"]
        for i, cv in enumerate(self.step_dots):
            cv.delete("all")
            if i < self.step:
                fill, fg = Theme.STEP_DONE, "#ffffff"
            elif i == self.step:
                fill, fg = Theme.STEP_ACTIVE, Theme.HEADER
            else:
                fill, fg = Theme.STEP_IDLE, "#94a3b8"
            cv.create_oval(4, 8, 28, 32, fill=fill, outline="")
            cv.create_text(16, 20, text=icons[i], font=("Segoe UI Emoji", 9))
            cv.create_text(72, 20, text=STEP_NAMES[i], font=("Segoe UI", 8, "bold" if i == self.step else "normal"),
                            fill=fg if i == self.step else "#cbd5e1")

    def _clear_content(self) -> None:
        for w in self.content_inner.winfo_children():
            w.destroy()

    def _log(self, text: str) -> None:
        self.log.configure(state=tk.NORMAL)
        self.log.insert(tk.END, text)
        if int(self.log.index("end-1c").split(".")[0]) > 400:
            self.log.delete("1.0", "50.0")
        self.log.see(tk.END)
        self.log.configure(state=tk.DISABLED)

    def _set_progress(self, pct: Optional[float], status: str) -> None:
        if pct is not None:
            if str(self.progress.cget("mode")) == "indeterminate":
                self.progress.stop()
                self.progress.configure(mode="determinate")
            self.progress_var.set(min(100.0, max(0.0, pct)))
        self.progress_status.config(
            text=status,
            fg=Theme.ACCENT if self._running else Theme.MUTED,
        )

    def _start_progress(self, indeterminate: bool = True) -> None:
        self._running = True
        if indeterminate:
            self.progress.configure(mode="indeterminate")
            self.progress.start(12)
        self._set_progress(0, "Running…")

    def _stop_progress(self, success: bool, msg: str = "") -> None:
        self._running = False
        self.progress.stop()
        self.progress.configure(mode="determinate")
        self.progress_var.set(100.0 if success else self.progress_var.get())
        self._set_progress(None, msg or ("Complete ✓" if success else "Failed ✗"))

    def _drain_output(self) -> None:
        try:
            while True:
                item = self.output_queue.get_nowait()
                if isinstance(item, tuple) and item[0] == "progress":
                    self._set_progress(item[1], item[2])
                elif isinstance(item, tuple) and item[0] == "done":
                    self._stop_progress(item[1] == 0, item[2])
                else:
                    self._log(str(item))
                    prog = parse_progress_line(str(item))
                    if prog:
                        st = (
                            f"{prog['pct']:.1f}%  {prog['done']} / {prog['total']}  "
                            f"@ {prog['rate']}/s  ETA {prog['eta']}  "
                            f"pass {prog['pass_cur']}/{prog['pass_tot']}"
                        )
                        self._set_progress(prog["pct"], st)
        except queue.Empty:
            pass
        self.after(80, self._drain_output)

    def _show_step(self, n: int) -> None:
        self.step = n
        self.step_title.config(text=f"Step {n + 1} of 6 - {STEP_NAMES[n]}")
        self._draw_pills()
        self._set_nav_state()
        self._clear_content()
        [self._step_welcome, self._step_target, self._step_options,
         self._step_review, self._step_confirm, self._step_execute][n]()

    # ── Steps ────────────────────────────────────────────────────────────────
    def _step_welcome(self) -> None:
        p = self.content_inner
        tk.Label(p, text="Welcome to OverWrite", font=("Segoe UI", 13, "bold"),
                 fg=Theme.HEADER, bg=Theme.CARD).grid(row=0, column=0, columnspan=2, sticky=tk.W)
        tips = (
            "① Refresh devices  ② Pick target  ③ Set options  ④ Dry-run  ⑤ Confirm  ⑥ Wipe\n"
            "Physical disks on Windows need Administrator · USB/SD auto single-thread"
        )
        tk.Label(p, text=tips, font=("Segoe UI", 9), fg=Theme.MUTED, bg=Theme.CARD,
                 justify=tk.LEFT).grid(row=1, column=0, columnspan=2, sticky=tk.W, pady=(4, 12))

        lf = tk.Frame(p, bg=Theme.CARD)
        lf.grid(row=2, column=0, columnspan=2, sticky=tk.EW)
        tk.Label(lf, text="CLI binary:", font=("Segoe UI", 9), fg=Theme.TEXT,
                 bg=Theme.CARD, width=12, anchor=tk.W).pack(side=tk.LEFT)
        help_icon(lf, "binary", "Binary path").pack(side=tk.LEFT)
        self.bin_var = tk.StringVar(value=str(self.binary))
        ttk.Entry(lf, textvariable=self.bin_var, width=58).pack(side=tk.LEFT, padx=4)
        ttk.Button(lf, text="Browse", command=self._browse_binary).pack(side=tk.LEFT)

        if sys.platform == "win32" and not is_admin():
            ttk.Button(p, text="🔐  Restart as Administrator", command=relaunch_as_admin).grid(
                row=3, column=0, sticky=tk.W, pady=14)

        feats = tk.Frame(p, bg="#f0fdf4", highlightbackground=Theme.OK, highlightthickness=1)
        feats.grid(row=4, column=0, columnspan=2, sticky=tk.EW, pady=8)
        for txt in ("🔒 Secure profiles & RNG", "⚡ Multi-thread internal disks",
                    "📊 Live progress", "📱 Android fastboot"):
            tk.Label(feats, text=f"  {txt}  ", font=("Segoe UI", 9), fg=Theme.OK,
                     bg="#f0fdf4").pack(side=tk.LEFT, padx=4, pady=6)

    def _step_target(self) -> None:
        p = self.content_inner
        left = tk.Frame(p, bg=Theme.CARD, width=300)
        left.grid(row=0, column=0, sticky=tk.NS, padx=(0, 8))
        left.grid_propagate(False)
        tk.Label(left, text="Operation", font=("Segoe UI", 10, "bold"),
                 fg=Theme.HEADER, bg=Theme.CARD).pack(anchor=tk.W)
        self.op_var = tk.StringVar(value=self.cfg.operation)
        for key, label, tip in OPERATIONS:
            rb = tk.Radiobutton(
                left, text=label, value=key, variable=self.op_var,
                font=("Segoe UI", 9), fg=Theme.TEXT, bg=Theme.CARD,
                activebackground=Theme.CARD, selectcolor=Theme.ACCENT,
                anchor=tk.W, cursor="hand2",
            )
            rb.pack(anchor=tk.W, pady=1)
            ToolTip(rb, tip, label.strip()[:30])

        right = tk.Frame(p, bg=Theme.CARD)
        self.target_right = right
        right.grid(row=0, column=1, sticky=tk.NSEW)
        p.columnconfigure(1, weight=1)

        tool = tk.Frame(right, bg=Theme.CARD)
        tool.pack(fill=tk.X)
        ttk.Button(tool, text="🔄 Refresh", command=self._refresh_devices).pack(side=tk.LEFT)
        tk.Label(tool, text="Filter:", font=("Segoe UI", 9), bg=Theme.CARD).pack(side=tk.LEFT, padx=(10, 2))
        help_icon(tool, "list_filter").pack(side=tk.LEFT)
        self.list_filter_var = tk.StringVar(value=self.cfg.list_filter)
        ttk.Combobox(tool, textvariable=self.list_filter_var, values=LIST_FILTERS,
                     state="readonly", width=11).pack(side=tk.LEFT, padx=4)

        cols = ("kind", "path", "size", "info")
        tree_wrap = tk.Frame(right, bg=Theme.CARD)
        tree_wrap.pack(fill=tk.BOTH, expand=True, pady=6)
        self.tree = ttk.Treeview(tree_wrap, columns=cols, show="headings", height=7)
        for c, w in zip(cols, (72, 200, 72, 180)):
            self.tree.heading(c, text=c.upper())
            self.tree.column(c, width=w, minwidth=w)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb = ttk.Scrollbar(tree_wrap, orient=tk.VERTICAL, command=self.tree.yview)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.bind("<<TreeviewSelect>>", self._on_device_select)

        path_row = tk.Frame(right, bg=Theme.CARD)
        path_row.pack(fill=tk.X)
        tk.Label(path_row, text="Target:", font=("Segoe UI", 9), bg=Theme.CARD).pack(side=tk.LEFT)
        help_icon(path_row, "target").pack(side=tk.LEFT)
        self.target_var = tk.StringVar(value=self.cfg.target_path)
        ttk.Entry(path_row, textvariable=self.target_var).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=4)
        ttk.Button(path_row, text="📁", width=3, command=self._pick_file).pack(side=tk.LEFT)

        and_row = tk.Frame(right, bg=Theme.CARD)
        self.and_row = and_row
        tk.Label(and_row, text="Android serial:", font=("Segoe UI", 9), bg=Theme.CARD).pack(side=tk.LEFT)
        help_icon(and_row, "serial").pack(side=tk.LEFT)
        self.serial_var = tk.StringVar(value=self.cfg.android_serial)
        ttk.Entry(and_row, textvariable=self.serial_var, width=28).pack(side=tk.LEFT, padx=4)
        self.op_var.trace_add("write", self._on_operation_changed)
        self._update_target_visibility()

        self._refresh_devices()

    def _on_operation_changed(self, *_args: object) -> None:
        if hasattr(self, "and_row"):
            self._update_target_visibility()

    def _update_target_visibility(self) -> None:
        if not hasattr(self, "and_row") or not hasattr(self, "op_var"):
            return
        op = self.op_var.get()
        if op in ANDROID_OPS:
            self.and_row.pack(fill=tk.X, pady=(6, 0))
        else:
            self.and_row.pack_forget()
        if hasattr(self, "target_right"):
            needs_target = op in ("whole", "free_space", "unallocated")
            if needs_target:
                self.target_right.grid()
            else:
                self.target_right.grid_remove()

    def _step_options(self) -> None:
        self._collect_cfg()
        p = self.content_inner
        is_android = self.cfg.operation == "android"
        is_disk_wipe = self.cfg.operation == "whole"

        self.profile_var = tk.StringVar(value=self.cfg.profile)
        self.rng_var = tk.StringVar(value=self.cfg.rng)
        self.nonce_var = tk.StringVar(value=self.cfg.nonce)
        self.passes_var = tk.IntVar(value=self.cfg.passes)
        self.threads_var = tk.IntVar(value=self.cfg.threads)
        self.chunk_var = tk.StringVar(value=self.cfg.chunk)
        self.amode_var = tk.StringVar(value=self.cfg.android_mode)
        self.format_var = tk.BooleanVar(value=self.cfg.format_after)
        self.format_fs_var = tk.StringVar(value=self.cfg.format_fs)

        row = 0
        labeled_row(p, row, "Profile", "profile",
                    lambda f: ttk.Combobox(f, textvariable=self.profile_var,
                                           values=PROFILES, state="readonly", width=22))
        row += 1
        labeled_row(p, row, "RNG mode", "rng",
                    lambda f: ttk.Combobox(f, textvariable=self.rng_var,
                                           values=RNG_MODES, state="readonly", width=22))
        row += 1
        labeled_row(p, row, "Nonce (hex)", "nonce",
                    lambda f: ttk.Entry(f, textvariable=self.nonce_var, width=24))
        row += 1
        labeled_row(p, row, "Passes", "passes",
                    lambda f: ttk.Spinbox(f, from_=0, to=99, textvariable=self.passes_var, width=8))
        row += 1
        labeled_row(p, row, "Threads", "threads",
                    lambda f: ttk.Spinbox(f, from_=0, to=128, textvariable=self.threads_var, width=8))
        row += 1
        labeled_row(p, row, "Chunk size", "chunk",
                    lambda f: ttk.Combobox(f, textvariable=self.chunk_var,
                                           values=["512K", "1M", "2M", "4M", "8M", "16M"], width=10))
        row += 1

        if is_android:
            labeled_row(p, row, "Android mode", "android_mode",
                        lambda f: ttk.Combobox(f, textvariable=self.amode_var,
                                               values=ANDROID_MODES, state="readonly", width=16))
            row += 1

        if is_disk_wipe:
            fmt_row = tk.Frame(p, bg=Theme.CARD)
            fmt_row.grid(row=row, column=0, sticky=tk.EW, pady=3)
            tk.Label(fmt_row, text="Format after wipe", font=("Segoe UI", 9),
                     fg=Theme.TEXT, bg=Theme.CARD, width=22, anchor=tk.W).pack(side=tk.LEFT)
            help_icon(fmt_row, "format_after").pack(side=tk.LEFT)
            tk.Checkbutton(fmt_row, text="Enable", variable=self.format_var,
                           font=("Segoe UI", 9), fg=Theme.TEXT, bg=Theme.CARD,
                           activebackground=Theme.CARD, selectcolor=Theme.ACCENT,
                           cursor="hand2").pack(side=tk.LEFT, padx=(6, 8))
            help_icon(fmt_row, "format_fs", "Filesystem").pack(side=tk.LEFT)
            ttk.Combobox(fmt_row, textvariable=self.format_fs_var, values=FORMAT_FS,
                         state="readonly", width=10).pack(side=tk.LEFT)
            row += 1

        flags = tk.LabelFrame(p, text="  Flags  ", font=("Segoe UI", 9, "bold"),
                              fg=Theme.HEADER, bg=Theme.CARD, padx=10, pady=8)
        flags.grid(row=0, column=1, rowspan=max(row, 7), sticky=tk.NW, padx=8)
        self.dry_var = tk.BooleanVar(value=self.cfg.dry_run)
        self.force_var = tk.BooleanVar(value=self.cfg.force)
        self.quiet_var = tk.BooleanVar(value=self.cfg.quiet)
        self.ssd_var = tk.BooleanVar(value=self.cfg.ssd_secure_erase)
        self.trim_var = tk.BooleanVar(value=self.cfg.allow_trim)
        self.meta_var = tk.BooleanVar(value=self.cfg.normalize_meta)
        flag_defs = [
            ("🧪  Dry run", "dry_run", self.dry_var),
            ("⚠️  Force system disk", "force", self.force_var),
            ("🔇  Quiet CLI output", "quiet", self.quiet_var),
            ("💿  SSD secure erase", "ssd_secure_erase", self.ssd_var),
            ("✂️  Allow TRIM", "allow_trim", self.trim_var),
            ("📋  Normalize metadata", "normalize_meta", self.meta_var),
        ]
        for text, tip_key, var in flag_defs:
            row_f = tk.Frame(flags, bg=Theme.CARD)
            row_f.pack(anchor=tk.W, pady=2)
            cb = tk.Checkbutton(row_f, text=text, variable=var, font=("Segoe UI", 9),
                                fg=Theme.TEXT, bg=Theme.CARD, activebackground=Theme.CARD,
                                selectcolor=Theme.ACCENT, cursor="hand2")
            cb.pack(side=tk.LEFT)
            help_icon(row_f, tip_key).pack(side=tk.LEFT)

    def _step_review(self) -> None:
        p = self.content_inner
        tk.Label(p, text="Command preview - run dry-run to validate without writing.",
                 font=("Segoe UI", 10, "bold"), fg=Theme.HEADER, bg=Theme.CARD).pack(anchor=tk.W)
        self.review_text = tk.Text(p, height=14, font=("Consolas", 9), bg="#f8fafc",
                                   fg=Theme.TEXT, relief=tk.FLAT, wrap=tk.WORD, padx=8, pady=6)
        self.review_text.pack(fill=tk.BOTH, expand=True, pady=6)
        ttk.Button(p, text="🧪  Run dry-run", command=self._dry_run).pack(anchor=tk.W)
        self._update_review()

    def _step_confirm(self) -> None:
        self._collect_cfg()
        p = self.content_inner
        tk.Label(p, text="⚠️  Safety confirmation", font=("Segoe UI", 11, "bold"),
                 fg=Theme.DANGER, bg=Theme.CARD).pack(anchor=tk.W)
        hint = self._confirm_hint()
        tk.Label(p, text=hint, font=("Segoe UI", 10), fg=Theme.DANGER, bg="#fef2f2",
                 wraplength=860, justify=tk.LEFT, padx=10, pady=8).pack(fill=tk.X, pady=8)
        row = tk.Frame(p, bg=Theme.CARD)
        row.pack(fill=tk.X)
        tk.Label(row, text="Type exactly:", font=("Segoe UI", 9), bg=Theme.CARD).pack(side=tk.LEFT)
        help_icon(row, "confirm").pack(side=tk.LEFT)
        self.confirm_var = tk.StringVar()
        ttk.Entry(row, textvariable=self.confirm_var, width=70).pack(side=tk.LEFT, padx=6, fill=tk.X, expand=True)

    def _step_execute(self) -> None:
        p = self.content_inner
        tk.Label(p, text="Ready to execute", font=("Segoe UI", 11, "bold"),
                 fg=Theme.HEADER, bg=Theme.CARD).pack(anchor=tk.W)
        self.exec_label = tk.Label(p, text="Press Run or the button below to start.",
                                   font=("Segoe UI", 10), fg=Theme.MUTED, bg=Theme.CARD)
        self.exec_label.pack(anchor=tk.W, pady=8)
        ttk.Button(p, text="🚀  Start operation", command=self._run_wipe).pack(anchor=tk.W)
        tk.Label(p, text="Progress appears above · full log below.",
                 font=("Segoe UI", 9), fg=Theme.MUTED, bg=Theme.CARD).pack(anchor=tk.W, pady=12)

    def _confirm_hint(self) -> str:
        if self.cfg.operation == "android":
            s = self.cfg.android_serial.strip()
            return f'Type exactly: WIPE ANDROID{" " + s if s else ""}'
        if self.cfg.operation in ("list_only", "android_list", "android_reboot"):
            return "No confirmation required for list/reboot operations."
        path = self.cfg.target_path.strip()
        return f"Type exactly: {path}" if path else "Set a target path first."

    def _collect_cfg(self) -> None:
        self.cfg.operation = self.op_var.get() if hasattr(self, "op_var") else self.cfg.operation
        self.cfg.target_path = self.target_var.get() if hasattr(self, "target_var") else self.cfg.target_path
        self.cfg.android_serial = self.serial_var.get() if hasattr(self, "serial_var") else self.cfg.android_serial
        if hasattr(self, "list_filter_var"):
            self.cfg.list_filter = self.list_filter_var.get()
        if hasattr(self, "profile_var"):
            self.cfg.profile = self.profile_var.get()
            self.cfg.rng = self.rng_var.get()
            self.cfg.nonce = self.nonce_var.get()
            self.cfg.passes = int(self.passes_var.get())
            self.cfg.threads = int(self.threads_var.get())
            self.cfg.chunk = self.chunk_var.get()
            self.cfg.android_mode = self.amode_var.get()
            self.cfg.dry_run = self.dry_var.get()
            self.cfg.force = self.force_var.get()
            self.cfg.quiet = self.quiet_var.get()
            self.cfg.ssd_secure_erase = self.ssd_var.get()
            self.cfg.allow_trim = self.trim_var.get()
            self.cfg.normalize_meta = self.meta_var.get()
        if hasattr(self, "format_var"):
            self.cfg.format_after = self.format_var.get()
            self.cfg.format_fs = self.format_fs_var.get()
        if hasattr(self, "bin_var"):
            self.binary = Path(self.bin_var.get())
            self.runner = OverWriteRunner(self.binary)

    def _update_review(self) -> None:
        self._collect_cfg()
        argv = self.runner.build_argv(self.cfg)
        self.review_text.delete("1.0", tk.END)
        self.review_text.insert(tk.END, "Command:\n  " + " ".join(argv) + "\n\n")
        if self.cfg.operation == "android":
            self.review_text.insert(tk.END, self._confirm_hint() + "\n")

    def _refresh_devices(self) -> None:
        if not self.binary.is_file():
            messagebox.showerror("Binary missing", f"Build OverWrite first:\n{self.binary}")
            return
        self._collect_cfg()
        list_argv = [str(self.binary), "--list"]
        if self.cfg.list_filter and self.cfg.list_filter != "all":
            list_argv.append(self.cfg.list_filter)
        self._start_progress(True)
        self._set_progress(None, "Refreshing device list…")

        def work() -> None:
            try:
                out = subprocess.run(list_argv, capture_output=True, text=True,
                                       cwd=str(repo_root()), timeout=120)
                text = out.stdout + out.stderr
                self.output_queue.put(("done", 0, "Device list ready"))
                self.after(0, lambda: self._apply_device_list(text))
            except Exception as exc:
                self.output_queue.put(("done", 1, "List failed"))
                self.after(0, lambda: messagebox.showerror("List failed", str(exc)))

        threading.Thread(target=work, daemon=True).start()

    def _apply_device_list(self, text: str) -> None:
        self.devices = parse_device_list(text)
        for i in self.tree.get_children():
            self.tree.delete(i)
        for d in self.devices:
            self.tree.insert("", tk.END, values=(d.kind, d.path, d.size, d.info))
        self._log("\n--- device list ---\n" + text[-2000:] + "\n")

    def _on_device_select(self, _evt: tk.Event) -> None:
        sel = self.tree.selection()
        if not sel:
            return
        vals = self.tree.item(sel[0], "values")
        if len(vals) >= 2 and vals[1]:
            self.target_var.set(vals[1])

    def _pick_file(self) -> None:
        path = filedialog.askopenfilename(title="Select file to wipe")
        if path:
            self.target_var.set(path)

    def _browse_binary(self) -> None:
        path = filedialog.askopenfilename(title="Select overwrite binary",
                                          filetypes=[("Executable", "*.exe *"), ("All", "*.*")])
        if path:
            self.bin_var.set(path)
            self.binary = Path(path)
            self.runner = OverWriteRunner(self.binary)

    def _dry_run(self) -> None:
        self._collect_cfg()
        saved = self.cfg.dry_run
        self.cfg.dry_run = True
        argv = self.runner.build_argv(self.cfg)
        self.cfg.dry_run = saved
        self._log("\n--- dry-run ---\n")
        self._start_progress(True)

        def on_line(line: str) -> None:
            self.output_queue.put(line)

        def on_done(code: int) -> None:
            self.output_queue.put(("done", code, "Dry-run complete" if code == 0 else "Dry-run failed"))

        self.runner.run_interactive(argv, None, on_line, on_done)

    def _run_wipe(self) -> None:
        self._collect_cfg()
        self.confirm_text = self.confirm_var.get().strip() if hasattr(self, "confirm_var") else ""

        if self.cfg.operation in ("whole", "free_space", "unallocated", "android"):
            if self.cfg.operation != "android" and not self.cfg.target_path.strip():
                messagebox.showerror("Missing target", "Select or enter a target path.")
                return
            if not self.cfg.dry_run and self.cfg.operation != "android":
                expect = self.cfg.target_path.strip()
                if self.confirm_text != expect:
                    messagebox.showerror("Confirmation", f"Type exactly:\n{expect}")
                    return
            if not self.cfg.dry_run and self.cfg.operation == "android":
                s = self.cfg.android_serial.strip()
                expect = f"WIPE ANDROID {s}".strip() if s else "WIPE ANDROID"
                if self.confirm_text != expect:
                    messagebox.showerror("Confirmation", f"Type exactly:\n{expect}")
                    return

        argv = self.runner.build_argv(self.cfg)
        if not self.cfg.dry_run and self.cfg.operation not in (
            "list_only", "android_list", "android_reboot"
        ):
            if not messagebox.askyesno("Final warning", "This will destroy data.\n\nProceed?"):
                return
            self.cfg.cli_yes = True
            argv = self.runner.build_argv(self.cfg)
            self.cfg.cli_yes = False

        self._log("\n--- run ---\n" + " ".join(argv) + "\n")
        if hasattr(self, "exec_label"):
            self.exec_label.config(text="Running…", fg=Theme.ACCENT)
        self._start_progress(not self.cfg.quiet)

        confirm = None

        def on_line(line: str) -> None:
            self.output_queue.put(line)

        def on_done(code: int) -> None:
            msg = "Success ✓" if code == 0 else f"Failed (exit {code})"
            self.output_queue.put(("done", code, msg))
            if hasattr(self, "exec_label"):
                self.after(0, lambda: self.exec_label.config(
                    text=msg, fg=Theme.OK if code == 0 else Theme.DANGER))

        self.runner.run_interactive(argv, confirm, on_line, on_done)

    def _back(self) -> None:
        if self.step > 0:
            self._collect_cfg()
            self._show_step(self.step - 1)

    def _next(self) -> None:
        self._collect_cfg()
        if self.step == 1:
            op = self.cfg.operation
            if op in ("whole", "free_space", "unallocated") and not self.cfg.target_path.strip():
                if not messagebox.askyesno("No target", "No target selected. Continue?"):
                    return
        if self.step < 5:
            self._show_step(self.step + 1)
            if self.step == 3:
                self._update_review()


def main() -> None:
    app = WizardApp()
    app.mainloop()


if __name__ == "__main__":
    main()
