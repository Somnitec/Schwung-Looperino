# Omniphone "Chord Journey" — analysis and porting guide for Looperino

All paths below are under `/home/memo/Documents/code/Omniphone`. The feature is called **"Harmonic Journey"** in code and lives entirely in one variant: `variants/teensy40-12pad-screen/` (the "Bamboo" 12-pad hang-drum instrument with a 1.28" round touchscreen). It is real, implemented, and working — not just planned (git: `ddf3734 Harmonic journey implemented`, `9963fdf Chord journey made more responsive`, `ecbb689 tonal map implemented`). KANBAN.md:43,53,382 shows it still "in progress" only in the sense of sound-design polish.

## 1. How it works

### The core idea
A "chord" in Omniphone is **a full retuning of all 12 pads at once**. The player never plays a chord change — they *navigate* to it on the touchscreen while both hands keep playing the pads. Tapping a chord node glides every pad's pitch to the new chord's voicing over 250 ms; the running play mode (proximity synth, arpeggiator, etc.) is untouched — only pitch targets change (`main.cpp:592-615`). Harmony becomes a place you travel through, not a thing you have to know how to spell.

### Chord representation — two coexisting schemes

**A. Fixed atlases: hand-authored absolute voicings.**
`struct HarmonicChord` (`config.h:840-849`):
- `name` (e.g. "Dm", "Db7"), `feeling` (an emotion word, e.g. "Hardship", "Tritone sub")
- `function` — `HarmonicFunction` enum (`config.h:822-828`): TONIC / SUBDOMINANT / DOMINANT / CHROMATIC / SYMMETRIC → drawn as node **shape** (circle/triangle/square/pentagon/hexagon)
- `mood` — `HarmonicMood` enum (`config.h:832-838`): WARM / TENSION / MELANCHOLY / MYSTERY / GROUNDED → node **colour**
- `freqs[12]` — absolute float Hz per pad (pads 0-5 upper ring, 6-11 lower ring an octave down). Not degrees, not MIDI — literal per-pad frequencies, hand-voiced.

**B. Tonal Map: compact symbolic ids + algorithmic voicing.**
`tonalmap.h:23-47`: node id = `root*4 + type`, where root ∈ 0…11 (C…B) and type ∈ {TM_MAJ, TM_MIN, TM_DOM7, TM_AUG} — 48 nodes, plus an 8-node "inner ring" (ids 48-55, a G7→C→E7→A→Db7→Gb→Bb7→Eb dominant cycle) whose root/type live in `CYCLE8_CHORD` (`tonalmap.h:39-42`). One `uint8_t` fully identifies a chord. Voicings are then **generated**, not stored (see below).

### The journeys (atlases) — six theories of harmonic motion
`HarmonicJourney` table at `config.h:1055-1078`; the big explanatory comment is `config.h:788-815`:

1. **Diatonic** (`HARMONIC_CHORDS`, `config.h:855-891`) — the 7 diatonic chords of C major / A minor, I at the centre (home) with satellites on a ring. Comes with a full 7×7 **emotional transition matrix** `HARMONIC_TRANSITIONS` (`config.h:897-912`): every from→to move has a label — V→I "Resolution", I→vi "Darkening", vi→I "Redemption", iii→IV "Magic lift", V→vii° "Into chaos". Credited to William R Thomas, "Tonal Map of Chord Sequences" (2025), diagram by @purpasteur.
2. **Flamenco** (`config.h:918-940`) — E Phrygian-dominant, Andalusian cadence, the ♭II "Spanish colour".
3. **Jazz** (`config.h:947-972`) — Cmaj7 hub + 7 nodes each naming a *device*: ii, V, tritone sub, secondary dominant, modal mixture (♭VI), Coltrane major-third change, whole-tone/augmented.
4. **Cinematic** (`config.h:1003-1042`) — an augmented-triad hub (C+) with 6 chromatic-mediant satellites (Cm, F, Em, A, Abm, Db). The voicings are constructed so **every pad moves at most ±1 semitone on any hub↔ring move** (`config.h:1011-1019` — "verified by exhaustive per-chord-tone search, not hand-tuned"). Only hub spokes are legal moves, so voice leading is always smooth.
5. **Hexatonic** (`config.h:974-1001`) — a Neo-Riemannian P/L hexatonic cycle (Cohn): C, Cm, Ab, Abm, E, Em on a ring with no centre; each step is a single-semitone voice-leading move. The "magic/awe" chromatic-mediant sound.
6. **Tonal Map** (`tonalmap.h`) — the dynamic 56-node graph, described next.

### Transition logic — a hand-drawn constrained graph, not a formula
`TONAL_MOVE_TABLE[56][4]` (`tonalmap.h:76-138`): each node offers **at most 4 legal outgoing moves** (`TONAL_MAX_MOVES = 4`), each flagged `dotted` (optional/tension/region-change) or solid (functional resolution, "always-good"). The header comment (`tonalmap.h:12-17`) is explicit: the graph is *not* uniform across roots — it was hand-drawn node-by-node in a purpose-built visual editor and pasted back as JSON, so `tonalMapMoves()` (`tonalmap.h:142-151`) is a straight table lookup. Concept credit: the chord-flower diagram by @briancalli.music. So the harmonic knowledge is a **curated adjacency table**, blending functional harmony (dominant resolutions), circle-of-fifths motion, and chromatic moves — with Neo-Riemannian logic present only in the Hexatonic/Cinematic atlases (as data, not code).

### Algorithmic voicing — `tonalMapFreqs()` (`tonalmap.h:170-215`)
Given a node id, fills 12 pad frequencies:
- Interval templates: `IV_MAJ {0,4,7}`, `IV_MIN {0,3,7}`, `IV_DOM7 {0,4,7,10}`, `IV_AUG {0,4,8}` (`tonalmap.h:172-175`).
- Finds the **widest gap** between consecutive chord tones (wrapping to the octave) and drops **exactly one passing tone** at its midpoint (`tonalmap.h:184-197`) — melodic interest without losing consonance.
- Builds 6 ascending "core" pitches by cycling the tone sequence upward with octave bumps from C4 = 261.6256 Hz, equal temperament (`tonalmap.h:201-209`).
- Pads pair up (0,1)(2,3)…: even pad = core pitch an octave **down**, odd pad = the pitch itself (`tonalmap.h:211-214`) — repeats are always octaves, never unisons; walking the pads traces a rising ~2-octave line; any 3 adjacent pads are a consonant cluster.

### Runtime transition mechanics (`main.cpp`)
- State: `activeJourney`, `activeHarmonicChord`, `activeTonalNode`, `harmonicFreqs[12]`, `harmonicOverride` (`main.cpp:251-256`).
- `loadHarmonicChord()` / `loadTonalMapNode()` (`main.cpp:597-634`): copy the target voicing into `harmonicFreqs[]`, snapshot current pitches into `glideStart[]`, start the glide clock.
- **Glide**: `SET_GLIDE_MS = 250` (`config.h:540`), exponential (log-domain) sweep `glideFreq = start * pow(target/start, t)` updated at ~1 kHz, deliberately decoupled from the 6 ms sensor frame so big whole-chord retunes never step audibly (`main.cpp:1237-1268`). The target is `harmonicFreqs[i]` when overriding, else the normal scale (`main.cpp:1258`).
- **Responsiveness**: chord taps act on the raw touch-**down** edge, not the touch chip's delayed CLICK gesture, because the delay "read as sluggish chord selection" (`main.cpp:1708-1716`, commit `9963fdf`).
- **Persistence**: EEPROM remembers screen state, atlas, chord, and tonal-map node (`main.cpp:280-284`), so the instrument boots back into the same place in the journey (`main.cpp:1163-1190`).
- Journey mode forces the Proximity play mode and restores the previous mode on exit (`main.cpp:1806-1821`).

### Display model (`display.h:291-321`)
Current chord at the **centre** (r=22px); its ≤4 legal moves on an **option ring** (r=64); each option's *own* moves as a **dim preview fan** further out (r=97) — you always see one move ahead. Tapping re-centres with a 400 ms animated relayout (`TM_ANIM_MS`, `display.h:297`): shared nodes glide to new positions, new ones fade in, dropped ones fade out. Colour by chord type (major pink, minor blue, dom7 amber, aug green — `display.h:325-338`); shape by harmonic function (`display.h:345-353`).

### What makes it playful (distilled)
1. **You can only go somewhere good.** The ≤4-move curated graph removes wrong answers; dotted edges add optional spice. Deep moves (tritone subs, chromatic mediants) are one tap away without needing to know their names.
2. **One-move lookahead** (the dim preview fan) makes navigation feel like exploring a map, not picking from a list.
3. **Continuous sound through the change** — the 250 ms exponential glide plus minimal-motion voicings (the Cinematic ±1-semitone guarantee; the hexatonic single-semitone cycle) means chord changes *morph* rather than jump.
4. **Emotion as the interface**: moves are labelled "Redemption", "Magic lift", "Into chaos" (`config.h:897-912`); nodes are shaped by function and coloured by mood — theory is translated into feel.
5. **Instant, latency-free selection** (touch-down edge) and **statefulness** (EEPROM resume) make it feel like an instrument, not a menu.
6. Related but separate: the scale layouts themselves are chord-aware — stacked-thirds sets where any 3 adjacent pads form a chord, and "Chords I IV V" / "Jazz ii V I" sets that put chord *functions* in pad zones (`config.h:302-373`).

## 2. What to port to Looperino, and in what form

**a. The Tonal Map graph — port nearly verbatim.** `tonalmap.h` is dependency-free (`stdint.h` + `math.h` only, ~215 lines). Take: the node-id packing `root*4 + type` (one `uint8_t` per chord — perfect for a step-lane event), `TONAL_MOVE_TABLE[56][4]` (`tonalmap.h:76-138`), `tonalMapMoves()` (`tonalmap.h:142-151`), `TONAL_NAMES` (`tonalmap.h:49-64`), the solid/dotted edge distinction. This table *is* the "playful deep changes" engine. Carry the attribution comments (@briancalli.music, William R Thomas / @purpasteur).

**b. The voicing algorithm — port the skeleton, retarget the output.** From `tonalMapFreqs()` (`tonalmap.h:170-215`): interval templates, widest-gap passing-tone insertion, and the ascending-cycle construction. For Looperino emit **MIDI note numbers / pitch classes** instead of Hz (drop the `powf` at `tonalmap.h:207` and keep the semitone math `tmRoot(node) + seq[idx] + 12*octUp`).

**c. The atlas concept.** `HarmonicJourney` (`config.h:1055-1067`) — multiple curated harmonic "worlds" selected by a picker: Diatonic / Flamenco / Jazz / Cinematic / Hexatonic / full Tonal Map. On Looperino this becomes the *palette* a project's chord track draws from. The Hexatonic (`config.h:982-1000`) and Cinematic-hub (`config.h:1020-1042`) atlases are tiny (6-7 entries) and give the strongest "wow" per byte — cinematic chromatic mediants with guaranteed smooth voice leading.

**d. The emotional transition matrix.** `HARMONIC_TRANSITIONS[7][7]` (`config.h:897-912`) plus per-chord `feeling` strings — show the move label on Move's display when a transpose pad is pressed. Cheap, and it's half the charm.

**e. Parameter values that were tuned in anger.** Glide 250 ms exponential (`config.h:540`, `main.cpp:1250-1260`); UI relayout 400 ms (`display.h:297`); max 4 moves per node; act on press-down not release (`main.cpp:1708-1716`); persist journey position across power cycles (`main.cpp:280-284`).

**f. Function/mood classification** (`config.h:822-838` and `display.h:345-353` for deriving function from chord type) — reuse for pad colours on Move's RGB pads: tonic/subdominant/dominant/chromatic/symmetric as colour families, so the four "move" pads telegraph what kind of move they are before you press them.

## 3. What to do differently in Looperino (step-sequenced groovebox)

Omniphone's journey is **performance navigation with no timeline** — chords change when a finger says so, and "transposition" is total retuning of the playing surface. Looperino adds a clock, a step lane, and tracks that already contain notes. Key adaptations:

1. **Represent chords symbolically, not as voicings.** Store one byte per chord step: the tonal-map node id (`root*4+type`, 0-47, reserve 48-55 or use 0xFF for "no chord"). Omniphone's fixed atlases store absolute Hz because pads *are* the voice; Looperino's chord track must instead broadcast `(rootPc, chordType → pitch-class mask)` to instrument tracks. Derive the mask from the same interval templates (`tonalmap.h:172-175`).

2. **Split "journey navigation" from "progression playback".** Omniphone has no sequencer; Looperino needs both layers:
   - **Chord lane**: chord steps on the 16-step (or per-bar) timeline; a chord step holds until the next one (OP-Z semantics). Steps store node ids.
   - **Live transpose pads = the current node's legal moves.** This is the central porting insight: instead of 12 chromatic transpose pads, light **up to 4 pads with `tonalMapMoves(currentNode)`** (+1 "home" pad returning to the progression's root node). Pressing one jumps the whole mix there — quantized to the next step/beat/bar — and is *recordable into the chord lane*. That reproduces "you can only go somewhere good" in a groovebox. Optionally a shift layer exposes raw chromatic/degree transpose for people who want it.
   - Show the one-move **preview**: dim-lit second row hinting where each move could go next (Omniphone's preview fan, `display.h:291-295`).

3. **Per-track follow modes instead of a global override.** Omniphone's `harmonicOverride` is all-or-nothing (`main.cpp:1258`). Looperino (per DESIGN.md:88-91) needs per-track flags; recommend three levels, not a boolean:
   - **ignore** — drums, vocal loops;
   - **root-follow** — transpose by the interval between chord roots (bass lines, samples);
   - **chord-follow (smart)** — remap each note to the nearest tone of the new chord's pitch-class set, choosing the **smallest signed semitone move**. This is the MIDI-domain equivalent of Omniphone's minimal-motion voice leading (the Cinematic ±1-semitone property, `config.h:1011-1019`) and is what will make deep changes (mediants, tritone subs) sound intentional rather than transposed-sideways.

4. **Quantized changes replace the pitch glide.** The 250 ms exponential glide is the right feel for a drone/proximity instrument but wrong for sequenced material; the groovebox analog is (a) change quantization (step/beat/bar, default next step) and (b) smooth voice leading via nearest-tone mapping. If a track hosts a synth engine Looperino controls directly, an optional 30-80 ms pitch glide on sustained notes at the change boundary recovers some of the morph feel.

5. **Keep the graph data, drop the screen renderer.** The radial map (`display.h`) is built for a 240×240 round touchscreen; on Move the pads *are* the map. Port the data structures and move logic; re-express shape/mood as pad colour + display text (move label like "Redemption" on Move's screen at the change).

6. **Handle jumps the graph doesn't cover.** The hand-drawn table is directed and sparse — a recorded progression edited step-by-step may contain from→to pairs with no edge. Playback must allow any pair (it's just data); the graph should constrain only the *live pad* affordances and act as a suggestion engine ("legal move" highlighting) when editing chord steps.

7. **Minimal M5 model** (honours DESIGN.md:88-91,113-114): chord lane of `uint8_t` node ids + per-track 2-bit follow mode + `tonalMapMoves()` for 4 live-transpose pads + nearest-tone remapper + change quantizer. Everything else (atlases beyond the Tonal Map, emotional labels, preview lighting) layers on without schema changes.

## Side observations
- `main.cpp:594-596` notes the emotional transition labels are currently *documentation/data* rather than drawn on screen (node names were removed in commit `ba0b699 "no chord names"`) — the feature regressed visually on Omniphone but the data is intact and ideal for Looperino's display.
- Possible latent bug in Omniphone worth telling the user: `CINEMATIC_CHORDS` (`config.h:1020-1042`) initializes `HarmonicChord` aggregates without the `function`/`mood` members (compare `CINEMATIC_HEXATONIC_CHORDS` at `config.h:982-1000`, which has them). Positionally the 12-float braced list would land on `uint8_t function`, which should be a compile error; the last built firmware (`.pio/build/teensy40-12pad-screen/firmware.elf`, Jul 3 16:46) predates the newest config.h commit and no compiler was available in this sandbox to confirm.


## Key risks / open uncertainties (from study)

- No automated progression/sequencing exists in Omniphone — journeys are entirely manual, screen-driven navigation with no clock; the 'chord steps on a timeline' half of Looperino's feature is new design territory, so section 3's model is a proposal, not a port.
- The TONAL_MOVE_TABLE graph is hand-drawn, directed, and asymmetric (some moves have no reverse edge, inner-ring nodes duplicate outer chords); if Looperino lets users edit arbitrary chord steps, the graph can only be advisory, and any regenerated/extended table loses the hand-curated musicality.
- Omniphone's fixed-atlas voicings are absolute per-pad Hz tuned for a 12-pad drone instrument; the pitch-class/degree abstraction proposed for Looperino's follow modes is my extrapolation of tonalMapFreqs(), not something Omniphone implements.
- Emotional transition labels exist as a full from->to matrix only for the Diatonic atlas (7x7 at config.h:897); other atlases fall back to one 'feeling' string per chord, so a per-move label system in Looperino needs new authoring for non-diatonic moves.
- Possible compile break at Omniphone HEAD: CINEMATIC_CHORDS aggregate initializers (config.h:1020-1042) omit the function/mood fields added to HarmonicChord; could not verify (no C++ compiler in sandbox, last .pio build predates the latest config.h commit) — worth a quick `pio run -e teensy40-12pad-screen` check before treating that table as authoritative.
- Attribution: the Tonal Map concept/graph is credited in-source to @briancalli.music and William R Thomas's 'Tonal Map of Chord Sequences' (diagram by @purpasteur); porting the move table into Looperino should carry that credit, and any licensing implications of the hand-transcribed diagram are unverified.
