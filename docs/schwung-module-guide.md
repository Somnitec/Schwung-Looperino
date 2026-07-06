# Looperino Implementation Guide — Schwung Overtake Module (target host v0.11.4)

All claims below were verified against local source in `reference/`. Where the shipped docs disagree with source, **source wins and the disagreement is called out**. Paths are abbreviated: `schwung/` = `reference/schwung/`, `twin/` = `reference/schwung-twinsampler/`, `magneto/` = `reference/magneto-move/`.

---

## 0. The one architectural fact everything else follows from

On device, Schwung is **not** a host that replaces Move. It is `schwung-shim.so`, LD_PRELOADed into Move's stock firmware, intercepting the SPI mailbox ioctl (`schwung/docs/ARCHITECTURE.md`). All module **JS** runs inside a separate long-lived QuickJS process, `shadow_ui` (`schwung/src/shadow/shadow_ui.c`); all module **DSP** (`dsp.so`) is dlopen'd **inside the shim** and runs on Move's SPI/audio thread. The standalone `schwung_host.c` binary is built but *never invoked on device* (`docs/API.md:8-15`, `docs/ARCHITECTURE.md:50-54`) — any API.md binding marked "schwung_host only" is unreachable for us.

An overtake module = **JS UI in shadow_ui** (full control of display + LEDs + all MIDI) + **optional dsp.so in the shim** (audio render + audio-thread MIDI). Looperino wants both.

---

## 1. Anatomy of an overtake module

### 1.1 module.json — the fields that matter for Looperino

TwinSampler's manifest (`twin/twinsampler/module.json`) is the closest template:

```json
{
  "id": "looperino",
  "name": "Looperino",
  "abbrev": "LOOP",
  "version": "0.1.0",
  "ui": "ui.js",
  "dsp": "dsp.so",
  "api_version": 2,
  "component_type": "overtake",
  "capabilities": {
    "audio_in": true,
    "audio_out": true,
    "midi_in": true,
    "raw_ui": true,
    "claims_master_knob": false,
    "suspend_keeps_js": true
  }
}
```

- Parsed by a **minimal JSON reader**: double-quoted keys, lowercase booleans, no comments, no trailing commas, ≤ 8 KB (`docs/MODULES.md:44-45`, skill `references/manifest.md`).
- `component_type: "overtake"` — makes the module appear in the Overtake/Tools menus and gets the overtake lifecycle. The overtake scanner (`shadow_ui.js:2834-2905, scanForOvertakeModules`) accepts it top-level **or** inside `capabilities`, and scans `modules/` **plus one level of subdirectories**, so any category folder works; the convention is `modules/overtake/<id>/`.
- `dsp`: relative filename; shadow_ui builds `basePath + "/" + dsp` and asks the shim to dlopen it (`shadow_ui.js:3473-3477`). Unlike chain audio_fx, the name is **not** constrained — `dsp.so` is fine.
- `api_version: 2` — the overtake DSP loader is **v2-only** (`schwung_shim.c:1318-1321`), see §3.
- Capabilities relevant to us (readers annotated):
  - `suspend_keeps_js` (read in `shadow_ui.js`): pressing Back/suspending parks the module; its JS `tick()` **keeps firing in the background** with display/LED bindings no-op'd (`shadow_ui.js:13997-14045`), and the DSP **stays loaded and rendering** ("Do NOT unload overtake DSP", `shadow_ui.js:3160`). This is how Looperino keeps loops playing while the user works in Move. Full exit = Shift+Back (docs) / module-driven `host_exit_module()`.
  - `button_passthrough: [85]` (read in `schwung_shim.c` + `shadow_ui.js:3600-3620`): CCs listed here still reach **Move firmware** while the overtake is active, and the host LED-clear skips them. Use it if you want Move's own Play button to keep driving Move's transport (which also keeps MIDI clock running — see §5).
  - `skip_led_clear`: skip LED-clear ceremony, keep Move's pad colors (not what a looper wants).
  - `claims_master_knob`: only parsed by the standalone host's `module_manager.c` (`docs/MODULES.md:86-91`); on-device relevance is minimal — leave false.
  - `raw_midi`, `requires_continuous_processing`: `requires_continuous_processing` is read **only** by `chain_host.c` (`docs/MODULES.md:79`, `chain_host.c:311-330`) for chain FX idle-parking. The overtake DSP path renders **unconditionally every frame** while loaded (`schwung_shim.c:1796` has no idle gate), so Looperino does *not* need it unless you also ship a chainable variant.
  - `requires_path`: module is hidden from the menu if the path doesn't exist (`shadow_ui.js:2853-2856`).

### 1.2 File layout and install path

```
/data/UserData/schwung/modules/overtake/looperino/
  module.json
  ui.js           # entry point — thin wrapper (see TwinSampler pattern §8)
  ui_core.js      # optional split; NOTE the import-caching pitfall in §6
  dsp.so          # aarch64 shared object
  help.json       # optional, auto-discovered Help viewer content
```

Launch: Shift+Vol+Jog-Click (or Shift+Vol+Step13) → Tools menu → module under the "Overtake" divider.

---

## 2. The JS runtime

### 2.1 Lifecycle and the tick-rate discrepancy — resolved from source

Hooks captured by the loader (`shadow_ui.js:3577-3592`): `init`, `tick`, `onMidiMessageInternal`, plus optional `onUnload` and `onResume`. `onMidiMessageExternal` is dispatched from the same global set. The loader saves shadow_ui's own globals, evaluates your ui.js, diffs the globals, and captures yours — so just assign `globalThis.init = ...` etc.

**Tick rate.** Docs disagree: `docs/API.md:85` says "~344 ticks/sec (audio block)", `docs/MODULES.md:414` says "~60fps", root `CLAUDE.md` says "~44x/sec". **Source** (`shadow_ui.c:2638-2644`):

```c
if (shadow_control && shadow_control->overtake_mode >= 2) {
    usleep(2000);   /* ~500 Hz effective (minus tick work) */
} else {
    usleep(16000);  /* ~60 Hz for normal shadow UI */
}
```

So: **an active overtake module's `tick()` runs at up to ~500 Hz** (2 ms sleep between iterations, minus your tick's own cost — the loop is sleep-paced, not deadline-paced); everything else (chain shims, menus, and your module while it's *suspended*) runs at ~60 Hz. None of the three documented numbers is right for our case; never derive wall time from tick counts — use `Date.now()`. (Corollary: the "~500 ms init delay" of `OVERTAKE_INIT_DELAY_TICKS = 30 // ~500ms at 16ms tick` at `shadow_ui.js:612` is computed with the 16 ms assumption; in overtake mode 30 ticks can pass in ~60 ms + LED-clear time.)

Overtake load sequence (`shadow_ui.js:3440-3660`): `shadow_set_overtake_mode(2)` → LED queue activated → **DSP loaded first** (`shadow_set_param(0,"overtake_dsp:load", path)`) → param shims installed → `shadow_load_ui_module(uiPath)` → callbacks captured → LEDs cleared progressively (20/batch, `clearLedBatch`, `shadow_ui.js:615-665`) with "Loading..." on screen → `init()` → run. Exit: host escape **Shift (CC49) + Volume-touch (note 8) + Jog click (CC3)** always works; module-driven exit calls `host_exit_module()` (installed for overtake modules at `shadow_ui.js:3509-3517` — the "tools only" warning in the skill does not apply here).

`onUnload()` fires on teardown (`exitOvertakeMode` → `invokeModuleOnUnload`); `onResume()` fires when a suspended module is re-foregrounded (LEDs were cleared while backgrounded → invalidate your LED cache and repaint; the host also replays its LED snapshot, `shadow_ui.js:3204-3205`).

### 2.2 MIDI in: the full control map (from `schwung/src/shared/constants.mjs`)

`onMidiMessageInternal(data)` receives `[status, d1, d2]` from cable 0. In overtake mode 2 the shim forwards **all** cables/events to shadow_ui (`schwung_shim.c:6784-6815`); cable 2 (USB-A) arrives at `onMidiMessageExternal`.

**Notes (cable 0):**

| Control | Notes |
|---|---|
| Pads (4 rows × 8, bottom-left → top-right) | **68–99** (`MovePad1=68`, `MovePad32=99`) |
| Step buttons 1–16 | **16–31** |
| Knob capacitive touch 1–8 | **0–7** (on=127) |
| Master-volume knob touch | **8** (`MoveMasterTouch`) |
| Jog wheel touch | **9** (`MoveMainTouch`) |

**CCs (cable 0):**

| Control | CC | Notes |
|---|---|---|
| Jog click | **3** | 127 = press (no LED) |
| Jog turn | **14** | relative; see delta encoding below |
| Step-icon LEDs | 16–31 | **LED-only** CC addresses (input comes as notes) |
| Track buttons | **43=Track1, 42=Track2, 41=Track3, 40=Track4** (reversed!) | RGB LEDs |
| Shift | **49** | 127 held |
| Menu / Back / Capture | **50 / 51 / 52** | white LEDs |
| Down / Up | **54 / 55** | |
| Undo / Loop / Copy | **56 / 58 / 60** | |
| Left / Right | **62 / 63** | |
| Knobs 1–8 | **71–78** | relative encoders (also the knob-LED indicator addresses) |
| Master volume knob | **79** | relative encoder, **no LED** (9th encoder) |
| Play / Rec | **85 / 86** | RGB LEDs |
| Mute | **88** | |
| Knobs 1–8 absolute | 102–109 | capture-rule path only, not normally seen |
| Mic/line-in plug detect | 114 | 0 = mic, 127 = line-in (XMOS) |
| Speaker/line-out detect | 115 | 0 = speaker, 127 = line-out |
| Sample (a.k.a. Record) | **118** | RGB LED |
| Delete | **119** | |

**Encoder encoding & batching.** Raw hardware: CW = 1..63, CCW = 65..127 per detent. In overtake mode the framework **accumulates** knob (71–78) and jog (14) deltas and flushes them as **one synthetic CC per knob per tick before `tick()`** — encoded CW = count (1..63), CCW = 128−|count| (65..127) (`shadow_ui.js:15345-15359` accumulate; `14940-14975` flush). Decode with `decodeDelta(value)` from `shared/input_filter.mjs` (returns the signed count). CC 79 and all buttons are forwarded immediately, not batched. Filter noise with `shouldFilterMessage()` (drops 0xF8 clock, sysex edges, aftertouch, touch notes) — but note that means JS won't see MIDI clock; clock belongs to the DSP (§5).

**Pads reach the DSP directly on the audio thread**: in overtake mode 2, cable-0 note-on/off with note ≥ 10 is delivered to your DSP's `on_midi(inst, msg, 3, MOVE_MIDI_SOURCE_INTERNAL)` from the SPI path (`schwung_shim.c:6801-6810`) — *in addition to* the JS delivery. This is the low-latency punch-in path: quantize/act on pad hits at block accuracy in C, and treat the JS copy as UI-only. Capability sentinel: `shadow_inbound_pad_midi_active()`. CCs (Play/Rec buttons etc.) do **not** get this treatment — they only reach JS; forward what the DSP needs via params (or have the DSP react to its own transport params).

**MIDI out from JS:** `move_midi_internal_send([cin, status, d1, d2])` (LEDs — but see LED queue below), `move_midi_external_send([...])` (USB-A), `move_midi_inject_to_move([...])` (into Move firmware's input — cable nibble 0 = simulate surface, 2 = route to Move tracks by channel; see `docs/ADDRESSING_MOVE_SYNTHS.md`).

### 2.3 LEDs

Palette: **indexed 0–127 RGB** for pads/steps/Play/Rec/Sample; the rest are white LEDs taking brightness 0–127. Canonical values in `constants.mjs` (header comment maps every index to hex): `Black=0, BrightRed=1, OrangeRed=2, BrightOrange=3, VividYellow=7, BrightGreen=8, NeonGreen=11, Cyan=14, AzureBlue=15, Purple=22, NeonPink=23, White=120, LightGrey=118, DarkGrey=124, PureBlue=125, Green=126, Red=127`; white-LED levels `WhiteLedOff=0, WhiteLedDim=0x10, WhiteLedMedium=0x40, WhiteLedBright=0x7c`. There are also **tempo-synced LED animation codes** (`Trans/Pulse/Blink × 24th/16th/8th/4th/2th`, `constants.mjs:634-649`) driven by Move — potentially free beat-blinking for a looper (untested in overtake mode).

Setting: `setLED(note, color)` (note message, cin 0x09) and `setButtonLED(cc, color)` (CC message, cin 0x0b) from `shared/input_filter.mjs:66-77` — both cache last value and skip duplicate sends. **Batching:** while an overtake module runs, `move_midi_internal_send` is intercepted by a last-writer-wins queue flushed at **16 LED messages per tick** (`LED_QUEUE_MAX_PER_TICK`, `shadow_ui.js:672-733`) — so you can be sloppy in JS; the queue absorbs bursts. The raw hardware constraint the queue protects: MIDI_OUT region holds 20 packets/frame, ~64-packet SHM buffer, >60/frame overflows (`docs/API.md:414-418`). Progressive init (≤ 8–20/frame) is still the documented pattern for first paint.

### 2.4 Display

128×64 1-bit. Globals registered in shadow_ui (`js_display.c:636-656`): `clear_screen()`, `print(x,y,text,color)`, `set_pixel`, `draw_rect`, `fill_rect`, `draw_line`, `fill_circle`, `draw_image`, `text_width(text)`, `set_font(name)`, `get_font_height()`. Rendering: your draws mark the buffer dirty; the shadow_ui loop packs it to SHM when dirty (or every 30 ticks) and the shim composites it onto Move's display (`shadow_ui.c:2632-2636`). Menu helpers (`shared/menu_layout.mjs` etc.) are importable with absolute paths `/data/UserData/schwung/shared/...`.

### 2.5 JS ⇄ DSP parameter bridge (as installed for overtake modules)

`loadOvertakeModule` installs (`shadow_ui.js:3483-3506`):

- `host_module_set_param(key, val)` → `shadow_set_param(0, "overtake_dsp:"+key, String(val))` — **fire-and-forget while overtake_mode ≥ 2** (`shadow_ui.c:713`): back-to-back writes can clobber each other in the single SHM slot.
- `host_module_set_param_blocking(key, val, timeoutMs)` → `shadow_set_param_timeout(...)` — waits for the shim to consume; use for critical ordered writes (TwinSampler wraps everything important in this, `twin/twinsampler/ui_chain.js:917-936`).
- `host_module_get_param(key)` → `shadow_get_param(0, "overtake_dsp:"+key)` — blocking, 200 µs poll, 100 ms default timeout (`shadow_ui.c:661-708`). Each call round-trips to the SPI thread — **cache values, don't poll dozens of keys per tick**.

Shim side, `overtake_dsp:*` requests are dispatched in `shim_handle_param_special` (`schwung_shim.c:3403-3445`): `load`/`unload` are magic, everything else goes to your plugin's `set_param`/`get_param`. **These execute on the SPI thread** — same "accepted hiccup" rule as chain patch loads (`docs/REALTIME_SAFETY.md:112-118`). Keep `set_param` cheap; anything heavy (WAV export/import) will glitch audio unless you hand it to your own background thread.

Also available to overtake JS: `host_read_file`, `host_write_file`, `host_ensure_dir`, `host_file_exists`, `host_system_cmd`, `host_open_text_entry` (installed at `shadow_ui.js:3520-3550`), plus the shadow_ui global set from `docs/API.md` (`host_speaker_active()`, `host_line_in_connected()`, `host_sampler_*`, `shadow_set_overtake_mode`, `shadow_request_exit`, ...).

---

## 3. The native DSP side

### 3.1 How an overtake dsp.so is loaded — and which struct to implement

`shadow_overtake_dsp_load` (`schwung_shim.c:1421-1533`): dlopen(RTLD_NOW|RTLD_LOCAL), then

1. try **`move_plugin_init_v2`** → `plugin_api_v2_t` ("generator" mode — you render your own audio into a buffer the shim mixes in), else
2. try **`move_audio_fx_init_v2`** → `audio_fx_api_v2_t` ("FX" mode — you process the **Schwung ME bus in-place**; note this is *Schwung's* internal audio, not Move's output, except under Link-Audio rebuild — `schwung_shim.c:2394-2411`).

**Looperino should be a v2 generator.** v1 is not probed at all on this path.

Structs (authoritative header `schwung/src/host/plugin_api_v1.h`):

```c
typedef struct plugin_api_v2 {
    uint32_t api_version;                      /* = 2 */
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void  (*destroy_instance)(void *instance);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int   (*get_error)(void *instance, char *buf, int buf_len);
    void  (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);
```

(The `audio_fx_api_v2_t` variant swaps `render_block` for `process_block(inst, int16_t *audio_inout, int frames)` and drops `get_error` — `src/host/audio_fx_api_v2.h`.)

`host_api_v1_t` (same header) — fields **actually populated for the overtake DSP** (`schwung_shim.c:1448-1459`; struct is memset(0) first):

| Field | Value for overtake DSP |
|---|---|
| `sample_rate` / `frames_per_block` | 44100 / 128 |
| `mapped_memory` | shadow SPI mailbox base |
| `audio_out_offset` | 256 (`MOVE_AUDIO_OUT_OFFSET`) — Move's outgoing audio (readable for resampling) |
| `audio_in_offset` | **2304** (`MOVE_AUDIO_IN_OFFSET` = 2048+256) — **line-in/mic input** |
| `log` | `shadow_log` (not RT-safe — never from render) |
| `midi_send_internal` | routes into chain slots' MIDI (`overtake_midi_send_internal`, shim:1325) |
| `midi_send_external` | **audio-thread-safe async ring** to USB-A (shim:1338-1418; sentinel `shadow_overtake_send_external_async_active()`) |
| `get_bpm` | `shim_get_bpm` → `sampler_get_bpm` fallback chain (§5) |
| `midi_inject_to_move` | inject into Move MIDI_IN as cable-0 (≤ 8 packets/tick) |
| `get_clock_status`, `mod_*`, `slot_recv_channel` | **NULL** on the overtake path — null-check before use |

MIDI `source` constants: `MOVE_MIDI_SOURCE_INTERNAL 0`, `EXTERNAL 2`, `HOST 3`, `FX_BROADCAST 4`.

What arrives at your DSP `on_midi`:
- **len 3, source INTERNAL**: cable-0 pad/step note-on/off (note ≥ 10) — audio-thread pad hits (`schwung_shim.c:6801-6810`).
- **len 1, source EXTERNAL**: system realtime **0xF8/0xFA/0xFB/0xFC from cable 0** (Move's own transport clock, delivered regardless of the user's "MIDI Clock Out" setting — `schwung_shim.c:1196-1214`).
- **len 3, source EXTERNAL**: external USB (cable-2) channel messages seen in MIDI_OUT (`schwung_shim.c:1304-1311`).

### 3.2 Audio path and latency

Your `render_block` is called **post-ioctl** into a deferred buffer used on the *next* frame (`shadow_inprocess_render_to_buffer`, `schwung_shim.c:1577-1839`) — deliberate +1 block (~2.9 ms) so Move's pad handling stays fast. Output is then mixed at unity into the ME bus and summed with Move's mailbox after master-FX (`schwung_shim.c:2357-2371`), scaled by Move's master volume at the DAC. Format: stereo interleaved int16, `[L0,R0,...,L127,R127]`, always 128 frames.

### 3.3 How JS gets recorded audio to/from the DSP (TwinSampler's answer)

There is **no shared-memory sample-upload path between JS and DSP**. The contract is: **DSP owns the audio data and the filesystem; JS passes file paths and trigger params as strings.**

- Record: UI sets `record_output_dir`, `record_max_seconds`, target params, then `record_start=1` (blocking writes) — `twin/ui_chain.js:3756-3782`. The DSP core captures from the mailbox input region during `render_block` and writes the WAV itself; the UI then polls `get_param("last_recorded_path")` until a new path shows up (`twin/ui_chain.js:3656-3699`) and, to load it onto a pad, sends the path back: `slot_sample_path` = `"sec:bank:slot:/path/to.wav"` (`twin/ui_chain.js:2733`).
- Magneto does the same shape from the chain side: `save_recs`/`load_a` filepath params; WAVs written with plain `fopen/fwrite` from `set_param` context (`magneto/src/dsp/magneto.c:719-736`) — i.e. on the SPI thread, accepted glitch.

For Looperino: keep loop buffers as preallocated int16 arrays inside the DSP instance (Magneto preallocates 30 s × 2 sides ≈ 10 MB; `magneto.c:62-64`), expose transport/loop state via `get_param` for the UI (single packed status string beats many keys — Magneto packs `varspeed=..;speed=..;...`), and use path-passing for save/load.

---

## 4. AUDIO INPUT — how a module reads live line-in/mic

**The API is: `(int16_t*)(host->mapped_memory + host->audio_in_offset)` — 128 stereo interleaved int16 frames of the current block's captured input, read inside `render_block`.** Evidence:

- `schwung/src/modules/sound_generators/linein/linein.c:962`: `int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);` then per-frame reads at `:1010-1011`. (`MOVE_AUDIO_IN_OFFSET = 2048+256 = 2304`, `plugin_api_v1.h:19`; region documented as "Audio IN: 128 stereo int16" at RX offset 2304, root `CLAUDE.md` SPI table.)
- `twin/twinsampler/dsp_wrapper_monitor.c:2605` (capture) and `:2701` (monitor) read the same pointer.
- **Overtake-specific guarantee**: right before calling the overtake DSP's `render_block`, the shim re-copies the *raw hardware* AUDIO_IN region into the shadow mailbox "so overtake plugins can read line-in" even when the resample bridge has overwritten it (`schwung_shim.c:1797-1821`). This is the strongest evidence that mailbox-input reads are the sanctioned overtake input path.
- Magneto, being a **chain audio_fx**, does *not* read the mailbox: it records `audio_inout` — the chain signal at its slot — and its docs state: "Records `audio_inout` (the chain signal at this slot). To loop **external** audio, place the stock **Line In (LI)** sound generator in the synth slot upstream" (`magneto/CLAUDE.md:9-10`). Looperino as an overtake generator skips that indirection entirely.

**Input source selection**: none in Schwung. "Audio input routing depends on the last selected input in the stock Move interface" (`docs/API.md:483`, `docs/MODULES.md:1554`). Plug state is queryable: JS `host_line_in_connected()` (XMOS CC 114) / `host_speaker_active()` (CC 115); no gain control API — linein.c implements trim/gate in DSP.

**Monitoring/passthrough**: yes — sum `audio_in` into your `render_block` output, exactly like TwinSampler's monitor (`dsp_wrapper_monitor.c:2698-2710`: `out[i] += audio_in[i] * gain`). Latency = the deferred-render block (+~2.9 ms) plus device I/O; measure in M0. **Feedback**: the framework's feedback gate covers only chain Line-In slots (`CLAUDE.md` Feedback Protection; `loadOvertakeModule` has no gate) — Looperino must implement its own guard with `host_speaker_active() && !host_line_in_connected()` before enabling monitoring, or ship monitor-off by default.

**Recording Move's own output (resampling)** is also possible: TwinSampler reads `host->audio_out_offset` as the "host bus" and can capture input, bus, or a mix (`dsp_wrapper_monitor.c:2596-2686` — includes its input-region swap trick to feed a mixed signal to a capture core that only reads AUDIO_IN). Note the mailbox out region is at Move's master-volume level (gain staging caveats in root `CLAUDE.md`).

---

## 5. Timing / clock for bar-quantized looping

Two mechanisms, both DSP-side:

1. **`host->get_bpm()`** — provided to overtake DSPs (`schwung_shim.c:1458`) and chain plugins (`shadow_chain_mgmt.c:994`). Fallback chain in `shadow_sampler.c:332-365`: live MIDI clock measurement → current Set's tempo (parsed from the Set file) → last known clock BPM → settings `tempo_bpm` → 120.0. Good for *displaying* tempo and for pre-computing target lengths. (Magneto's CLAUDE.md claims `host_api_v1_t` has no BPM field — true for its vendored older struct, **stale vs current source**; but Magneto also can't see it because its embedded struct predates the field. Check `min_host_version` if you rely on it.)
2. **Raw MIDI clock in `on_midi`** — the shim taps **cable 0** of MIDI_OUT and delivers 0xF8 (tick), 0xFA (start), 0xFB (continue), 0xFC (stop) as 1-byte messages to the overtake DSP (`schwung_shim.c:1196-1214`; cable 0 carries Move's transport "regardless of the user's MIDI Clock Out preference"). **This is the sample-accurate path.** Magneto's proven recipe (`magneto.c:843-885`, `magneto/CLAUDE.md`):
   - keep a per-instance `clk_frame` sample counter advanced by `frames` each block;
   - on each 0xF8, measure inter-tick interval in samples, smooth (`+= 0.15*(iv-avg)`), derive BPM at 24 PPQN;
   - **for exact bar-length takes, don't convert to seconds — count ticks**: arm `rec_target_ticks = beats × 24` at record-start and stop when the count hits it (tick-locked, immune to BPM jitter/drift), with a hard buffer-cap fallback for stalled clocks;
   - `clock_is_live()` = BPM known && tick within ~0.5 s; otherwise fall back to `get_bpm()`/manual tempo × sample math.
   - Clock only flows while Move's transport runs (cable-0 realtime is Move's internal sequencer state). If the user never presses Play on Move, there is no grid — Looperino needs a free-run mode or its own tempo.
3. `get_clock_status()` exists in the header (UNAVAILABLE/STOPPED/RUNNING) but is **NULL on the overtake host API** — use 0xFA/0xFC + tick staleness instead.

JS never sees clock (`shouldFilterMessage` drops 0xF8; the shadow MIDI ring doesn't carry it anyway) — all quantization logic belongs in the DSP.

---

## 6. Build & deploy

### 6.1 Target and toolchain

**aarch64 Linux, glibc** (`BUILDING.md:3, 549`; TwinSampler's shipped blob is ELF machine 0xB7 = AArch64; Magneto Docker uses `gcc-aarch64-linux-gnu` on `debian:bookworm-slim`). **Not armv7.** `CROSS_PREFIX=aarch64-linux-gnu-` (Docker) or `aarch64-unknown-linux-gnu-` (Homebrew). Minimal module build needs no Schwung sources beyond the two headers:

```bash
aarch64-linux-gnu-gcc -O2 -g -shared -fPIC -std=c11 \
  -o dist/looperino/dsp.so src/dsp/looperino.c -lm
```

(Magneto adds `-ffast-math`; TwinSampler uses `-O2 -std=c11 -Wall -Wextra`. Either vendor the API typedefs like Magneto or `-I schwung/src` and `#include "host/plugin_api_v1.h"`.) Verify: `file dsp.so` → ARM aarch64; `nm -D dsp.so | grep move_plugin_init_v2`.

### 6.2 Install & iterate over SSH

```bash
ssh ableton@move.local 'mkdir -p /data/UserData/schwung/modules/overtake/looperino'
scp dist/looperino/{module.json,ui.js,dsp.so} \
    ableton@move.local:/data/UserData/schwung/modules/overtake/looperino/
```

Iteration facts (all from source):
- **module.json + module list**: `scanForOvertakeModules` re-reads every module.json each time the Overtake/Tools menu is entered — no rescan/reboot needed. (`host_rescan_modules()` exists for the generic module list.)
- **ui.js (entry file)**: `shadow_load_ui_module` evaluates with a unique module name `path#N` to bypass QuickJS's module cache — "fresh code on every launch ... without restarting shadow_ui" (`shadow_ui.c:620-655`). Exit and relaunch the module = new code.
- **⚠ imported files are NOT cache-busted**: the loader uses stock `js_module_loader` (`shadow_ui.c:2281`); relative imports resolve to their real path and stay cached for the life of the long-lived shadow_ui process (which only exits on Move restart, `schwung_shim.c:7104-7113`). TwinSampler's `ui.js → ui_chain.js` split has this exposure. **Practical rule: while iterating, keep hot code in the entry ui.js, or restart Move after touching imported files.**
- **dsp.so**: dlopen'd on each overtake launch and dlclosed on exit (`shadow_overtake_dsp_load/unload`) → copy new .so, exit + re-enter the module. No reboot needed (unlike chain DSPs, where the skill's "reboot for .so changes" advice applies). Caveat: if the module is *suspended* (suspend_keeps_js), the old .so stays loaded until full exit.
- Restart Move without reboot: `ssh ableton@move.local` → `/data/UserData/schwung/scripts` has restart helpers; the manager UI (http://move.local:7700) can restart too.

### 6.3 Logging / debugging on device

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'   # enable
ssh ableton@move.local 'tail -f /data/UserData/schwung/debug.log'    # watch
ssh ableton@move.local 'rm /data/UserData/schwung/debug_log_on'      # disable
ssh ableton@move.local ': > /data/UserData/schwung/debug.log'        # truncate (never rotates)
```
JS `console.log()` routes there automatically; C uses `host->log(msg)` (single `const char*`, **never from render_block**) or `LOG_DEBUG("looperino", ...)` from `host/unified_log.h`. Display mirror: http://move.local:7681. Web manager + file browser: http://move.local:7700. Perf: OTLP tracing via `touch /data/UserData/schwung/otlp_trace_on`, JS spans via `host_trace_begin/end`.

Release plumbing (later): tag-driven GitHub workflow builds `looperino-module.tar.gz` (tarball root = `looperino/`), `release.json` on main with `version` + `download_url`, catalog entry `component_type: "overtake"`, `min_host_version` per used features.

---

## 7. Realtime constraints & known pitfalls

From `docs/REALTIME_SAFETY.md`, root `CLAUDE.md`, and the skill's `references/realtime.md`/`manifest.md`:

**Hard rules for `render_block` / `on_midi` / `set_param`-on-SPI-thread:**
- Budget: 128 frames = 2.9 ms total; ~900 µs of shim compute after the ~2 ms SPI transfer. Your render shares that with 4 chain slots + master FX.
- No `fopen/fprintf`/file I/O (observed 78 ms spikes), no `malloc`, no locks shared with non-RT threads, no `host->log`. Preallocate all loop buffers in `create_instance`.
- If you spawn a helper thread (e.g. WAV writer): it inherits **SCHED_FIFO 70** from MoveOriginal — reset to `SCHED_OTHER` and keep off core 3 (`sched_setscheduler` + `taskset 0x7` pattern; `shadow_process.c` precedent). Use an SPSC ring from RT → writer (the shim-worker pattern, REALTIME_SAFETY.md:101-110).
- `midi_inject_to_move`: ≤ 8 packets/tick; >16 injections/tick historically SIGABRTs. MIDI_IN events are 8 bytes (packet+timestamp) — never hand-write 4-byte events into MIDI_IN.
- Never write to `/tmp` (rootfs 100% full — crashes the device). Everything under `/data/UserData/`.

**Module-level pitfalls:**
- module.json: minimal parser (no comments/trailing commas), ≤ 8 KB; wrong `component_type` ⇒ wrong install dir ⇒ invisible module; wrong DSP entry symbol ⇒ silent load failure (check `debug.log` for "Overtake DSP: ...").
- Fire-and-forget `set_param` clobbering in overtake mode — batch related values into one CSV/packed param, or use the blocking variant (this is why the host pushes `passthrough` as a single CSV, `shadow_ui.js:3607-3612`).
- `get_param` from JS blocks up to 100 ms on timeout — poll one packed status key per tick, not N keys.
- LED first-paint: progressive (queue flushes 16/tick; full 32-pad + buttons repaint takes ~5-6 ticks).
- Don't rely on tick cadence for musical timing in JS (tick is sleep-paced, ~500 Hz in overtake, ~60 Hz when suspended).
- `enum` set_param: don't `atoi()`-fallback blindly (skill's parse_enum trap); wire format for shadow enum knobs is the option index unless `options_as_string`.
- Exiting: never consume the host escape combo (Shift CC49, vol-touch note 8, jog CC3); handle Back yourself (`raw_ui`) like TwinSampler's wrapper.
- If Looperino sends MIDI out from the DSP audio thread, only `midi_send_external` is ring-buffered async; `midi_send_internal` routes into chain slot dispatch — prefer JS for LEDs.
- Overtake FX-mode processes only Schwung's ME bus, not Move's output — don't pick FX mode expecting to hear Move through your looper.

---

## 8. Recommended Looperino skeleton

```
looperino/                          # repo root
  module.json                       # §1.1 contents
  src/
    ui.js                           # thin wrapper: init/tick/onMidi* + Back/exit handling
                                    #   (TwinSampler wrapper pattern, twin/twinsampler/ui.js)
                                    #   during dev keep ALL UI logic here (import-cache pitfall §6.2)
    dsp/
      looperino.c                   # v2 generator plugin, single file to start:
                                    #   - vendored plugin_api_v1.h typedefs (Magneto style) or -Ischwung/src
                                    #   - create_instance: calloc loop buffers (N loops × max_secs × 44100 × 2 × int16)
                                    #   - on_midi: 0xF8/FA/FB/FC clock+transport (Magneto recipe §5),
                                    #             pad notes 68-99 → punch-in/out at block accuracy,
                                    #   - render_block: read audio_in (host->mapped_memory+audio_in_offset),
                                    #             record/overdub (old*feedback + new, DC-blocker — magneto.c:1103,1276-1303),
                                    #             sum loop playback + optional input monitor into out,
                                    #   - set_param/get_param: transport verbs (rec/play/stop/clear per loop),
                                    #             packed "status" string for the UI, path params for save/load
      wav_writer.c                  # later: SCHED_OTHER writer thread + SPSC ring for exports
  scripts/
    build.sh                        # docker run debian:bookworm gcc-aarch64-linux-gnu; §6.1 command
    install.sh                      # ssh mkdir + scp to modules/overtake/looperino/ (magneto/scripts/install.sh)
    Dockerfile                      # FROM debian:bookworm-slim + gcc-aarch64-linux-gnu (magneto/scripts/Dockerfile)
  help.json                         # on-device help (20-char lines)
  release.json                      # version + download_url (CI-maintained, later)
  .github/workflows/release.yml     # tag → build → tarball → release (later)
```

Division of labor (from the working precedents):
- **DSP (C)**: all audio, all clock math, all quantization, pad-triggered punch-in (audio-thread `on_midi`), loop state machine, file I/O for save/export (off-thread eventually).
- **JS (ui.js)**: LEDs (loop states via pad colors + Play/Rec button LEDs), display (loop lengths/positions from a packed `get_param("status")` once per tick), encoder handling (decodeDelta on batched CCs 71-78/14), transport buttons → `set_param` verbs, Back/suspend handling, speaker-feedback guard before enabling monitor, settings persistence via `host_write_file` under the module dir.
- Ship every feel-related constant (gesture timings, LED colors, FX ranges) as a `const` table at the top of ui.js / a `defaults` block — per the project's working agreements.

M0 (input/latency spike) maps to: minimal v2 generator that copies `audio_in` → `render_block` output with a gain, one pad toggling monitor, `get_bpm()` + 0xF8 tick counter printed to the display — this exercises every risky integration point (input read, deferred-render latency, clock delivery, param bridge, LED/display) in <300 lines.


## Key risks / open uncertainties (from study)

- Latency (M0 gate): overtake DSP output is deferred one audio block by design (schwung_shim.c:1577-1596), so input→output monitoring is at least ~2.9 ms + device ADC/DAC latency — actual end-to-end number must be measured on hardware before committing to DSP-side monitoring.
- MIDI clock availability: cable-0 0xF8 only flows while Move's transport is running; behavior when the user never presses Play (no grid) needs a designed free-run/manual-tempo fallback. Also host->get_bpm exists in current source (schwung_shim.c:1458) but Magneto's docs show older hosts lacked it — verify on the device's v0.11.4 that the field is populated (repo clone is newer than the installed release).
- JS import caching: shadow_load_ui_module cache-busts only the entry ui.js; imported .mjs/.js files stay cached in the long-lived shadow_ui process (stock js_module_loader, shadow_ui.c:2281). Multi-file UI iteration may silently run stale code — verify on device; until then keep hot UI code in one file.
- Fire-and-forget param writes in overtake mode can clobber each other (single SHM slot); set_param/get_param are served on the SPI thread, so heavy work there glitches audio — the save/load UX must be designed around this (blocking writes for ordered sequences, background thread for WAV I/O).
- Master-volume knob (CC 79) behavior while an overtake module is active was not fully traced in the shim's pre-ioctl filter — unclear whether Move keeps adjusting its own volume or the module must handle CC 79; verify on device (affects whether Looperino needs claims_master_knob-like handling or button_passthrough).
- Memory budget: preallocated loop buffers (e.g. 4 stereo loops × 60 s ≈ 42 MB int16) are allocated in the Move firmware process via the shim — Magneto's 10 MB precedent works, but larger footprints are unproven; also the tick-rate/LED budgets assume the shim isn't already loaded with 4 active chain slots.
- TwinSampler's actual recording engine (dsp_core.so) ships as a binary blob with no source, so its WAV-write threading model is unknown — the guide's recommendation of an off-thread writer is inferred from REALTIME_SAFETY.md patterns, not from a proven looper precedent (Magneto writes files on the SPI thread and accepts the glitch).
- Docs vs source drift: tick rate (60/344/44 per docs vs ~500 Hz overtake / ~60 Hz normal in shadow_ui.c:2638-2644), overtake init delay (~500 ms comment computed at 16 ms ticks), and Magneto's 'no host BPM' claim are all stale in places — anything load-bearing should be re-verified against the exact Schwung version installed on the device (v0.11.4) rather than the cloned main branch.
