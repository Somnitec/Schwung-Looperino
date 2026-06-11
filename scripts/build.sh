#!/usr/bin/env bash
#
# Build the Looperoni DSP plugin (dsp.so) for the Ableton Move (aarch64 Linux)
# and stage the full module under dist/looperoni/ ready to tar up.
#
# Cross-compiler: set CROSS_PREFIX to your aarch64 toolchain prefix.
#   Linux  : CROSS_PREFIX=aarch64-linux-gnu-          (gcc-aarch64-linux-gnu)
#   macOS  : CROSS_PREFIX=aarch64-unknown-linux-gnu-  (messense/macos-cross-toolchains)
#
# If no cross-compiler is found and Docker is available, it builds in the
# Schwung builder container instead (see scripts/Dockerfile).
#
# Usage:
#   ./scripts/build.sh                 # auto-detect toolchain or Docker
#   CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODULE_ID="looperoni"
OUT="dist/${MODULE_ID}"
CROSS_PREFIX="${CROSS_PREFIX:-}"

detect_cross() {
    if [ -n "$CROSS_PREFIX" ]; then return 0; fi
    for p in aarch64-linux-gnu- aarch64-unknown-linux-gnu-; do
        if command -v "${p}gcc" >/dev/null 2>&1; then CROSS_PREFIX="$p"; return 0; fi
    done
    return 1
}

build_native() {
    local CC="${CROSS_PREFIX}gcc"
    echo "==> Compiling ${MODULE_ID}.so with ${CC}"
    mkdir -p "$OUT"
    "$CC" -g -O3 -shared -fPIC \
        src/looperoni.c \
        -o "$OUT/dsp.so" \
        -Isrc \
        -lm
    "${CROSS_PREFIX}file" "$OUT/dsp.so" 2>/dev/null || file "$OUT/dsp.so" || true
}

stage_assets() {
    echo "==> Staging module assets into $OUT"
    mkdir -p "$OUT"
    cp module.json          "$OUT/"
    cp ui.js                "$OUT/"
    cp help.json            "$OUT/"
    cp settings-schema.json "$OUT/"
    echo "==> dist tree:"
    ( cd dist && find "${MODULE_ID}" -type f | sort )
}

if detect_cross; then
    echo "==> Using cross toolchain prefix: ${CROSS_PREFIX}"
    build_native
    stage_assets
elif command -v docker >/dev/null 2>&1; then
    echo "==> No cross toolchain found; building in Docker"
    docker build -t looperoni-builder -f scripts/Dockerfile .
    docker run --rm -v "$ROOT":/build -w /build looperoni-builder \
        bash -lc "CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh"
else
    echo "ERROR: need an aarch64 cross-compiler (set CROSS_PREFIX) or Docker." >&2
    echo "  Linux: sudo apt-get install gcc-aarch64-linux-gnu" >&2
    echo "  macOS: brew tap messense/macos-cross-toolchains && brew install aarch64-unknown-linux-gnu" >&2
    exit 1
fi

echo "==> Build complete: $OUT/dsp.so"
