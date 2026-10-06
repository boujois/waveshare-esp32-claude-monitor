#!/bin/sh
# Updates Claude Monitor (the Mac helper and the display) to the latest GitHub release.
#   update.sh            update anything that's behind
#   update.sh --check    just show installed and available versions
DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/find_python.sh"
cd "$DIR" && exec "$PY" update.py "$@"
