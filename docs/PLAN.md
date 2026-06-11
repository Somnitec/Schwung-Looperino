# Looperoni — implementation plan

A staged plan to take this scaffold to a gig-ready looper. Each milestone is
small and independently testable on the device. The **first three milestones
are about de-risking** the one thing we can't verify off-hardware: whether an
overtake module's DSP is rendered by the host.

---

## Milestone 0 — Build pipeline (no hardware needed) ✅ scaffolded

- [x] `module.json`, `ui.js`, `src/looperoni.c`, vendored ABI header.
- [x] `scripts/build.sh` (aarch64 cross-compile → `dist/looperoni/`).
- [x] `scripts/Dockerfile`, `scripts/deploy.sh`, GitHub release workflow.
- [ ] **Do:** install an aarch64 toolchain and confirm a clean compile:
  ```bash
  # Linux:  sudo apt-get install gcc-aarch64-linux-gnu
  # macOS:  brew tap messense/macos-cross-toolchains
  #         brew install aarch64-unknown-linux-gnu
  ./scripts/build.sh
  file dist/looperoni/dsp.so      # -> ELF 64-bit LSB shared object, ARM aarch64
  ```
- [ ] Fix any warnings (`-Wall -Wextra`); the engine is plain C99 + libm.

## Milestone 1 — Does the takeover even render audio? (critical spike)

Goal: prove the host calls `render_block` on an overtake module's DSP and routes
its output to the speakers.

- [ ] Temporarily make `render_block` emit a quiet sine/test tone regardless of
      input. Build, `./scripts/deploy.sh`, launch Looperoni from the Schwung menu.
- [ ] **If you hear the tone:** the overtake-DSP architecture works — proceed to
      Milestone 2.
- [ ] **If silent:** switch architecture (see *Fallback A* below) before going
      further. This is the single biggest unknown; do it first.

**Fallback A — Signal Chain engine.** Ship the engine as a `sound_generator`
(or `audio_fx`) with `"chainable": true` instead of (or in addition to) the
overtake. The overtake UI then loads it into a shadow slot and talks to it with
`shadow_set_param(slot, "looperoni:pad", "5")`. Mechanically the same engine;
only the host wiring and the param-bridge call change.

## Milestone 2 — Live-input looping (the core)

- [ ] Confirm `audio_in` carries the line/mic input (plug a guitar / line source).
- [ ] Free mode (`quant=0`): record one pad, commit, hear it loop; layer a second
      pad; `Delete`+pad clears.
- [ ] Verify the input monitor (Knob 2) and master volume (Master knob).
- [ ] Tune buffer ceiling vs. RAM: `max_sec` default 16 → ~2.8 MB/loop; confirm
      32 simultaneous loops don't pressure memory (lower the default if needed).

## Milestone 3 — Tempo sync & quantize

- [ ] Bar mode: first loop becomes the master and snaps to whole bars; armed pads
      start on the downbeat; layered loops stay phase-locked.
- [ ] Cross-check `host->get_bpm()` against Move's transport; decide whether
      Looperoni should *follow* Move's clock or own its own tempo (UI already
      supports both — Up/Down and Shift+Jog set BPM).
- [ ] Visual playhead (`barPhase`) lines up audibly with the downbeat.

## Milestone 4 — Resample Move's own output (the killer feature)

Loop the beat/synths you're already making on Move, not just live input.

- [ ] Investigate `host_sampler_*` JS bindings: `host_sampler_set_source`,
      `host_sampler_start/stop/pause/resume`, `host_sampler_get_samples_written`,
      `host_preview_play/stop`. These already record Move's audio to disk.
- [ ] Add an "input source" toggle: **Live in** (DSP path, M2) vs **Move mix**
      (host-sampler path). For Move-mix, a pad records via the host sampler to a
      wav, then loops it through the engine (load wav into a loop buffer) or via
      `host_preview_play`.
- [ ] Reconcile latency/offset so sampled loops land on the grid.

## Milestone 5 — Loopy-style depth

- [ ] **Overdub undo/redo:** keep a one-deep pre-overdub copy per loop; map to
      Shift+Record. (Engine: add a `shadow` buffer + `undo:i` command.)
- [ ] **Per-loop pan**, and a second knob bank (Shift+Knob) for pan/feedback.
- [ ] **Loop groups / mute groups:** step buttons (notes 16–31) select which of
      4 groups the pad bank shows, à la scenes — 4 banks × 32 = 128 loops.
- [ ] **Fade on overdub / loop decay** (feedback < 1.0) for ambient layering.
- [ ] **Reverse / half-speed** per loop (cheap: step pos backwards / by 0.5).
- [ ] **Clear-all confirm** and a "panic" stop.

## Milestone 6 — Persistence & polish

- [ ] Save/load a loop set to `/data/UserData/schwung/...` (buffers as wav +
      a json manifest of states/lengths/tempo).
- [ ] Honour `settings-schema.json` defaults at `init` (read `config.json`).
- [ ] On-device `help.json` review; tune the LED palette on real pads.
- [ ] Metering on the display (per-loop level from the engine).
- [ ] First tagged release `v0.1.0` → CI publishes `looperoni-module.tar.gz`;
      add the entry to Schwung's `module-catalog.json` (PR to charlesvestal/schwung)
      so it appears in the on-device module store.

---

## Dev loop cheatsheet

```bash
./scripts/build.sh                              # cross-compile + stage dist/
MOVE_HOST=ableton@move.local ./scripts/deploy.sh   # push to device
ssh ableton@move.local                          # tail logs while testing
# On the Move: Schwung menu -> Looperoni. Exit: Shift + Volume-touch + Jog-click.
```

## Decisions worth your input

- **Default loop source** — live input (shipping default) vs. Move's output mix.
  M2 is live-input; M4 adds the mix. Which matters more to you first?
- **Tempo authority** — should Looperoni follow Move's transport/clock, or be the
  master clock? (Affects whether you sequence on Move *and* loop simultaneously.)
- **Grid meaning** — 32 independent loops (current), or fewer loops with
  banks/scenes for more tracks? Loopy-style groups are M5.
