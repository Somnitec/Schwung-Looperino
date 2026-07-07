# Looperino v1 — on-device test checklist

The host-side logic test (`scratchpad/test_looperino.c`, run during the build)
covers the DSP state machine in isolation. Everything below can only be judged
on the Move. Work top-down; each milestone gates the next. Deploy with
`scripts/install.sh`, launch **Shift+Vol+Jog-Click → Tools → Looperino**, and
`ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'` +
`tail -f …/debug.log` to watch `console.log`.

Feel constants to reach for first (all in `src/dsp/looperino.h` unless noted):
`TAP_MS`, `HOLD_MS`, `QUANT_GRACE_MS`, `SEAM_XFADE`, `FIRSTLOOP_TARGET_BPM`,
`SAMP_THRESH`, `STOP_FADE_COEF`; LED colors + `PLAYHEAD_RAMP` in `src/ui.js`.

## 0. It loads and runs (M0-equivalent smoke)
- [ ] Module appears in the Tools/Overtake menu and launches without a
      "Loading…" hang. If it hangs: check `debug.log` for `Overtake DSP:` load
      errors and confirm `nm -D dsp.so | grep move_plugin_init_v2`.
- [ ] **Memory**: watch `free -m` over SSH right after launch — the ~74 MB
      preallocation (16 clips + 6 layers + 64 drum slots @ 16 s / 2 s) must not
      OOM the firmware. If it's tight, lower `MAX_CLIP_SEC`. **This is the #1
      risk.**
- [ ] Display shows "LOOPERINO / no grid", pads paint, no audio glitch on load
      beyond the one-time memset hiccup.
- [ ] Input monitoring: plug headphones, enable Monitor in Menu (or it force-
      enables while recording), speak — you hear the vocal chain (track 4
      preset by default). Feedback guard blocks it if speaker is on with no
      line-in cable.

## 1. One looper track (M1 — the real gate)
- [ ] **First loop sets tempo**: select track 1, press an empty left-half clip
      pad, sing/beatbox a bar or two, tap the pad to end. Display shows a bpm in
      65–180 and "grid". The loop plays back in time. If the guessed bar count
      is wrong, verify it feels right; the guess rule is closest-to-100-bpm.
- [ ] Loop seam is click-free (listen at the loop point). If it ticks, raise
      `SEAM_XFADE`.
- [ ] **tap** toggles play/stop (immediate, ~5 ms fade — should not click).
- [ ] **hold** on the filled clip overdubs while held; release commits; you
      hear the added layer on the next pass (not doubled live).
- [ ] **double-tap** undoes the last overdub; overdub again = redo path works.
- [ ] **triple-tap** deletes the clip.
- [ ] **hold + press another clip pad** copies audio to that pad.
- [ ] Recording into tracks 1/2/3/4 applies the right input-chain flavor
      (kick+snare / perc / bass / vocal) — they should sound distinctly
      shaped. Tune presets in `CHAIN_PRESETS` (`src/dsp/chains.c`).
- [ ] Quantized punch-in: with a grid established, arming a record waits for the
      next bar (pad blinks armed-green), and end rounds to the nearest bar.
- [ ] Exclusive-per-row: launching a second clip in the same track stops the
      first.

## 2. Four tracks + mixer (M2)
- [ ] Track buttons 1–4 select looper tracks; **Shift+track** selects instrument
      tracks 5–8; pressing the selected track again selects **master**.
- [ ] Encoders 1–8 ride track volumes; master knob (CC 79) rides output — verify
      CC 79 actually reaches the module and Move isn't eating it for its own
      volume (open question in the guide; if Move eats it, wire master to a
      different control).
- [ ] Mute (hold + track, or Mute on selected) mutes/unmutes.

## 3. Drum sampling + sequencer (M3)
- [ ] In view A or C, select an instrument track (drum type by default for
      tracks 5–6). Press **Sample**, then make a sound — threshold capture
      arms ("waiting…") and captures on the transient ("capturing"), landing on
      the focused slot. Tune `SAMP_THRESH` / `SAMP_STOP_LVL`.
- [ ] The captured slot's pad lights; pressing it plays the sample.
- [ ] Step buttons 1–16 toggle the focused slot's steps; **Play** runs the
      sequencer in time with the loops; the playhead step lights white.
- [ ] **Hold a step + turn jog** sets per-step probability (dimmer LED < 100%).
- [ ] Sample kick→snare→hat→bass into slots and build a beat (the start-of-set
      flow).

## 4. Performance FX (M4)
- [ ] View B/C right half = FX pads. Hold one — momentary effect on the selected
      track (or master if master selected): stutter 1/16, stutter 1/4, filter
      sweep, tape-stop, delay throw, duck. Release ends it (delay rings out).
- [ ] FX target follows selection; check master-targeted FX too.
- [ ] Tune ranges/lengths in `src/dsp/punchfx.c`.

## 5. Instruments + chords (M5)
- [ ] Set an instrument track to synth (Menu → Instr type). Its right-half pads
      play chord-aware notes; neighbors are consonant.
- [ ] Jog wheel navigates the chord journey (legal moves shown on display);
      jog-click commits the move at the next bar; instrument tracks following
      chord/root transpose accordingly.
- [ ] Verify the tonal-map moves feel musical (data ported from Omniphone).

## 6. Views, save/load, glue (M6)
- [ ] Left/Right cycle the three views; pad meaning + LEDs update.
- [ ] Back parks the module (loops keep playing in the background via
      `suspend_keeps_js`); re-entering repaints. Shift+Back fully exits.
- [ ] Save/load a clip or slot WAV (audible hiccup expected — "save while not
      performing"). Files under `/data/UserData/`.

## Known rough edges (by design in v1 — see docs/v1-spec.md)
- Overdub is DENIED when the 6-buffer pool or 4-layer/clip cap is full (pad
  won't start an overdub; no auto-merge yet).
- Stereo line-in collapses to mono through the input chain.
- External MIDI clock is displayed but does not drive the grid.
- Save writes WAVs on the SPI thread (brief glitch); clip save flattens the
  base only in v1 (committed layers are captured by copy, not yet by save).
