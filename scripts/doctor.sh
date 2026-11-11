#!/bin/sh
# Validate darwin-port configuration and host toolchains.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CONFIG=${DARWIN_PORT_CONFIG:-$ROOT/darwin-port.env}
PROFILE=${DARWIN_PORT_PROFILE:-full}
[ ! -f "$CONFIG" ] || . "$CONFIG"
case "$PROFILE" in
  core) tools='make cc python3' ;;
  full) tools='make cc clang llvm-mc ld64.lld python3' ;;
  *) echo "doctor: unknown profile '$PROFILE' (choose core or full)" >&2; exit 2 ;;
esac
failed=0
printf 'darwin-port doctor (profile: %s)\n' "$PROFILE"
for tool in $tools; do
  if command -v "$tool" >/dev/null 2>&1; then
    printf 'PASS %-10s %s\n' "$tool" "$(command -v "$tool")"
  else
    printf 'FAIL %-10s not found in PATH\n' "$tool"
    failed=1
  fi
done
if [ -n "${DARWIN_SDK:-}" ]; then
  if [ -d "$DARWIN_SDK" ] && [ -d "$DARWIN_SDK/usr/include" ] && [ -d "$DARWIN_SDK/usr/lib" ]; then
    printf 'PASS DARWIN_SDK %s\n' "$DARWIN_SDK"
  else
    printf 'FAIL DARWIN_SDK configured path is incomplete: %s\n' "$DARWIN_SDK"
    failed=1
  fi
else
  sdk=${DARWIN_SDK:-/opt/xcode/MacOSX.sdk}
  if [ -d "$sdk/usr/include" ] && [ -d "$sdk/usr/lib" ]; then
    printf 'PASS DARWIN_SDK %s\n' "$sdk"
  else
    printf 'INFO DARWIN_SDK not configured; loader tests still work (run scripts/sdk-setup.sh when available)\n'
  fi
fi
if [ "$PROFILE" = full ]; then
  for tool in openssl; do
    if command -v "$tool" >/dev/null 2>&1; then
      printf 'PASS %-10s %s (XIP signature verification)\n' "$tool" "$(command -v "$tool")"
    else
      printf 'INFO %-10s missing (only verify_xip.py signature verification needs it)\n' "$tool"
    fi
  done
fi
if [ -n "${DARWIN_SDK:-}" ]; then
  printf 'CONFIG DARWIN_SDK=%s\n' "$DARWIN_SDK"
fi
if [ -n "${HOST_CLANG:-}" ]; then
  printf 'CONFIG HOST_CLANG=%s\n' "$HOST_CLANG"
fi
if [ -n "${DS_SYSROOT:-}" ]; then
  printf 'CONFIG DS_SYSROOT=%s\n' "$DS_SYSROOT"
fi
if [ -n "${DS_LIB_PATH:-}" ]; then
  printf 'CONFIG DS_LIB_PATH=%s\n' "$DS_LIB_PATH"
fi
if [ -n "${DARWIN_TARGET:-}" ]; then
  printf 'CONFIG DARWIN_TARGET=%s\n' "$DARWIN_TARGET"
fi
if [ "$failed" -ne 0 ]; then
  echo 'doctor: required tools missing; run ./bootstrap.sh --install or adjust PATH' >&2
  exit 1
fi
printf 'doctor: required tools ready\n'
