# Looperino — Design (v1)

Canonical spec. Decisions below were made by Arvid on 2026-07-06 in a scoping
Q&A; don't silently revisit them. Open questions are listed at the bottom —
those are fair game and should be decided by feel, on the device.

## Vision

The Move + a microphone as a complete solo live-performance rig, in the spirit
of Beardyman / Dub FX / Jacques / Marc Rebillet: build a track live from
nothing — beatbox or sample objects for drums, loop bass and chords, sing over
it — with deep enough performance controls to *jam hard*, and muscle-memory
knob functions everywhere.

Interaction ancestry (for taste, not cloning): Loopy Pro's loop lifecycle,
OP-Z's punch-in FX + step components + master/chord track, Ableton's clip rows.

## Locked decisions

### Signal path & audio
1. **Input**: built-in mic or 3.5mm line-in (both must work; USB mics are
   impossible — the Move's USB-A port is MIDI-only).
2. **Wet monitoring**: the performer hears the voice *through* its FX chain,
   and FX are printed into the loops (Dub FX model). This makes input →
   FX → output latency the project's #1 feasibility question → Milestone 0.
3. **Fully self-contained**: Looperino generates and loops all its own audio.
   No switching to stock Move instruments mid-set, no menu diving.

### Tempo & loop lifecycle
4. **First loop defines the tempo** by default (Loopy model). A pre-set fixed
   tempo mode also exists (needed for sequencer-first sets) — both possible,
   first-loop is the default.
5. **Recording ends on tap**, length rounds to the nearest bar.
6. **Multiple clips per track**, Ableton-style: a track's clips sit next to
   each other in its row; launching one stops the previous (exclusive per
   row). Easy copy from clip to clip, then layer over the copy.
   **Layered undo/redo**, including *belated* undo — peeling off overdub
   layers even after other actions happened (exact semantics: open question).

### Pads & gestures
7. **One pad per clip.** Gestures:
   - hold → record / overdub
   - tap → start / stop
   - double-tap → undo
   - triple-tap → delete
   - hold + press another pad → copy to that pad
   - LED shows loop position: red at loop start → fades to blue through the
     cycle. (Also needs states for: empty, armed, recording, stopped-has-audio,
     muted — see open questions.)
8. **Track buttons** select the active track. **Deselecting all tracks selects
   the master** — encoders and perf FX then target the master bus.

### Performance FX (right pad square)
9. **Momentary punch-in FX** (OP-Z model): active while held, on the selected
   track or master. Both *audio*-style (beat repeat, glitch, filter) and
   *MIDI*-style (retrigger/note-level, affecting instrument tracks' sequences).
   FX gestures are **step-sequenceable** with the 16 step buttons, OP-Z
   performance-track style.
10. **v1 FX set** (accepted as starting point, will be tweaked by feel):
    stutter/beat-repeat ×2 lengths, filter sweep, tape-stop/pitch drop,
    delay throw, duck/pump.

### Tracks
11. **8 tracks: 4 looper tracks + 4 instrument tracks.** Each instrument track
    is a drum kit *or* a synth. Page buttons switch pad views:
    - view A: loopers | instruments
    - view B: loopers | perf FX
    - view C: instruments | perf FX
12. **Preset track flavors** (looper side), each with a preset FX chain that
    makes its material sound good immediately:
    - track 1: kick + snare
    - track 2: percussion
    - track 3: bass
    - track 4: spacy / **vocal** — this *is* the mic channel (no separate 5th
      vocal channel; keep it simple).
13. **Vocal/spacy chain**: gate, compressor, EQ, reverb, delay. Pitch shift and
    harmonizer are wanted but are a bigger DSP lift — nice-to-have, later.

### Sequencer
14. **16-step sequencer** on the step buttons for the selected (drum) track.
    No sub-steps. **Per-step probability**: hold a step button and rotate the
    jog wheel to set a percentage.
15. **Start-of-set flow** this must serve: sample kick, snare, hat, bass one at
    a time (mic/line-in), each landing on its flavored track sounding good
    immediately, then sequence each on the 16 steps.

### Harmony
16. **Master chord/progression track**, OP-Z style: transposes instrument
    tracks over time; per-track follow/exclude flags. Chord *navigation*
    should be inspired by Omniphone's "chord journey" — playful, deep chord
    changes rather than a static progression lane.

### Meta
17. **Development happens on the device** — no desktop simulator (the feel
    lives in the hardware; don't spend time on one).
18. **Name: Looperino** (the old "Looperoni" spelling is dead — that's a
    different thing).

## Milestones

- **M0 — input/latency spike (GO/NO-GO)**: minimal overtake module that passes
  line-in/mic through a trivial DSP chain to the output; measure round-trip
  latency on device; verify a module can actually read the live input stream.
  Nothing else is built until this is answered.
- **M1 — one looper track**: record/overdub/play with the full pad gesture set,
  bar rounding, first-loop-sets-tempo, LED playhead.
- **M2 — 4 looper tracks**: track select, per-track preset FX chains, vocal
  chain (gate/comp/EQ/reverb/delay), mixer on encoders.
- **M3 — drum sampling + sequencer**: quick-sample flow → 16-step sequencing
  with per-step probability.
- **M4 — performance FX**: right-grid punch-in FX (v1 set), master vs track
  targeting, FX step-sequencing.
- **M5 — instrument tracks + chords**: synth track type, master chord track,
  chord-journey navigation, follow/exclude flags.
- **M6 — views & glue**: page-button views, copy flows, undo layers polish,
  set-level save/recall.

## Open questions (decide by feel / on device)

- 8 tracks vs 4 track buttons: how track selection pages between looper bank
  and instrument bank (shift? double-press? last-touched half?).
- Exact clip count per row (4 in a 4×4 half-grid is natural; more via paging?).
- Belated undo/redo semantics: per-clip layer stack depth, what "redo" means
  after new overdubs.
- LED language beyond the red→blue playhead fade (empty/armed/recording/muted),
  given LED batching limits (~8–20 updates per frame).
- Instrument-track row contents in view A (pattern slots, mirroring looper
  clips?).
- Where FX step-sequences live (per pattern? per view?) and how they're
  cleared/recorded live.
- Whether tempo can be changed after loops exist (time-stretch is likely out
  of scope for v1).

## Platform constraints (from research — see platform-research.md)

- Schwung overtake module = QuickJS (ES2023) UI/logic + native C/C++ DSP `.so`;
  audio fixed at 44.1 kHz, 128-frame stereo interleaved int16 (~2.9 ms blocks);
  realtime code must be non-allocating, non-blocking.
- 32 pads (notes 68–99, poly aftertouch), 9 endless encoders, jog wheel
  (CC 14 relative), 16 step buttons, 128×64 1-bit OLED, batched LED updates.
- Unverified at design time: measured round-trip input latency; module access
  to the live input stream (M0 exists to answer both).
- Device: Move firmware 2.0.5, Schwung v0.11.4 (as of 2026-07-06).
