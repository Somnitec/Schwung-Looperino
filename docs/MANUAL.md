# Looperino — Manual

A live-looping instrument for the Ableton Move: loop your voice, sample and
sequence drums, punch in effects, ride chord changes — all on the device, no
computer. This manual grows as the instrument does; anything marked _(rough)_
is known to still need work.

> **State right now:** the looper core (record / play / overdub / undo) is the
> part to lean on. Recording is **clean/dry by default** so you hear exactly
> what you played — the flavored vocal/drum FX are a switch you turn on later
> once they're tuned by ear (see [Input FX](#input-fx)).

---

## Launch & exit

- **Launch:** Shift + Volume + Jog-click → Tools menu → **Looperino**.
- **Park it (keep loops playing):** **Back**. The module steps aside and your
  loops keep going in the background; open it again to pick up where you were.
- **Full exit:** **Shift + Back**. (The Move's own escape — Shift + Volume-touch
  + Jog-click — always works too.)

---

## The screen

Top line shows **LOOPERINO** and the tempo (or `no grid` before your first
loop). Below that: the selected track and current view, the current chord, the
transport state, and an input level meter along the bottom. While sampling a
drum it shows `SAMPLE: waiting…` / `capturing`.

---

## Your first loop (start here)

1. Plug in **headphones** (or the internal speaker — but then monitoring is
   muted to avoid feedback; headphones are better for singing along).
2. Press **Track 1** so a looper track is selected.
3. On the **left half of the pads**, each pad is a loop clip. Record with either
   gesture:
   - **Hold to record:** press and hold an empty pad, make your sound, **let go
     to finish**.
   - **Tap to start, tap to end:** tap an empty pad once to start, make your
     sound, tap the same pad again to stop.
4. Your **first loop sets the tempo.** Looperino rounds its length to a whole
   number of bars and locks the grid to it; the screen shows the resulting BPM.
   Everything you record after that snaps to this grid.
5. The loop plays back immediately. **Tap** it to stop, tap again to restart —
   always in time with the grid.

> If a fresh loop sounds like clicks instead of your voice, that was the old FX
> chain gating a quiet mic — it's off by default now. If it still happens, the
> input level may be very low; get closer to the mic or use line-in.

---

## Clip gestures (left-half pads)

Each looper track owns a **row** of clips. One clip per track plays at a time
(launching one stops the other in that row).

| Gesture | What it does |
|---|---|
| **Hold** empty pad | record, ends when you release |
| **Tap** empty pad, then tap again | record, ends on the second tap |
| **Tap** a recorded clip | play / stop (toggles) |
| **Hold** a recorded clip | overdub a new layer on top while held |
| **Double-tap** | undo the last overdub layer (double-tap again = redo) |
| **Triple-tap** | delete the clip |
| **Hold** a clip **+ press another clip pad** | copy it to that pad |

Notes:
- **Overdub** stacks layers you can peel back one at a time with undo, even
  after doing other things (up to a few layers per clip).
- Single **tap** to play/stop currently lags a beat behind your finger (up to
  a quarter second) because it's waiting to see if you meant a double-tap.
  _(rough — tunable; tell me if it feels sluggish.)_

---

## Tracks & views

- **4 looper tracks** (record live audio) + **4 instrument tracks** (sampled
  drums or a synth).
- **Track buttons 1–4** select a looper track. **Shift + Track** selects an
  instrument track (5–8). Press the **selected** track's button again to select
  the **master** (encoders/FX then act on the whole mix).
- **Left / Right** buttons cycle three pad layouts:
  - **A** — clips (left) · instruments (right)
  - **B** — clips (left) · performance FX (right)
  - **C** — instruments (left) · performance FX (right)

---

## Instruments: drums & synth (right-half pads)

Instrument tracks 5–6 are **drum kits** by default; 7–8 are **synths** (change
this in the Menu → Instr type).

**Sampling a drum:**
1. Select a drum track (Shift+Track 1 or 2), view **A** or **C**.
2. Press **Sample**, then make the sound. It arms (`waiting…`) and captures on
   the hit (`capturing`), landing on the last drum pad you touched.
3. Press that pad to play the sample.

**Sequencing:** the **16 step buttons** program the focused drum slot (or the
synth). Press steps to toggle them; press **Play** to run the sequencer in time
with your loops. **Hold a step + turn the jog wheel** to set that step's
probability (how often it fires).

**Synth pads** play chord-aware notes — neighbouring pads always sound good
together; they follow the current chord.

---

## Performance FX (right-half pads in views B / C)

Hold an FX pad for a **momentary** effect on the master mix; let go to stop.
The v1 set: **stutter (1/16)**, **stutter (1/4)**, **filter sweep**,
**tape-stop**, **delay throw**, **duck/pump**. _(rough — ranges/feel need
on-device tuning.)_

---

## Chords (the "chord journey")

The jog wheel walks a curated map of chord moves (ported from Omniphone) — you
can only go somewhere that sounds good. The screen shows the current chord;
**turn the jog** to preview a legal move, **click the jog** to commit it at the
next bar. Instrument tracks set to follow will transpose to the new chord.

---

## Transport & mixing

- **Encoders 1–8** — track volumes. **Master knob** — output volume.
- **Play** — run/stop the sequencer. **Shift + Play** — stop all clips.
- **Mute** — mute the selected track.
- **Menu** — settings: metronome, monitor on/off, instrument type, clear-all.

---

## Input FX (opt-in)

Recording is **dry by default** so loops sound like the source. Each looper
track has a "flavor" chain (Track 1 kick+snare, 2 percussion, 3 bass, 4 vocal
with reverb/delay) that prints its effect into the loop, Dub-FX style — but
these are **off until you enable them** (they need tuning by ear first). Turning
them on is a Menu item _(coming as they're dialled in)_.

---

## If something's wrong

- **Loop is silent:** input level too low, or you're on the speaker with
  monitoring muted — use headphones/line-in.
- **Nothing loads / hangs on "Loading…":** over SSH,
  `tail -f /data/UserData/schwung/debug.log` shows the reason.
- Full device test checklist for the developer: [on-device-tests.md](on-device-tests.md).
