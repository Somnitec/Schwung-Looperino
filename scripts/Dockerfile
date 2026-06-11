# Looperoni build environment — cross-compiles dsp.so for Ableton Move (aarch64).
FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    make file bash \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-
WORKDIR /build
CMD ["bash", "-lc", "CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh"]
