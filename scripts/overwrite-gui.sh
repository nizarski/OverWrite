#!/usr/bin/env bash
# Launch OverWrite GUI (Linux / macOS)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUI="$ROOT/gui/overwrite_gui.py"

if [[ ! -f "$GUI" ]]; then
    echo "GUI not found: $GUI" >&2
    exit 1
fi

if ! python3 -c "import tkinter" 2>/dev/null; then
    echo "Install Python 3 tkinter, e.g.:" >&2
    echo "  sudo apt install python3-tk   # Debian/Ubuntu" >&2
    exit 1
fi

exec python3 "$GUI" "$@"
