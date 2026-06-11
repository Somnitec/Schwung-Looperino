# Looperoni — architecture

## The two halves

Looperoni is one Schwung module with two cooperating parts:

```
   ┌─────────────────────────── ui.js (QuickJS, ~60 fps) ──────────────────────────┐
   │  reads pads/knobs/buttons (onMidiMessageInternal)                              │
   │  draws the 128×64 display (clear_screen/print/fill_rect)                       │
   │  sets pad + button LEDs (setLED / setButtonLED)                                │
   │  commands  ──► host_module_set_param("pad","5") / ("tempo","120") / ...        │
   │  polls     ◄── host_module_get_param("ui")  →  "120.0|1|0.37|5|4.0|0033..."    │
   └───────────────────────────────────────────────┬───────────────────────────────┘
                                                    │  (same module instance)
   ┌────────────────────────────── dsp.so (audio thread) ──────────────────────────┐
   │  set_param(): latch commands / params                                         │
   │  render_block(): read input @ host->mapped_memory + audio_in_offset,          │
   │                  record / overdub into per-loop buffers,                       │
   │                  sum playing loops + input monitor → stereo out               │
   │  get_param("ui"): serialize state for the UI                                  │
   └───────────────────────────────────────────────────────────────────────────────┘
```

The UI never touches audio; the DSP never touches the screen. They talk only
through `set_param` (UI→DSP, commands) and `get_param("ui")` (DSP→UI, state),
which the Schwung host routes to **this module's own plugin instance**
(`host_module_set_param` / `host_module_get_param` → `mm_set_param` in the host).

## Why an "overtake" module

Schwung module types: `sound_generator`, `audio_fx`, `midi_fx`, `tool`,
`overtake`. A grid looper wants the **whole pad grid, all the LEDs, and the
display** — that is exactly what an `overtake` module gets (it replaces Move's
UI until you exit with Shift + Volume-touch + Jog-click). So Looperoni is an
overtake module that *also* ships a `dsp.so` for the audio engine.

## DSP loop engine (`src/looperoni.c`)

- 32 independent loop cells, each a small state machine:
  `EMPTY → (ARMED) → REC → PLAY ⇄ OVERDUB / STOP`.
- The **first** recorded loop is the *master* and defines `master_len`
  (the cycle, optionally snapped to whole bars). Other loops, in sync mode,
  record exactly one cycle then auto-play, so everything stays phase-locked off
  the shared `clock` sample counter.
- Quantized transitions are latched (`pending`) on the UI thread and applied on
  the audio thread at the next bar/beat boundary, so the threads never race over
  buffer memory.
- Buffers are int16 interleaved stereo, allocated lazily on first record, capped
  at `max_sec` seconds (default 16).
- Tempo follows `host->get_bpm()` when a clock is present, else the UI's value.

### Command protocol (UI → DSP via `set_param`)

| key | value | meaning |
| --- | --- | --- |
| `pad` | `i` | pad press on loop *i* (engine decides the transition) |
| `clear` | `i` | clear loop *i* |
| `overdub` | `i` | toggle overdub on loop *i* |
| `stopall` / `playall` / `clearall` | `1` | transport-wide |
| `tempo` | `120.0` | BPM |
| `quant` | `0\|1\|2` | Free / Bar / Beat |
| `sel` | `i` | selected loop (for knob edits) |
| `vol` | `i:0.8` | per-loop gain (0–2) |
| `master_vol` | `0.9` | output gain |
| `monitor` | `1.0` | live-input monitor gain |
| `max_sec` | `16` | per-loop buffer ceiling |

### State readout (DSP → UI via `get_param("ui")`)

`bpm | quant | barPhase | sel | masterSec | <32 state chars>`
e.g. `120.0|1|0.3700|5|4.00|00332100000000000000000000000000`
where each state char is `0..5` = `EMPTY/ARMED/REC/PLAY/OVERDUB/STOP`, and
`barPhase` (0–1) drives the on-screen playhead.

## Open hardware-integration questions

These can only be settled on a real Move + Schwung install. They are the first
milestones in [PLAN.md](PLAN.md):

1. **Does the host call `render_block` for an `overtake` module's `dsp.so`, and
   route its output to the speakers/headphones?** Sound generators get this for
   free; overtake modules historically drove audio via an external engine (the
   RNBO runner launches `jackd`). If overtake DSP is *not* auto-rendered, the
   fallback is to ship the same engine as a `sound_generator`/`audio_fx` and run
   it through the Signal Chain, with the overtake UI as the controller.
2. **What feeds `audio_in`?** On the `linein` generator it is the physical
   line/mic input. Looperoni therefore loops *live input* by default. To loop
   Move's *own* output mix (your beats/synths), use the host sampler route
   (`host_sampler_set_source` + `host_preview_play`) — see PLAN milestone 4.
3. **Exact LED palette indices.** Move's colour bytes are a 0–127 palette;
   the constants we use (BrightGreen=8, BrightRed=1, …) come from Schwung's
   `constants.mjs` but should be eyeballed on-device and tuned.
4. **`host_module_set_param`/`get_param` reaching an overtake module's DSP.**
   Confirmed to exist as JS bindings and to call `mm_set_param`; verify the
   currently-loaded module *is* this one when the overtake UI is active.
