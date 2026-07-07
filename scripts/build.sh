#!/bin/bash
# Cross-compile dsp.so for the Move (aarch64) and assemble dist/looperino/.
# Uses rootless podman + the same debian:bookworm gcc the reference modules use.
# The :Z mount flag is required on Fedora (SELinux).
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=schwung-builder
if ! podman image exists "$IMAGE"; then
    podman build -t "$IMAGE" -f scripts/Dockerfile scripts
fi

mkdir -p dist/looperino
podman run --rm -v "$PWD:/build:Z" "$IMAGE" \
    aarch64-linux-gnu-gcc -O2 -g -shared -fPIC -std=c11 -Wall -Wextra \
    -o dist/looperino/dsp.so \
    src/dsp/looperino.c src/dsp/chains.c src/dsp/punchfx.c \
    src/dsp/kit.c src/dsp/wavio.c src/dsp/tonalmap.c -lm

cp src/module.json src/ui.js dist/looperino/
[ -f src/help.json ] && cp src/help.json dist/looperino/

podman run --rm -v "$PWD:/build:Z" "$IMAGE" sh -c \
    'file dist/looperino/dsp.so && aarch64-linux-gnu-nm -D dist/looperino/dsp.so | grep move_plugin_init_v2'

tar -czf dist/looperino-module.tar.gz -C dist looperino
echo "OK: dist/looperino/ + dist/looperino-module.tar.gz"
