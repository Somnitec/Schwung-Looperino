/*
 * Looperino UI — overtake module (Schwung host v0.11.4).
 *
 * The DSP owns all audio + timing state; pads reach it directly on the audio
 * thread (the low-latency punch-in path). This file paints LEDs/display from a
 * polled status string, owns the *editorial* surfaces (views, track select,
 * step patterns + probability, chord navigation, menu, mixer encoders), and
 * pushes a pad->role map so the DSP can act on pads without knowing about
 * views. It never makes timing decisions.
 *
 * Keep ALL hot code in this one file — imported project files are cached for
 * the life of shadow_ui (schwung-module-guide §6.2). Host shared/*.mjs is safe.
 *
 * Everything feel-related (colors, timings, layout) is a const near the top.
 */

import { shouldFilterMessage, setLED, setButtonLED, decodeDelta }
    from '/data/UserData/schwung/shared/input_filter.mjs';

// ---------------- LED palette (indexed, shared/constants.mjs) ----------------
const LED = {
    Black: 0, BrightRed: 1, OrangeRed: 2, BrightOrange: 3, VividYellow: 7,
    BrightGreen: 8, NeonGreen: 11, Cyan: 14, AzureBlue: 15, Purple: 22,
    NeonPink: 23, White: 120, LightGrey: 118, DarkGrey: 124, PureBlue: 125,
    Green: 126, Red: 127,
};
// red -> purple -> blue playhead fade, 8 steps (feel constant)
const PLAYHEAD_RAMP = [LED.Red, LED.BrightRed, LED.OrangeRed, LED.NeonPink,
                       LED.Purple, LED.Purple, LED.AzureBlue, LED.PureBlue];
// per-track identity colors (looper 1-4 warm, instr 5-8 cool)
const TRACK_COLOR = [LED.BrightRed, LED.BrightOrange, LED.VividYellow, LED.Cyan,
                     LED.BrightGreen, LED.NeonGreen, LED.AzureBlue, LED.Purple];
const FX_COLOR = [LED.NeonPink, LED.Purple, LED.Cyan, LED.BrightOrange,
                  LED.AzureBlue, LED.VividYellow];

// ---------------- control map (CC numbers) ----------------
const CC = {
    JOG_CLICK: 3, JOG: 14,
    TRACK1: 43, TRACK2: 42, TRACK3: 41, TRACK4: 40,
    SHIFT: 49, MENU: 50, BACK: 51,
    DOWN: 54, UP: 55, UNDO: 56, LOOP: 58, COPY: 60, LEFT: 62, RIGHT: 63,
    KNOB1: 71, MASTER: 79,
    PLAY: 85, REC: 86, MUTE: 88,
    SAMPLE: 118, DELETE: 119,
};
const TRACK_CC = [43, 42, 41, 40];  // track 1..4

// ---------------- roles (must match looperino.h) ----------------
const ROLE = { NONE: 0, CLIP: 1, DRUM: 2, NOTE: 3, FX: 4 };
const VIEW = { A: 0, B: 1, C: 2 };  // A clips|instr, B clips|fx, C instr|fx
const VIEW_NAMES = ['CLIPS|INSTR', 'CLIPS|FX', 'INSTR|FX'];

// ---------------- feel/timing ----------------
const STATUS_POLL_TICKS = 16;   // ~30/s at ~500 Hz overtake tick
const JACK_POLL_TICKS = 256;
const GAIN_STEP = 0.02;
const LEDS_PER_TICK = 12;       // progressive first-paint budget (< 16/tick)

// ---------------- state ----------------
let tick_n = 0;
let status = null;
let view = VIEW.A;
let sel = 0;                    // 0-3 looper, 4-7 instr, 8 master
let shift = false;
let menuOpen = false;
let menuIdx = 0;
let heldStep = -1;             // step button held (for probability)
let allowMonitor = null;
let displayDirty = true;
let ledDirty = true;
let ledCursor = 0;

// JS-owned editorial data (mirrored to DSP)
const instrType = [0, 0, 1, 1];       // 0 drum, 1 synth (default)
const focusSlot = [0, 0, 0, 0];       // focused drum slot per instr track
const gains = [1,1,1,1,1,1,1,1];
const mutes = [0,0,0,0,0,0,0,0];
// step patterns: per instr track. drums: 16 slots x 16 steps; synth: 16 steps.
const drumPat = [];   // drumPat[t][slot] = {on:[16], prob:[16]}
const synthPat = [];  // synthPat[t] = {on:[16], prob:[16]}
for (let t = 0; t < 4; t++) {
    drumPat[t] = [];
    for (let s = 0; s < 16; s++) drumPat[t][s] = { on: new Array(16).fill(0), prob: new Array(16).fill(100) };
    synthPat[t] = { on: new Array(16).fill(0), prob: new Array(16).fill(100) };
}
let masterGain = 1.0;
let monitor = 0;
let metroOn = 0;

// ---------------- status parsing ----------------
function parseStatus(s) {
    if (!s) return null;
    const o = {};
    for (const part of s.split(';')) {
        const i = part.indexOf('=');
        if (i > 0) o[part.slice(0, i)] = part.slice(i + 1);
    }
    return o;
}
function clipState(i) {           // returns {state,pos,layers} from status.clips
    if (!status || !status.clips) return { state: 0, pos: 0, layers: 0 };
    const c = status.clips;
    const h = x => parseInt(x, 16) || 0;
    return { state: h(c[i*3]), pos: h(c[i*3+1]), layers: h(c[i*3+2]) };
}
function kitMask(t) {
    if (!status || !status.kits) return 0;
    return parseInt(status.kits.slice(t*4, t*4+4), 16) || 0;
}

// ---------------- pad<->grid ----------------
function noteRC(note) { const i = note - 68; return { row: (i/8)|0, col: i%8 }; }
function selInstrTrack() { return (sel >= 4 && sel < 8) ? sel - 4 : 0; }

// Build the 32-byte padmap (hex) from the current view + selection and push it.
function pushPadmap() {
    let hex = '';
    for (let note = 68; note <= 99; note++) {
        const { row, col } = noteRC(note);
        let type = ROLE.NONE, val = 0;
        const leftIsClips = (view === VIEW.A || view === VIEW.B);
        const rightIsInstr = (view === VIEW.A);
        const half = col < 4 ? 'L' : 'R';
        const showClips = (half === 'L' && leftIsClips);
        const showInstr = (half === 'L' && !leftIsClips) || (half === 'R' && rightIsInstr);
        const showFX = (half === 'R' && !rightIsInstr);
        if (showClips) {
            const track = row, slot = col;         // track's clips in its row
            type = ROLE.CLIP; val = track * 4 + slot;
        } else if (showInstr) {
            const slot = row * 4 + (col % 4);       // 4x4 -> 16 slots
            const it = selInstrTrack();
            if (instrType[it] === 0) { type = ROLE.DRUM; val = slot; }
            else { type = ROLE.NOTE; val = slot; }
        } else if (showFX) {
            const idx = row * 4 + (col - 4);
            type = ROLE.FX; val = idx & 0x0F;
        }
        const b = ((type & 0x0F) << 4) | (val & 0x0F);
        hex += b.toString(16).padStart(2, '0');
    }
    spb('padmap', hex);
}

// ---------------- param bridge ----------------
function sp(k, v) { host_module_set_param(k, String(v)); }
function spb(k, v) { host_module_set_param_blocking(k, String(v), 100); }
function gp(k) { return host_module_get_param(k); }

function pushSeq(t, slot) {   // push one drum slot lane or synth lane
    const p = slot === undefined ? synthPat[t] : drumPat[t][slot];
    let on = 0;
    for (let s = 0; s < 16; s++) if (p.on[s]) on |= (1 << (15 - s));
    let hex = on.toString(16).padStart(4, '0') + ':';
    for (let s = 0; s < 16; s++) hex += (p.prob[s] & 0xff).toString(16).padStart(2, '0');
    spb(slot === undefined ? ('seq' + t) : ('seq' + t + '_' + slot), hex);
}

// ---------------- feedback guard ----------------
function refreshGuard(force) {
    const safe = !(host_speaker_active() && !host_line_in_connected());
    const allow = safe ? 1 : 0;
    if (force || allowMonitor === null || allow !== allowMonitor) {
        allowMonitor = allow;
        spb('allow_monitor', allow);
    }
}

// ---------------- LED painting ----------------
function padColor(note) {
    const { row, col } = noteRC(note);
    const leftIsClips = (view === VIEW.A || view === VIEW.B);
    const rightIsInstr = (view === VIEW.A);
    const half = col < 4 ? 'L' : 'R';
    const showClips = (half === 'L' && leftIsClips);
    const showInstr = (half === 'L' && !leftIsClips) || (half === 'R' && rightIsInstr);
    const showFX = (half === 'R' && !rightIsInstr);
    if (showClips) {
        const idx = row * 4 + col;             // clip index
        const cs = clipState(idx);
        switch (cs.state) {
            case 0: return LED.Black;                       // empty
            case 1: return LED.DarkGrey;                    // stopped
            case 2: return PLAYHEAD_RAMP[cs.pos >> 1];      // playing (fade)
            case 3: return (tick_n & 32) ? LED.BrightGreen : LED.Black; // armed blink
            case 4: return LED.Red;                         // recording
            case 5: return LED.BrightOrange;                // overdub
            case 6: return LED.Red;                         // rec end-queued
            default: return LED.DarkGrey;
        }
    } else if (showInstr) {
        const slot = row * 4 + (col % 4);
        const it = selInstrTrack();
        if (instrType[it] === 0) {
            const filled = (kitMask(it) >> slot) & 1;
            if (slot === focusSlot[it]) return LED.White;
            return filled ? TRACK_COLOR[4 + it] : LED.DarkGrey;
        } else {
            return (slot % 4 === 0) ? LED.AzureBlue : LED.PureBlue;  // note grid tint
        }
    } else if (showFX) {
        const idx = row * 4 + (col - 4);
        if (idx >= FX_COLOR.length) return LED.Black;
        const active = status && parseInt(status.fx) === idx;
        return active ? LED.White : FX_COLOR[idx];
    }
    return LED.Black;
}

function paintAllPads() { ledDirty = true; ledCursor = 0; }

function paintPadsProgressive() {
    // progressive first-paint / repaint (<= LEDS_PER_TICK per tick)
    let painted = 0;
    while (ledCursor < 32 && painted < LEDS_PER_TICK) {
        setLED(68 + ledCursor, padColor(68 + ledCursor));
        ledCursor++; painted++;
    }
    if (ledCursor >= 32) ledDirty = false;
}

function paintButtons() {
    for (let i = 0; i < 4; i++) {
        const isSel = (sel === i) || (sel === 4 + i);
        setButtonLED(TRACK_CC[i], isSel ? 0x7c : 0x20);
    }
    setButtonLED(CC.PLAY, status && status.run === '1' ? LED.BrightGreen : 0x20);
    setButtonLED(CC.REC, 0x20);
    setButtonLED(CC.BACK, 0x40);
    setButtonLED(CC.MENU, menuOpen ? 0x7c : 0x30);
}

function paintSteps() {
    // step LEDs (CC 16-31) reflect the focused lane + playhead
    const t = selInstrTrack();
    const lane = instrType[t] === 0 ? drumPat[t][focusSlot[t]] : synthPat[t];
    const cur = status ? parseInt(status.step) : -1;
    for (let s = 0; s < 16; s++) {
        let c = lane.on[s] ? (lane.prob[s] < 100 ? LED.DarkGrey : TRACK_COLOR[4 + t]) : LED.Black;
        if (s === cur && status && status.run === '1') c = LED.White;
        setButtonLED(16 + s, c);
    }
}

// ---------------- display ----------------
function drawDisplay() {
    clear_screen();
    if (menuOpen) { drawMenu(); return; }
    print(2, 0, 'LOOPERINO', 1);
    const bpm = status ? parseFloat(status.bpm) : 0;
    const grid = status && status.grid === '1';
    print(72, 0, grid ? (bpm.toFixed(1) + ' bpm') : 'no grid', 1);

    // selection + view
    let selTxt = sel === 8 ? 'MASTER' : (sel < 4 ? ('LOOP ' + (sel+1)) : ('INST ' + (sel-3)));
    print(2, 12, selTxt + '  ' + VIEW_NAMES[view], 1);

    // chord
    const cn = gp('chordname') || '?';
    const pend = status ? parseInt(status.pend) : -1;
    print(2, 24, 'chord: ' + cn + (pend >= 0 ? (' >' + pend) : ''), 1);

    // transport / rec
    const rec = status ? parseInt(status.rec) : -1;
    const run = status && status.run === '1';
    print(2, 36, (run ? 'SEQ>' : 'seq.') + (rec >= 0 ? '  REC clip ' + (rec+1) : ''), 1);

    // sampling
    const samp = status ? parseInt(status.samp) : 0;
    if (samp === 1) print(2, 48, 'SAMPLE: waiting...', 1);
    else if (samp === 2) print(2, 48, 'SAMPLE: capturing', 1);
    else {
        // input peak meter
        const peak = status ? parseInt(status.peak) : 0;
        const w = Math.min(124, Math.round((peak / 32767) * 124));
        draw_rect(2, 56, 124, 6, 1);
        if (w > 0) fill_rect(2, 56, w, 6, 1);
    }
}

// ---------------- menu ----------------
const MENU = [
    { label: 'Tempo mode', get: () => status && status.grid === '1' ? 'locked' : 'first-loop',
      turn: () => {} },
    { label: 'Metronome', get: () => metroOn ? 'on' : 'off',
      toggle: () => { metroOn = metroOn ? 0 : 1; sp('metro', metroOn); } },
    { label: 'Monitor', get: () => monitor ? 'on' : 'off',
      toggle: () => { monitor = monitor ? 0 : 1; sp('mon', monitor); } },
    { label: 'Instr type', get: () => instrType[selInstrTrack()] ? 'synth' : 'drum',
      toggle: () => { const t = selInstrTrack(); instrType[t] = instrType[t] ? 0 : 1;
                      sp('itype', t + ':' + instrType[t]); pushPadmap(); } },
    { label: 'Follow', get: () => ['ignore','root','chord'][2], turn: () => {} },
    { label: 'Clear all', action: () => { spb('clear_all',''); } },
];
function drawMenu() {
    print(2, 0, 'MENU', 1);
    for (let i = 0; i < MENU.length; i++) {
        const y = 14 + i * 8;
        if (i === menuIdx) { fill_rect(0, y - 1, 128, 8, 1); }
        const m = MENU[i];
        const v = m.get ? m.get() : '';
        print(4, y, m.label + (v ? (': ' + v) : ''), i === menuIdx ? 0 : 1);
    }
}

// ---------------- chord navigation ----------------
let moveList = [];      // [{node,dotted}] parsed from get_param('moves')
let moveSel = 0;
function refreshMoves() {
    const s = gp('moves');
    moveList = [];
    if (s) for (const part of s.split(',')) { const [n, d] = part.split(':'); moveList.push({ node: +n, dotted: +d }); }
    if (moveSel >= moveList.length) moveSel = 0;
}

// ---------------- lifecycle ----------------
globalThis.init = function () {
    shadow_set_overtake_mode ? shadow_set_overtake_mode(2) : 0;
    refreshGuard(true);
    spb('monitor_gain', '1.00');
    pushPadmap();
    spb('sel', sel);
    paintAllPads();
    paintButtons();
    displayDirty = true;
};

globalThis.onResume = function () {
    paintAllPads();
    paintButtons();
    displayDirty = true;
};

globalThis.onUnload = function () { };

globalThis.tick = function () {
    tick_n++;
    if (tick_n % STATUS_POLL_TICKS === 0) {
        const s = parseStatus(gp('status'));
        if (s) {
            status = s;
            if (s.focus !== undefined) focusSlot[selInstrTrack()] = parseInt(s.focus) || 0;
            ledDirty = true; ledCursor = 0;   // playhead animates; repaint pads
            displayDirty = true;
            refreshMoves();
        }
    }
    if (tick_n % JACK_POLL_TICKS === 0) refreshGuard(false);
    if (ledDirty) paintPadsProgressive();
    // steps + buttons are cheap; refresh at status rate
    if (tick_n % STATUS_POLL_TICKS === 0) { paintSteps(); paintButtons(); }
    if (displayDirty) { drawDisplay(); displayDirty = false; }
};

// ---------------- MIDI in ----------------
globalThis.onMidiMessageInternal = function (data) {
    if (shouldFilterMessage(data)) return;
    const [statusByte, d1, d2] = data;
    const type = statusByte & 0xF0;

    if (type === 0x90 && d2 > 0) {                     // note-on
        if (d1 >= 16 && d1 <= 31) { onStepDown(d1 - 16); return; }
        // pads (68-99) are handled by the DSP; JS just notes nothing
        return;
    }
    if (type === 0x80 || (type === 0x90 && d2 === 0)) { // note-off
        if (d1 >= 16 && d1 <= 31) { if (heldStep === d1 - 16) heldStep = -1; return; }
        return;
    }
    if (type === 0xB0) onCC(d1, d2);
};

globalThis.onMidiMessageExternal = function (_d) { };

// ---------------- step buttons ----------------
function onStepDown(s) {
    heldStep = s;
    const t = selInstrTrack();
    if (instrType[t] === 0) {
        const lane = drumPat[t][focusSlot[t]];
        lane.on[s] = lane.on[s] ? 0 : 1;
        pushSeq(t, focusSlot[t]);
    } else {
        const lane = synthPat[t];
        lane.on[s] = lane.on[s] ? 0 : 1;
        pushSeq(t);
    }
    paintSteps();
}

// ---------------- CC handling ----------------
function onCC(cc, v) {
    if (cc === CC.SHIFT) { shift = (v === 127); return; }

    // Back: suspend (loops keep playing); Shift+Back: full exit
    if (cc === CC.BACK && v === 127) {
        if (shift) { host_exit_module(); }
        else if (typeof shadow_request_exit === 'function') { shadow_request_exit(); }
        else host_exit_module();
        return;
    }
    if (cc === CC.MENU && v === 127) { menuOpen = !menuOpen; displayDirty = true; paintButtons(); return; }

    // track select
    for (let i = 0; i < 4; i++) if (cc === TRACK_CC[i] && v === 127) {
        const want = shift ? (4 + i) : i;
        if (sel === want) sel = 8;              // press selected again -> master
        else sel = want;
        spb('sel', sel);
        pushPadmap(); paintAllPads(); paintButtons(); displayDirty = true;
        return;
    }

    // transport
    if (cc === CC.PLAY && v === 127) {
        if (shift) { spb('stopall', '1'); }
        else { const run = status && status.run === '1'; sp('play', run ? 0 : 1); }
        return;
    }
    if (cc === CC.MUTE && v === 127) {
        const t = sel < 8 ? sel : -1;
        if (t >= 0) { mutes[t] = mutes[t] ? 0 : 1; sp('mute', t + ':' + mutes[t]); }
        return;
    }
    if (cc === CC.UNDO && v === 127) {
        const c = lastClip(); if (c >= 0) spb('undo', c);
        return;
    }
    if (cc === CC.SAMPLE && v === 127) {
        // arm threshold-sampling into the focused slot of the selected drum track
        const t = selInstrTrack();
        if (instrType[t] === 0) spb('sample_slot', t + ':' + focusSlot[t]);
        return;
    }
    if (cc === CC.DELETE && v === 127) { const c = lastClip(); if (c >= 0) spb('delete', c); return; }

    // views
    if ((cc === CC.LEFT || cc === CC.RIGHT) && v === 127) {
        view = (view + (cc === CC.RIGHT ? 1 : 2)) % 3;
        pushPadmap(); paintAllPads(); displayDirty = true;
        return;
    }

    // encoders 1-8 -> track gains; master knob -> master gain
    if (cc >= CC.KNOB1 && cc <= CC.KNOB1 + 7 && v !== 0) {
        if (menuOpen) { onMenuTurn(cc - CC.KNOB1, decodeDelta(v)); return; }
        const t = cc - CC.KNOB1;
        gains[t] = clamp(gains[t] + decodeDelta(v) * GAIN_STEP, 0, 2);
        sp('gain', t + ':' + gains[t].toFixed(2));
        displayDirty = true;
        return;
    }
    if (cc === CC.MASTER && v !== 0) {
        masterGain = clamp(masterGain + decodeDelta(v) * GAIN_STEP, 0, 2);
        sp('master_gain', masterGain.toFixed(2));
        return;
    }

    // jog
    if (cc === CC.JOG && v !== 0) { onJog(decodeDelta(v)); return; }
    if (cc === CC.JOG_CLICK && v === 127) { onJogClick(); return; }

    // menu nav
    if (menuOpen && (cc === CC.UP || cc === CC.DOWN) && v === 127) {
        menuIdx = (menuIdx + (cc === CC.DOWN ? 1 : MENU.length - 1)) % MENU.length;
        displayDirty = true;
        return;
    }
}

function onMenuTurn(knob, delta) {
    const m = MENU[menuIdx];
    if (m.turn) m.turn(delta);
    displayDirty = true;
}

function onJog(delta) {
    if (heldStep >= 0) {
        // per-step probability
        const t = selInstrTrack();
        const lane = instrType[t] === 0 ? drumPat[t][focusSlot[t]] : synthPat[t];
        lane.prob[heldStep] = clamp(lane.prob[heldStep] + delta * 5, 0, 100);
        pushSeq(t, instrType[t] === 0 ? focusSlot[t] : undefined);
        paintSteps(); displayDirty = true;
        return;
    }
    if (menuOpen) {
        menuIdx = clamp(menuIdx + (delta > 0 ? 1 : -1), 0, MENU.length - 1);
        displayDirty = true; return;
    }
    // chord navigation: rotate through legal moves (preview; commit on click)
    if (moveList.length) {
        moveSel = ((moveSel + (delta > 0 ? 1 : -1)) % moveList.length + moveList.length) % moveList.length;
        displayDirty = true;
    }
}
function onJogClick() {
    if (menuOpen) { const m = MENU[menuIdx]; if (m.action) m.action(); else if (m.toggle) m.toggle(); displayDirty = true; return; }
    if (moveList.length && moveSel < moveList.length) {
        spb('chord', moveList[moveSel].node);   // commits at next bar in DSP
        displayDirty = true;
    }
}

// ---------------- helpers ----------------
function clamp(x, lo, hi) { return x < lo ? lo : (x > hi ? hi : x); }
function lastClip() {
    // best-effort: first playing/recording clip in the selected looper track
    if (sel >= 0 && sel < 4) {
        for (let s = 0; s < 4; s++) { const cs = clipState(sel*4+s); if (cs.state >= 2) return sel*4+s; }
        return sel*4;   // default to first clip of the track
    }
    return -1;
}
