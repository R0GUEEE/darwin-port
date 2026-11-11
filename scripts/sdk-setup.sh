#!/bin/sh
# Locate a usable macOS SDK and optionally persist its path to config.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CONFIG=${DARWIN_PORT_CONFIG:-$ROOT/darwin-port.env}
OUT=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --write-config) [ "$#" -ge 2 ] || { echo 'usage: sdk-setup.sh [--write-config FILE] [SDK_PATH]' >&2; exit 2; }; OUT=$2; shift 2 ;;
    -h|--help) echo 'Usage: scripts/sdk-setup.sh [--write-config FILE] [SDK_PATH]'; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) [ -z "${SDK_ARG:-}" ] || { echo 'only one SDK path may be supplied' >&2; exit 2; }; SDK_ARG=$1; shift ;;
  esac
done
is_sdk() { [ -d "$1/usr/include" ] && [ -d "$1/usr/lib" ] && [ -d "$1/System/Library/Frameworks" ]; }
found=
if [ -n "${SDK_ARG:-}" ]; then
  is_sdk "$SDK_ARG" && found=$(CDPATH= cd -- "$SDK_ARG" && pwd) || { echo "not a macOS SDK: $SDK_ARG" >&2; exit 1; }
elif [ -n "${DARWIN_SDK:-}" ] && is_sdk "$DARWIN_SDK"; then
  found=$(CDPATH= cd -- "$DARWIN_SDK" && pwd)
else
  for candidate in /opt/xcode/MacOSX.sdk /opt/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk; do
    if is_sdk "$candidate"; then found=$candidate; break; fi
  done
fi
if [ -z "$found" ]; then
  cat >&2 <<'EOF'
No macOS SDK found. This is optional for loader tests.
Set DARWIN_SDK to an existing MacOSX.sdk directory or pass its path here.
The project does not download or redistribute Apple's SDK.
EOF
  exit 1
fi
printf 'SDK: %s\n' "$found"
printf 'Headers: %s\n' "$found/usr/include"
printf 'Frameworks: %s\n' "$found/System/Library/Frameworks"
printf 'Link stubs: %s\n' "$found/usr/lib"
if [ -n "$OUT" ]; then
  [ -f "$OUT" ] || { echo "config file not found: $OUT" >&2; exit 1; }
  tmp=${OUT}.tmp.$$
  trap 'rm -f "$tmp"' EXIT HUP INT TERM
  awk '!/^DARWIN_SDK=/' "$OUT" > "$tmp"
  printf '\nDARWIN_SDK=%s\n' "$(printf '%s' "$found" | sed "s/'/'\\''/g; s/.*/'&'/")" >> "$tmp"
  chmod "$(stat -c %a "$OUT" 2>/dev/null || printf 644)" "$tmp" 2>/dev/null || true
  mv "$tmp" "$OUT"
  trap - EXIT HUP INT TERM
  printf 'Updated %s\n' "$OUT"
fi
