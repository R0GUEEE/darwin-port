#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CONFIG=${DARWIN_PORT_CONFIG:-$ROOT/darwin-port.env}
SDK=
WRITE=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --write-config) [ "$#" -ge 2 ] || { echo 'missing config path' >&2; exit 2; }; CONFIG=$2; WRITE=1; shift 2 ;;
        -h|--help) echo 'Usage: sdk-setup.sh [SDK_PATH] [--write-config FILE]'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) [ -z "$SDK" ] || { echo 'only one SDK path allowed' >&2; exit 2; }; SDK=$1; shift ;;
    esac
done
ACTION=plan
[ "$WRITE" -eq 0 ] || ACTION=configure
# tools permits SDK configuration on x64 hosts too.
if [ -n "$SDK" ]; then
    exec "$ROOT/setup.sh" "$ACTION" --profile tools --config "$CONFIG" --sdk "$SDK"
fi
exec "$ROOT/setup.sh" "$ACTION" --profile tools --config "$CONFIG"
