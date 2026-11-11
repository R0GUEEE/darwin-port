#!/bin/sh
# Install/check the minimal host toolchain for darwin-port.
# Usage: ./bootstrap.sh [--install] [--check]
set -eu
SCRIPT_DIR=${0%/*}
[ "$SCRIPT_DIR" = "$0" ] && SCRIPT_DIR=.
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR" && pwd) || exit 1
MODE=${1:---check}
case "$MODE" in
  --install) ;;
  --check) ;;
  -h|--help) echo "Usage: $0 [--install|--check]"; exit 0 ;;
  *) echo "Unknown option: $MODE" >&2; exit 2 ;;
esac
missing=0
for tool in make cc clang llvm-mc ld64.lld python3; do
  if command -v "$tool" >/dev/null 2>&1; then
    printf 'found  %-10s %s\n' "$tool" "$(command -v "$tool")"
  else
    printf 'missing %s\n' "$tool" >&2
    missing=1
  fi
done
if [ "$MODE" = --install ]; then
  if command -v apk >/dev/null 2>&1; then
    apk add build-base clang llvm lld python3 openssl
  elif command -v apt-get >/dev/null 2>&1; then
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential clang llvm lld python3 openssl
  else
    echo 'No supported package manager (apk or apt-get); install the tools listed above.' >&2
    exit 1
  fi
  missing=0
  for tool in make cc clang llvm-mc ld64.lld python3; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'still missing %s\n' "$tool" >&2; missing=1; }
  done
fi
if [ "$missing" -ne 0 ]; then
  echo 'Required tools missing. Run ./bootstrap.sh --install or install them with your OS package manager.' >&2
  exit 1
fi
printf 'Host tool versions:\n'
cc --version | head -n 1
clang --version | head -n 1
llvm-mc --version | head -n 1
ld64.lld --version
python3 --version
printf '\nDependencies ready. Run: make test\n'
