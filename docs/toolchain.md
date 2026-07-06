# Build path for Schwung module DSP (.so) on this machine — verified

## 1. How the reference modules actually build

**Target is aarch64 (ARM64), not armv7.** Verified three ways:

- `/home/memo/Documents/code/Schwung-Looperino/reference/schwung/Dockerfile` — `FROM debian:bookworm`, installs `gcc-aarch64-linux-gnu`, sets `ENV CROSS_PREFIX=aarch64-linux-gnu-`; prints "Target: aarch64-linux-gnu".
- `/home/memo/Documents/code/Schwung-Looperino/reference/schwung-twinsampler/twinsampler/dsp.so` and `dsp_core.so` ship **prebuilt**; `file` says `ELF 64-bit LSB shared object, ARM aarch64`, `readelf -h` confirms Class ELF64 / Machine AArch64.
- `/home/memo/Documents/code/Schwung-Looperino/reference/magneto-move/scripts/build.sh` line 18: `aarch64-linux-gnu-gcc -O2 -shared -fPIC -ffast-math -o .../magneto.so /build/src/dsp/*.c -I/build/src/dsp -lm` inside a `debian:bookworm-slim` container (its `scripts/Dockerfile` installs `gcc/g++/binutils-aarch64-linux-gnu`, `make`).

**Build contract:** `scripts/build.sh` in schwung uses `${CROSS_PREFIX}gcc` / `${CROSS_PREFIX}g++` everywhere; if `CROSS_PREFIX` is unset and not inside Docker, it wraps itself in `docker build` + `docker run` (image `schwung-builder`). No Makefile/CMake for module DSPs — plain one-liner gcc invocations: `-O3 -shared -fPIC ... -lm` (`-ldl -lpthread` where needed). `BUILDING.md` documents the non-Docker path as `CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh` (Debian/Ubuntu apt toolchain) or macOS `aarch64-unknown-linux-gnu` via Homebrew. Toolchain in the reference image is **gcc 12.2.0 (Debian bookworm), glibc 2.36 sysroot**; the Move itself runs **glibc 2.34** (build.sh comments this explicitly — they wrap `arc4random` for that reason).

## 2. What this machine actually is (surprise)

The shell is **not flatpak-confined**. cgroup shows `snap.code.code-*.scope` — VS Code here runs as a **snap with effectively full host access**: Fedora 44, `systemd-detect-virt` = none, no `/.flatpak-info`, no `/usr/lib/sdk`, `dnf`/`rpm` present, user `memo` in `wheel`. `flatpak-spawn` doesn't exist and isn't needed.

Available: `gcc` 16.1.1 (native x86_64 only), **`podman` 5.x rootless (crun) — works**. Missing: g++, clang, zig, go, docker, any `aarch64-linux-gnu-*` or `arm-linux-gnueabihf-*` cross toolchain.

## 3. On-device compile: non-starter

No mention of on-device gcc anywhere in schwung docs/scripts (grepped `docker|toolchain|cross|gcc` across BUILDING.md, scripts, module-creator skill). The only documented builders are Docker and a host cross-toolchain. The Move's root FS is ~463 MB and usually 100% full (schwung CLAUDE.md "Device Constraints") — installing a toolchain there is not viable.

## 4. RECOMMENDED PATH (verified end-to-end on this machine): rootless podman + reference builder image

I built the exact reference builder image with podman and cross-compiled a test `.so`: output is `ELF 64-bit LSB shared object, ARM aarch64` referencing only `GLIBC_2.17` symbols (safe on Move's glibc 2.34). Compiler inside: `aarch64-linux-gnu-gcc (Debian 12.2.0-14)` — byte-for-byte the same toolchain the reference modules use.

**One-time setup** (image is already cached as `schwung-builder-test` from my verification; to create it properly for the project):

```bash
mkdir -p ~/Documents/code/Schwung-Looperino/scripts
cat > ~/Documents/code/Schwung-Looperino/scripts/Dockerfile <<'EOF'
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
EOF
podman build -t schwung-builder -f ~/Documents/code/Schwung-Looperino/scripts/Dockerfile ~/Documents/code/Schwung-Looperino/scripts
```

**Per-build** (magneto-style module layout; note the `:Z` — required on Fedora SELinux, verified):

```bash
cd ~/Documents/code/Schwung-Looperino
mkdir -p dist/looperino
podman run --rm -v "$PWD:/build:Z" schwung-builder \
  aarch64-linux-gnu-gcc -O3 -shared -fPIC \
  -o dist/looperino/dsp.so src/dsp/*.c -Isrc/dsp -lm
file dist/looperino/dsp.so   # must say: ELF 64-bit ... ARM aarch64
```

Rootless podman maps container root to your host uid, so output files land owned by `memo` — no `-u` gymnastics needed (I verified file ownership).

**If you ever need to run schwung's own `scripts/build.sh` unchanged** (it hardcodes `docker`), a user-local shim works without any system install:

```bash
mkdir -p ~/.local/bin
printf '#!/bin/sh\nexec podman "$@"\n' > ~/.local/bin/docker
chmod +x ~/.local/bin/docker   # ~/.local/bin is on PATH on Fedora
```

Caveats for that full-host build: their `docker run -v "$REPO_ROOT:/build"` lacks `:Z` (SELinux denials likely — either add `:Z` in the script or set `label=false` in `~/.config/containers/containers.conf`), and their `-u "$(id -u):$(id -g)"` maps to a subuid under rootless podman (harmless for reading, may create root-owned-looking files inside; drop it or use `--userns=keep-id`). For module-only builds none of this applies since we control our own build script.

## 5. FALLBACK (also verified): zig cc, zero-daemon, exact glibc pin

Downloaded zig 0.14.1 (49 MB) into scratch and compiled the same test source: output `ELF 64-bit ... ARM aarch64`, only `GLIBC_2.17` refs. Advantage over the ARM GNU tarball: `-target aarch64-linux-gnu.2.34` pins **exactly** the Move's glibc version, eliminating the newer-symbol class of bug entirely.

```bash
mkdir -p ~/Documents/code/Schwung-Looperino/toolchain
cd ~/Documents/code/Schwung-Looperino/toolchain
curl -LO https://ziglang.org/download/0.14.1/zig-x86_64-linux-0.14.1.tar.xz
tar xf zig-x86_64-linux-0.14.1.tar.xz
# build:
~/Documents/code/Schwung-Looperino/toolchain/zig-x86_64-linux-0.14.1/zig cc \
  -target aarch64-linux-gnu.2.34 -O3 -shared -fPIC \
  -o dist/looperino/dsp.so src/dsp/*.c -Isrc/dsp -lm
```

(Third option, not preferred: ARM GNU toolchain 13.3.rel1 `x86_64-aarch64-none-linux-gnu` tarball — URL verified live at developer.arm.com — but its sysroot glibc is newer than 2.34, so zig is the better standalone fallback.)

## Verification artifacts (scratch, disposable)

- `/tmp/claude-1000/-home-memo/9d95c7c2-3798-4cd7-8c6b-78c342d5bf75/scratchpad/xbuild/` — Dockerfile, `test_dsp.c`, `test_dsp.so` (podman/gcc), `test_dsp_zig.so` (zig)
- podman image `localhost/schwung-builder-test:latest` (can retag to `schwung-builder` or `podman rmi` it)
- zig 0.14.1 extracted in scratchpad (session-scoped; re-download to `toolchain/` for permanent use)

## Key risks / open uncertainties (from study)

- Environment mismatch with prior notes: this shell runs under snap-packaged VS Code with full host access (Fedora 44 host tools, rootless podman), NOT the flatpak sandbox described in memory. If the user later opens the project in flatpak VS Code (com.visualstudio.code), podman/gcc will not be visible there and the commands would need flatpak-spawn --host prefixes — re-verify in that shell before relying on this report.
- glibc skew: Debian bookworm toolchain has glibc 2.36 sysroot vs Move's 2.34. Proven safe by upstream (all reference modules build this way; my test .so referenced only GLIBC_2.17), but avoid glibc>=2.35 symbols (e.g. arc4random) in DSP code, or use the zig fallback which pins glibc 2.34 exactly.
- SELinux: podman bind mounts on Fedora require ':Z' (verified) — schwung's own build.sh omits it, so running their full host build via a docker->podman shim needs the :Z patch or containers.conf label=false.
- Rootless podman semantics differ from Docker for the '-u $(id -u)' flag used in schwung's build.sh (subuid mapping); module builds avoid this by using our own podman command, but a full schwung-host build through their script is only partially verified (image build + compile verified; their end-to-end script not run).
- Network dependency: first 'podman build' pulls debian:bookworm-slim and apt packages (~150 MB); offline builds work only after the image is cached (already cached as schwung-builder-test from this session).
- zig cc is clang-based, so the fallback .so is not byte-identical to reference gcc builds; fine for plain C DSP with -lm, but any future C++/-static-libstdc++ or exotic-flag needs should be validated on device.
- On-device Move behavior (dlopen of the produced dsp.so by the Schwung host) was not tested — only architecture and symbol-version correctness were verified locally; final validation requires deploying to move.local.
