# Looperino v1 — implementation spec

The concrete design the v1 code implements. DESIGN.md is *what*; this is *how*.
Every decision marked **[provisional]** is a call made to unblock the one-shot
build where DESIGN.md left an open question — each ships as a tweakable
constant or is trivially rewireable, and all of them are listed again at the
bottom for review with Arvid.

## Ownership split

**DSP (`src/dsp/*.c`, audio thread)** owns everything sample-accurate:
timeline/tempo/grid, clip state machines *including pad-gesture
classification* (pads arrive on the audio thread — the punch-in path),
record/overdub/undo layers, the input FX chain, drum sampler + step-sequencer
playback, synth voices, punch-in FX, chord engine (note transforms +
bar-quantized chord changes), metering.

**JS (`src/ui.js`)** owns everything visual and editorial: views, track
selection, the pad→role map, LEDs, display, encoders, step-button editing
(patterns + probability), chord navigation UI, menu, transport buttons,
feedback guard, save/load orchestration. JS never makes timing decisions.

## The pad→role map (how DSP interprets pads without knowing about views)

JS recomputes a 32-byte table (pads 68→99 in order) on every view/selection
change and pushes it blocking as `padmap=<64 hex chars>`. Byte = `type<<4 |
value`:

| type | meaning | value |
|---|---|---|
| 0 | NONE | — |
| 1 | CLIP | track(0-3)\*4 + slot(0-3) |
| 2 | DRUM | slot 0-15 of selected drum track |
| 3 | NOTE | grid index 0-15 (chord-aware pitch, see Chord engine) |
| 4 | FX | punch FX index 0-15 |

DSP acts on pad note-on/off by role. Race between a view switch and a pad hit
is bounded by one blocking param write — accepted.

## Views & controls

- 4×8 pads, left half / right half. **Views** (Left/Right buttons CC 62/63
  cycle): A = looper clips | instrument pads · B = looper clips | punch FX ·
  C = instrument pads | punch FX.
- **Track buttons** (CC 43/42/41/40 = tracks 1-4): select looper track 1-4.
  **Shift+track** selects instrument track 5-8 **[provisional]**. Pressing the
  selected track's button again deselects → **master** (locked decision #8).
  LED: warm color = looper bank, cool = instrument bank, bright = selected.
- **Encoders 1-8** (CC 71-78): track volumes 1-8. **Encoder 9 / master knob
  (CC 79)**: module master gain if events arrive (behavior on-device
  unverified). Knob 1 while **Menu** open: menu value.
- **Jog (CC 14)**: chord-journey navigation (rotate through legal moves;
  click CC 3 = commit at next bar). While a **step button is held**: per-step
  probability. While **Menu** open: menu navigation.
- **Play (CC 85)**: sequencer run/stop. **Shift+Play**: stop all clips.
- **Rec (CC 86)**: FX-lane live record arm **[provisional]**.
- **Sample (CC 118)** held + drum pad: arm threshold sampling into that slot.
- **Undo (CC 56)**: undo on last-touched clip. **Delete (CC 119)** held + clip
  pad: delete. **Copy (CC 60)**: not needed (hold-pad+pad copies) — unused v1.
- **Mute (CC 88)** held + track button: mute/unmute track.
- **Menu (CC 50)**: settings overlay (tempo mode/BPM, metronome, monitor,
  save/load set). **Back (CC 51)**: suspend to Move (loops keep playing);
  **Shift+Back**: full exit. Never touch the host escape combo.
- **Step buttons** (notes 16-31 in, LEDs CC 16-31): 16-step lane of the
  focused lane (see Sequencer).

## Timeline & tempo

- Global `timeline` frame counter runs from grid establishment; never stops.
  All positions derive from it. 4/4 assumed everywhere (v1).
- **First-loop mode (default)**: first recording is free-length; on end-tap,
  pick bars b ∈ {1,2,4,8,16} whose implied BPM is closest to `TARGET_BPM=100`
  clamped to [65,180] **[provisional guess rule]**; that sets `bar_frames`
  exactly (no audio rounding — the grid derives from the loop). Display shows
  the result; Menu can re-interpret ×2/÷2 afterwards.
- **Fixed-tempo mode**: BPM set in Menu; grid starts at first record/Play.
- Records after the grid exists: **start quantized to next bar** (pad blinks
  while armed), **end rounds to nearest bar** (tap early → runs to boundary;
  tap late → truncates, playback already in phase). `QUANT_START` is a
  constant so free-start can be tried by feel.
- Tempo is immutable once the grid exists (no time-stretch in v1). Clear-all
  resets the grid.
- Move's MIDI clock is parsed and displayed but does **not** drive the grid
  in v1 **[provisional]** — Looperino is self-contained; ext-sync is a later
  feature.
- Metronome click available from Menu (useful in fixed-tempo mode).

## Clips & gestures (per clip pad, classified in DSP, frame-timed)

Constants: `TAP_MS=250`, `HOLD_MS=330` (tweak by feel).

- **hold on empty clip** → record. Capture actually starts **at press**
  (scratch), so no audio is lost if it classifies as hold; discarded on tap.
- **hold on filled clip** → overdub **while held**, ends on release
  (punch-in; capture from press, kept only if hold). Initial record instead
  ends on **tap** (hands-free while singing) — the asymmetry is deliberate
  **[provisional]**.
- **tap** → toggle play/stop. Stop = immediate mute with 5 ms fade, restart =
  in-phase unmute (`(timeline − anchor) mod len`) **[provisional: immediate,
  not bar-quantized — Ableton-unmute feel]**. Launching a clip stops the
  row's other clip (exclusive per row).
- **double-tap** → undo: pop last overdub layer (toggle back = redo). On a
  fresh just-recorded clip: clears it.
- **triple-tap** → delete (no recovery v1).
- **hold + press another CLIP pad** → copy source→target (aborts the
  overdub the hold started). Allowed cross-row if target empty.
- Taps act **immediately** and are *reverted* if a second tap upgrades the
  gesture (stop→undo etc.) — no tap-window action latency **[provisional]**.

**Undo model**: playback = base + Σ layers (saturating). Overdubs record the
*added* audio into a layer buffer from a shared pool (`LAYER_POOL=6` buffers);
undo = O(1) drop, redo = O(1) re-add (until a new overdub replaces it).
Pool exhausted → oldest committed layer merges into its base amortized
(N frames per block); if nothing mergeable, overdub is denied (pad flashes).

**Memory**: 16 clips × `MAX_CLIP_SECONDS=30` stereo int16 ≈ 85 MB + 6-layer
pool ≈ 32 MB + samples/FX ≈ 13 MB → ~130 MB, memset-prefaulted at load
(one-time hiccup at module launch). Unproven footprint on device — first
thing to watch **[concern]**.

## Audio graph (per 128-frame block)

```
in(int16 st) → InputChain(preset of routed track) → wetIn
wetIn → recording clip (printed, Dub-FX model) + monitor mix (feedback-guarded)
looper track t: playing clip layers → trackbuf[t]
instr track  t: sequencer/live → drum sampler | synth → trackbuf[4+t]
punch FX (one live instance): processes its target trackbuf or master
master: Σ trackbuf·gain + monitor → soft-clip → out
```

- **One InputChain instance** (gate → comp → 3-band EQ → drive → delay →
  reverb), re-preset when the routed track changes; short crossfade, tails
  cut on switch **[provisional — CPU budget; 8 live chains won't fit]**.
  Presets per flavor: t1 kick+snare (tight gate, punch comp, low boost, no
  verb), t2 percussion (gate, comp, hp, slap delay), t3 bass (LPF, drive,
  comp, octave-ish sub via LPF'd square shaper — real pitch tracking is
  out of v1), t4 vocal/spacy (gate, comp, EQ, long reverb + dotted delay).
  Input routes to: selected looper track, else track 4 (the mic channel).
- **Instrument tracks are dry** (their character comes from the sampled
  material; no per-track reverb in v1) **[provisional — CPU]**.
- **Punch FX v1** (momentary while pad held, on selected track or master;
  one active instance, new press steals, tail rings out): stutter 1/16,
  stutter 1/4, filter sweep down, tape-stop, delay throw, duck/pump
  (beat-synced). FX index 0-5 on the right grid's bottom rows.
- **FX step lane**: 16 steps × fx-id, targets master **[provisional]**.
  Edited by holding an FX pad + pressing steps; live-recorded when Rec is
  armed and the sequencer passes steps while an FX pad is held.

## Sequencer (16 steps, one bar, 16ths)

- Runs when grid exists + Play on. Step = `bar_frames/16`.
- **Drum tracks**: 16 slots (right 4×4 = one kit). Focused slot = last
  played/sampled pad. Step buttons toggle the focused slot's steps;
  hold step + jog = probability 0-100%. Live pad hits always play.
- **Synth tracks**: step buttons toggle; hold step + note pad sets that
  step's pitch; steps default to chord root. Live play always.
- Pattern data lives in JS (editing) and is mirrored to DSP as
  `seq<t>=<hex>` params; DSP is the only clock consumer.
- One pattern per instrument track in v1 (no pattern clips yet)
  **[provisional — DESIGN's instrument-row-contents question]**.

## Chord engine (port of Omniphone's Tonal Map — see chord-journey.md)

- `TONAL_MOVE_TABLE` 56-node graph ported verbatim into `tonalmap.c`
  (credits: @briancalli.music, William R Thomas / @purpasteur).
- Current node + ≤4 legal moves on the display; jog rotates, click commits
  **at the next bar**. Chord lane: 16 bar-slots (`chordlane=` param), each
  holds until the next entry; recorded live when Rec armed **[provisional]**.
- **NOTE pads** (synth tracks): 16 ascending "core" pitches from the
  tonalMapFreqs voicing algorithm (chord tones + one passing tone, octave
  pairs) as MIDI notes — any neighbors are consonant. Re-laid on chord
  change.
- **Follow modes** per instrument track: 0 = ignore (drums default),
  1 = root-follow (transpose by root delta), 2 = chord-follow (nearest
  chord tone, smallest signed move; synth default). Applied at trigger
  time; already-sounding notes are left alone.

## JS ⇄ DSP protocol

Blocking writes (`host_module_set_param_blocking`) for anything ordered or
multi-write; fire-and-forget only for single hot values (gains, monitor).
One packed `status` string polled every 16 ticks (~30 Hz), < 480 bytes:

```
bpm=…;bar=…;grid=0/1;run=0/1;step=0-15;peak=…;mon=…;chord=<node>;
pend=<node|-1>;fx=<idx|-1>;lat=<denied-flags>;clips=<16×3 hex: state,pos,layers>;
kits=<4×4 hex: filled mask>;samp=<state>;focus=<slot>;
```

Clip state: 0 empty · 1 stopped · 2 playing · 3 armed · 4 recording ·
5 overdub · 6 rec-pending-end. `pos` = playhead in 16ths of its length,
capped 15 (drives the red→blue LED fade).

Params DSP←JS: `padmap`, `sel`, `gain=<t>:<v>`, `mute=<t>:<0/1>`,
`mon`, `allow_monitor`, `monitor_gain`, `tempo_mode`, `tempo`, `play`,
`stopall`, `metro`, `seq<t>`, `note<t>`, `fxlane`, `fxtarget`, `chord`,
`chordlane`, `follow=<t>:<m>`, `undo=<c>`, `delete=<c>`, `copy=<s>:<d>`,
`sample_slot=<t>:<s>`, `sample_cancel`, `octave`, `interp=<mul>` (×2/÷2
first-loop reinterpret), `save_clip=<c>:<path>`, `load_clip=<c>:<path>`,
`save_slot=<t>:<s>:<path>`, `load_slot=…`, `clear_all`.

Save/load: JS sequences per-clip/slot WAV writes through the DSP (SPI-thread
fopen — audible hiccup, so "save while not performing"), plus a set JSON via
`host_write_file`. **[v1 rough edge, by design]**

## LED language **[all colors provisional — tweak on device]**

- Clip pads: empty Black · stopped DarkGrey · armed blink-green (JS-timed) ·
  recording Red · overdub Orange · playing = 8-step ramp red→purple→blue by
  `pos` (indexed palette ramp constant `PLAYHEAD_RAMP`).
- Drum pads: filled = kit color, empty dim, trigger flash. Note pads: chord
  tones lit, root bright. FX pads: dim identity color, bright while active.
- Steps: on = track color, playhead step = white, prob<100 = dimmer.
- Buttons: Play green when running, Rec red when armed, Sample orange while
  arming, Mute/Delete lit while held-relevant.

## Provisional decisions to review with Arvid (the "open questions" calls)

1. Bank switch = Shift+track button; same-button-again = master.
2. 4 clips per looper row (4×4 left half), no paging.
3. Undo = layered, pool-backed, depth ~3/clip typical; redo = re-add last
   pop; triple-tap delete unrecoverable.
4. First-loop bar-count guess rule (closest-to-100-BPM); Menu ×2/÷2 fix-up.
5. Clip stop/start immediate (5 ms fade), not quantized.
6. Initial record ends on tap; overdub ends on release.
7. One input chain instance (selected track's preset), tails cut on switch.
8. Instrument tracks dry; no send FX in v1.
9. One live punch-FX at a time; FX lane is master-targeted, single lane.
10. One pattern per instrument track; drums edit via focused slot.
11. Chord navigation on jog wheel + display (no pads stolen); commits at bar.
12. External MIDI clock displayed but never drives the grid.
13. Save = WAVs via SPI-thread writes + JSON; audible glitch accepted.
14. ~74 MB preallocated buffers (`MAX_CLIP_SEC=16`). **Resolved 2026-07-07**:
    device has ~1.68 GB available, so footprint is a non-issue — `MAX_CLIP_SEC`
    can go back toward 30 freely; per-block CPU is the real budget.
