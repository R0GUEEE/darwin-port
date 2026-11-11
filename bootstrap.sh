#!/bin/sh
# Compatibility entry point; all dependency logic lives in setup.py.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODE=${1:---check}
[ "$#" -eq 0 ] || shift
case "$MODE" in
    --install) exec "$ROOT/setup.sh" install "$@" ;;
    --check) exec "$ROOT/setup.sh" doctor "$@" ;;
    -h|--help) echo 'Usage: bootstrap.sh [--check|--install] [setup options]' ;;
    *) echo "Unknown option: $MODE" >&2; exit 2 ;;
esac
