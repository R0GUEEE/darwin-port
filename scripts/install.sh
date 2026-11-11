#!/bin/sh
# Backward-compatible entry point for the unified installer.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$ROOT/setup.sh" install "$@"
