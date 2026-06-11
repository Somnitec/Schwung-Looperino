#!/usr/bin/env bash
#
# Copy a freshly built Looperoni module straight onto a Move running Schwung,
# for fast dev iteration (skips the Schwung Manager / GitHub release flow).
#
# Requires: ./scripts/build.sh has produced dist/looperoni/.
# Move must be reachable over SSH (default host: ableton@move.local).
#
# Usage:
#   ./scripts/deploy.sh                       # ableton@move.local
#   MOVE_HOST=ableton@192.168.1.50 ./scripts/deploy.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODULE_ID="looperoni"
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
DEST="/data/UserData/schwung/modules/overtake/${MODULE_ID}"

if [ ! -f "dist/${MODULE_ID}/dsp.so" ]; then
    echo "ERROR: dist/${MODULE_ID}/dsp.so missing. Run ./scripts/build.sh first." >&2
    exit 1
fi

echo "==> Deploying to ${MOVE_HOST}:${DEST}"
ssh "$MOVE_HOST" "mkdir -p '$DEST'"
scp -r "dist/${MODULE_ID}/." "${MOVE_HOST}:${DEST}/"

echo "==> Triggering module rescan"
# Schwung Manager rescan endpoint (best-effort); otherwise reselect in the menu.
ssh "$MOVE_HOST" "curl -s -X POST http://127.0.0.1:7700/api/modules/rescan >/dev/null 2>&1 || true"

echo "==> Done. On the Move: open Schwung menu -> Looperoni to (re)launch."
