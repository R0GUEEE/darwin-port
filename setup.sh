#!/bin/sh
# Portable entry point. Never install dependencies before showing a plan.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    cat >&2 <<'EOF'
Python 3.8+ is required to start setup. Install Python once, then retry:
  Debian/Ubuntu: sudo apt-get update && sudo apt-get install python3
  Alpine:        apk add python3
  Fedora:        sudo dnf install python3
  Arch:          sudo pacman -S python
Windows: run setup inside a Linux WSL distribution.
EOF
    exit 1
fi
exec python3 "$ROOT/scripts/setup.py" "$@"
