# Dev notes — build, deploy, debug

Quick reference. The full verified deep-dives live in
[schwung-module-guide.md](schwung-module-guide.md) (module anatomy, control
map, DSP API, audio input, clock, realtime rules) and
[toolchain.md](toolchain.md) (why podman, fallbacks).

## Build

```bash
scripts/build.sh        # podman + debian bookworm gcc-aarch64-linux-gnu
```

Output: `dist/looperino/{module.json,ui.js,dsp.so}` + `dist/looperino-module.tar.gz`.
First run builds the `schwung-builder` image (~150 MB download, then cached).
Target: aarch64 glibc (Move ≈ glibc 2.34 — avoid glibc ≥ 2.35 symbols like
`arc4random`). Fallback toolchain: `zig cc -target aarch64-linux-gnu.2.34`.

## Deploy & iterate

```bash
scripts/install.sh              # build + scp to move.local
scripts/install.sh --no-build
```

- Launch on device: **Shift+Vol+Jog-Click** → Tools → Looperino.
- Exit: Back (we call `host_exit_module()`), or the always-alive host escape
  **Shift + Volume-touch + Jog-click** (never consume those controls).
- **ui.js / module.json**: re-read on every module launch — exit + re-enter, no
  restart. **But only the entry ui.js is cache-busted**: any project file it
  `import`s stays cached until Move restarts → keep hot code in ui.js.
  (Host `shared/*.mjs` imports are fine — they're stable.)
- **dsp.so**: dlopen'd per launch — full exit (not suspend) + re-enter loads
  the new one.

## Debug

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'   # enable
ssh ableton@move.local 'tail -f /data/UserData/schwung/debug.log'    # watch (console.log lands here)
ssh ableton@move.local ': > /data/UserData/schwung/debug.log'        # truncate (never rotates)
```

Display mirror: http://move.local:7681 · Manager/file browser: http://move.local:7700
Never write to `/tmp` on the device (rootfs is full — crashes it); use `/data/UserData/`.

## M0 spike — how to run the latency test

1. Patch a 3.5mm cable from Move's **output** jack to its **input** jack
   (or, rougher: speaker on + mic, in a quiet room).
2. Launch Looperino, tap pad **2** in the bottom row (ping). It emits an
   impulse and counts frames until it re-appears on the input.
3. Display shows `lat: N ms` — that number is the full output→input round trip
   (includes the deliberate +1-block deferred render, ~2.9 ms, plus DAC/ADC).
4. Pad **1** toggles input monitoring (feedback-guarded: blocked while the
   speaker is active with no line-in cable), knob 1 = monitor gain.
   Pad **3** = 440 Hz test tone to verify the output path alone.
