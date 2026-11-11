#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$ROOT/setup.sh" doctor --profile "${DARWIN_PORT_PROFILE:-auto}" "$@"
