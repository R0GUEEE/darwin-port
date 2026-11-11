#!/bin/sh
# Smart installation for darwin-port: detect OS, install only selected features,
# configure a local SDK path, and validate the result.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROFILE=full
INSTALL=1
CONFIGURE_SDK=auto
RUN_TESTS=1
YES=0
usage() {
  cat <<'EOF'
Usage: scripts/install.sh [options]

Profiles:
  --profile core       loader build + Python utilities (default: full)
  --profile full       core plus LLVM Mach-O tools and XIP-verification OpenSSL
  --profile sdk        full plus validation/persistence of an already available SDK

Options:
  --check-only         detect/report requirements; do not install packages
  --no-tests           skip the final make test
  --sdk PATH           configure an existing MacOSX.sdk path
  --no-sdk             do not look for/configure an SDK
  --config PATH        env file to update (default: darwin-port.env)
  --yes                noninteractive package install (default behavior)
  -h, --help           show this help

SDK acquisition is not automated. Apple SDK/Xcode archives must be provided
separately; this script never downloads proprietary SDK content.
EOF
}
CONFIG=$ROOT/darwin-port.env
SDK_PATH=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --profile) [ "$#" -ge 2 ] || { echo 'missing --profile value' >&2; exit 2; }; PROFILE=$2; shift 2 ;;
    --check-only) INSTALL=0; shift ;;
    --no-tests) RUN_TESTS=0; shift ;;
    --sdk) [ "$#" -ge 2 ] || { echo 'missing --sdk path' >&2; exit 2; }; SDK_PATH=$2; CONFIGURE_SDK=explicit; shift 2 ;;
    --no-sdk) CONFIGURE_SDK=none; shift ;;
    --config) [ "$#" -ge 2 ] || { echo 'missing --config path' >&2; exit 2; }; CONFIG=$2; shift 2 ;;
    --yes) YES=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
case "$PROFILE" in core|full|sdk) ;; *) echo "unknown profile: $PROFILE" >&2; exit 2 ;; esac
[ "$PROFILE" = sdk ] && CONFIGURE_SDK=auto
case "$CONFIG" in /*) ;; *) CONFIG=$ROOT/$CONFIG ;; esac
command -v apk >/dev/null 2>&1 && PKG=apk || PKG=
[ -n "$PKG" ] || { command -v apt-get >/dev/null 2>&1 && PKG=apt-get || PKG=; }
if [ -n "$PKG" ]; then
  case "$PKG" in
    apk) BASE='build-base python3'; FULL='clang llvm lld openssl';;
    apt-get) BASE='build-essential python3'; FULL='clang llvm lld openssl';;
  esac
else
  BASE=; FULL=
fi
if [ "$PROFILE" = core ]; then TOOLS=$BASE; else TOOLS="$BASE $FULL"; fi
if [ "$INSTALL" -eq 1 ]; then
  [ -n "$PKG" ] || { echo 'No supported package manager found. Use --check-only or install tools manually.' >&2; exit 1; }
  if [ "$PKG" = apt-get ]; then
    if [ "$(id -u)" -eq 0 ]; then SUDO=; else SUDO=sudo; fi
    $SUDO apt-get update
    # shellcheck disable=SC2086
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y $TOOLS
  else
    if [ "$(id -u)" -eq 0 ]; then APK=apk; else APK='sudo apk'; fi
    # shellcheck disable=SC2086
    $APK add $TOOLS
  fi
fi
printf 'darwin-port installer\nprofile: %s\npackage manager: %s\n' "$PROFILE" "${PKG:-none}"
if [ "$PROFILE" = core ]; then DARWIN_PORT_PROFILE=core "$ROOT/scripts/doctor.sh"; else DARWIN_PORT_PROFILE=full "$ROOT/scripts/doctor.sh"; fi
if [ "$CONFIGURE_SDK" = explicit ]; then
  [ -n "$SDK_PATH" ] || { echo 'empty SDK path' >&2; exit 2; }
  "$ROOT/scripts/sdk-setup.sh" "$SDK_PATH" --write-config "$CONFIG"
elif [ "$CONFIGURE_SDK" = auto ]; then
  if [ -n "${DARWIN_SDK:-}" ] || [ -d /opt/xcode/MacOSX.sdk ] || [ -d /opt/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk ] || [ -d /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk ]; then
    "$ROOT/scripts/sdk-setup.sh" --write-config "$CONFIG"
  else
    printf 'SDK step skipped: no local macOS SDK detected. Configure one later with:\n  scripts/sdk-setup.sh /path/to/MacOSX.sdk --write-config %s\n' "$CONFIG"
  fi
fi
if [ "$RUN_TESTS" -eq 1 ]; then
  make -C "$ROOT" test
fi
printf '\nSetup complete. To use the saved config in this shell:\n  set -a; . %s; set +a\n' "$CONFIG"
