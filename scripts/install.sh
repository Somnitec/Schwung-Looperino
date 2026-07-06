#!/bin/bash
# Deploy dist/looperino/ to the Move over SSH.
# Iteration notes (schwung-module-guide §6.2):
#   - ui.js + module.json: re-read on every module (re)launch — just exit/re-enter.
#   - dsp.so: dlopen'd per launch — exit the module fully (not suspend) first.
# Usage: scripts/install.sh [--no-build]
set -euo pipefail
cd "$(dirname "$0")/.."

MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules/overtake/looperino

[ "${1:-}" = "--no-build" ] || scripts/build.sh

ssh "$MOVE_HOST" "mkdir -p $DEST"
scp dist/looperino/module.json dist/looperino/ui.js dist/looperino/dsp.so \
    "$MOVE_HOST:$DEST/"

echo "Deployed to $MOVE_HOST:$DEST"
echo "Launch on device: Shift+Vol+Jog-Click (Tools menu) -> Looperino"
echo "Debug log:  ssh $MOVE_HOST 'touch /data/UserData/schwung/debug_log_on'"
echo "            ssh $MOVE_HOST 'tail -f /data/UserData/schwung/debug.log'"
