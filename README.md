# Looperino

A standalone live-looping performance instrument for **Ableton Move**, built as an
overtake module for [Schwung](https://github.com/charlesvestal/schwung).

One performer, one Move, one mic, a lot of whacky objects to sample. Looperino
turns the Move into a self-contained looper + sampler + sequencer for live sets:
loop your voice through effects, sample and step-sequence drums, punch in
performance effects OP-Z-style, and ride chord journeys — no computer, no menu
diving.

## Status

Early development. The design is settled ([docs/DESIGN.md](docs/DESIGN.md)).

**M0 (input/latency spike) — done, GO.** Measured round-trip latency
(output → input, cable loopback) on device: **7.98 ms**. Comfortably low
enough for wet-monitored live vocals — locked decision #2 in DESIGN.md is
confirmed feasible. Next: M1, a single working looper track.

## Layout at a glance

- **4 looper tracks + 4 instrument tracks** (drum or synth), Ableton-style clip
  rows on the 4×8 pad grid, with page buttons switching views
  (loopers+instruments / loopers+FX / instruments+FX).
- **Right pad square: performance FX** — momentary punch-in effects (stutter,
  filter sweep, tape-stop, delay throw, duck…), targeting the selected track or
  the master bus, and step-sequenceable.
- **16 step buttons: sequencer** for sampled drums (kick/snare/hat/bass) and
  FX gestures, with per-step probability (hold step + turn wheel).
- **Preset track flavors**: 1 kick+snare · 2 percussion · 3 bass · 4 spacy/vocal
  (the mic channel lives here, wet-monitored through its FX chain).
- **Master chord track**: OP-Z-style progression that transposes instrument
  tracks, with playful "chord journey" navigation; tracks can opt out.

## Repo map

- [docs/DESIGN.md](docs/DESIGN.md) — the canonical spec: every locked decision, open questions, milestones.
- [docs/dev-notes.md](docs/dev-notes.md) — build, deploy, debug quick reference (start here to hack).
- [docs/schwung-module-guide.md](docs/schwung-module-guide.md) — source-verified deep dive: overtake module anatomy, full control map, DSP API, audio input path, MIDI clock recipe, realtime rules.
- [docs/toolchain.md](docs/toolchain.md) — cross-compilation setup (podman) and fallbacks.
- [docs/chord-journey.md](docs/chord-journey.md) — analysis of Omniphone's Harmonic Journey and the porting plan for Looperino's chord system.
- [docs/platform-research.md](docs/platform-research.md) — researched facts about the Move hardware, Schwung internals, and reference instruments (Loopy Pro / OP-Z / loop artists' rigs), with sources.
- `src/` — module source: `module.json`, `ui.js` (QuickJS UI), `dsp/looperino.c` (native DSP).
- `scripts/` — `build.sh` (cross-compile via podman), `install.sh` (deploy over SSH).
- `reference/` — local clones of Schwung + example modules for study (gitignored).

## Credits

Built on the [Schwung](https://schwung.dev/) framework by Charles Vestal and the
Move hacking community (lineage: bobbydigitales' move-anything). Unofficial;
not affiliated with or endorsed by Ableton.
