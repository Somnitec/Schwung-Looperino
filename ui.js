/*
 * Looperoni — overtake UI for a live multitrack looper on Ableton Move.
 *
 * The 4x8 pad grid is a bank of loop cells. Each pad records / overdubs /
 * plays / stops one loop. All audio lives in the DSP (looperoni.so); this UI
 * only sends high-level commands (host_module_set_param) and polls the engine
 * state (host_module_get_param "ui") each frame to drive LEDs and the display.
 *
 *   Pad press      EMPTY -> record, REC -> play (commit), PLAY -> stop,
 *                  STOP -> play, ARMED -> cancel.
 *   Delete + pad   clear that loop.
 *   Record button  toggle overdub on the selected loop.
 *   Play button    stop-all / play-all toggle.
 *   Loop button    cycle quantize: Bar -> Beat -> Off.
 *   Up / Down      tempo +/- 1 BPM (Shift: +/- 10).
 *   Jog            select loop  (Shift+Jog: tempo).  Jog click: overdub sel.
 *   Knob 1         selected-loop volume.   Master knob / Knob 8: master volume.
 *   Knob 2         input monitor level.
 *   Exit overtake  Shift + Volume touch + Jog click  (handled by the host).
 */

import {
    Black, White, LightGrey, DarkGrey,
    BrightRed, RustRed, DeepRed,
    BrightGreen, ForestGreen, DeepGreen, DullGreen,
    VividYellow, Ochre, Mustard,
    PureBlue, SkyBlue,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MoveShift, MoveMainKnob, MoveUp, MoveDown,
    MovePlay, MoveRecord, MoveDelete, MoveLoop, MoveCapture,
    MovePads, MoveSteps,
    MoveKnob1, MoveKnob2, MoveKnob8, MoveMaster
} from '/data/UserData/schwung/shared/constants.mjs';

import {
    setLED, setButtonLED, clearAllLEDs, decodeDelta
} from '/data/UserData/schwung/shared/input_filter.mjs';

/* ---- loop states (must match looperoni.c) ---- */
const LOOP_EMPTY = 0, LOOP_ARMED = 1, LOOP_REC = 2,
      LOOP_PLAY = 3, LOOP_OVERDUB = 4, LOOP_STOP = 5;

const NUM_LOOPS = 32;
const JOG_CLICK_CC = 3;            /* jog wheel click */

/* ---- runtime state mirrored from the DSP ---- */
let states = new Array(NUM_LOOPS).fill(LOOP_EMPTY);
let bpm = 120.0;
let quant = 1;                     /* 0 off, 1 bar, 2 beat */
let barPhase = 0.0;
let masterSec = 0.0;
let sel = 0;

/* ---- local UI state ---- */
let shiftHeld = false;
let deleteHeld = false;
let frame = 0;
let dspOk = false;

/* knob absolute trackers (the engine takes 0..2 gains) */
let selVol = 1.0;
let masterVol = 0.9;
let monitor = 1.0;

/* progressive LED init to avoid USB-MIDI buffer overflow on entry */
let ledInitIndex = 0;
const LEDS_PER_FRAME = 8;
let ledReady = false;

/* ---------- DSP bridge ---------- */
function dspSet(key, val) {
    if (typeof host_module_set_param === 'function') {
        host_module_set_param(key, '' + val);
    }
}
function dspGet(key) {
    if (typeof host_module_get_param === 'function') {
        return host_module_get_param(key);
    }
    return null;
}

function pollDsp() {
    const s = dspGet('ui');
    if (!s) { dspOk = false; return; }
    dspOk = true;
    /* bpm|quant|barPhase|sel|masterSec|<32 chars> */
    const parts = ('' + s).split('|');
    if (parts.length < 6) return;
    bpm       = parseFloat(parts[0]) || bpm;
    quant     = parseInt(parts[1], 10);
    barPhase  = parseFloat(parts[2]) || 0;
    sel       = parseInt(parts[3], 10) || 0;
    masterSec = parseFloat(parts[4]) || 0;
    const chars = parts[5];
    for (let i = 0; i < NUM_LOOPS && i < chars.length; i++) {
        states[i] = chars.charCodeAt(i) - 48;
    }
}

/* ---------- LED colours per state ---------- */
function loopColor(i) {
    const st = states[i];
    const blinkSlow = (Math.floor(frame / 8) % 2) === 0;
    const blinkFast = (Math.floor(frame / 4) % 2) === 0;
    switch (st) {
        case LOOP_EMPTY:   return (i === sel) ? DarkGrey : Black;
        case LOOP_ARMED:   return blinkFast ? BrightRed : Black;
        case LOOP_REC:     return BrightRed;
        case LOOP_PLAY:    return BrightGreen;
        case LOOP_OVERDUB: return blinkSlow ? VividYellow : Ochre;
        case LOOP_STOP:    return (i === sel) ? DullGreen : DeepGreen;
        default:           return Black;
    }
}

function refreshPadLeds() {
    for (let i = 0; i < NUM_LOOPS; i++) {
        setLED(MovePads[i], loopColor(i));
    }
}

function refreshButtonLeds() {
    const anyPlaying = states.some(s => s === LOOP_PLAY || s === LOOP_OVERDUB);
    setButtonLED(MovePlay, anyPlaying ? BrightGreen : DarkGrey);
    setButtonLED(MoveRecord, states[sel] === LOOP_OVERDUB ? BrightRed : DarkGrey);
    setButtonLED(MoveLoop, quant === 0 ? DarkGrey : (quant === 1 ? SkyBlue : PureBlue));
    setButtonLED(MoveDelete, deleteHeld ? BrightRed : DarkGrey);
}

/* ---------- display ---------- */
const STATE_LABEL = ['empty', 'armed', 'REC', 'PLAY', 'DUB', 'stop'];

function drawDisplay() {
    clear_screen();

    /* Header */
    print(2, 2, 'Looperoni', 1);
    const qstr = quant === 0 ? 'FREE' : (quant === 1 ? 'BAR' : 'BEAT');
    const bpmStr = bpm.toFixed(0) + ' BPM ' + qstr;
    print(128 - bpmStr.length * 6 - 2, 2, bpmStr, 1);
    fill_rect(0, 12, 128, 1, 1);

    if (!dspOk) {
        print(2, 26, 'DSP not loaded', 1);
        print(2, 42, 'check looperoni.so', 1);
        return;
    }

    /* Selected loop line */
    const lbl = STATE_LABEL[states[sel]] || '?';
    print(2, 18, 'Loop ' + (sel + 1) + ': ' + lbl, 1);

    /* Counts */
    const playing = states.filter(s => s === LOOP_PLAY || s === LOOP_OVERDUB).length;
    const filled = states.filter(s => s !== LOOP_EMPTY).length;
    const lenStr = masterSec > 0 ? masterSec.toFixed(1) + 's' : '--';
    print(2, 34, playing + ' play / ' + filled + ' used   ' + lenStr, 1);

    /* Vol readout */
    print(2, 50, 'vol ' + Math.round(selVol * 100) + '%  mst ' +
                 Math.round(masterVol * 100) + '%', 1);

    /* Cycle playhead bar */
    fill_rect(0, 62, 128, 2, 0);
    const x = Math.round(barPhase * 127);
    fill_rect(0, 62, Math.max(1, x), 2, 1);
}

/* ---------- pad / button handling ---------- */
function onPad(note) {
    const i = note - MovePads[0];
    if (i < 0 || i >= NUM_LOOPS) return;
    sel = i;
    dspSet('sel', i);
    if (deleteHeld) {
        dspSet('clear', i);
    } else {
        dspSet('pad', i);
    }
}

function onButton(cc, on) {
    switch (cc) {
        case MoveShift:  shiftHeld = on; break;
        case MoveDelete: deleteHeld = on; break;
        case MoveRecord:
            if (on) dspSet('overdub', sel);
            break;
        case MovePlay:
            if (on) {
                const anyPlaying = states.some(s => s === LOOP_PLAY || s === LOOP_OVERDUB);
                dspSet(anyPlaying ? 'stopall' : 'playall', 1);
            }
            break;
        case MoveLoop:
            if (on) { quant = (quant + 1) % 3; dspSet('quant', quant); }
            break;
        case MoveCapture:
            if (on && shiftHeld) dspSet('clearall', 1);  /* Shift+Capture = wipe */
            break;
        case MoveUp:
            if (on) { bpm = Math.min(300, bpm + (shiftHeld ? 10 : 1)); dspSet('tempo', bpm.toFixed(1)); }
            break;
        case MoveDown:
            if (on) { bpm = Math.max(20, bpm - (shiftHeld ? 10 : 1)); dspSet('tempo', bpm.toFixed(1)); }
            break;
        case JOG_CLICK_CC:
            if (on) dspSet('overdub', sel);
            break;
    }
}

function onKnob(cc, value) {
    const d = decodeDelta(value);
    switch (cc) {
        case MoveMainKnob: /* jog */
            if (shiftHeld) {
                bpm = Math.max(20, Math.min(300, bpm + d));
                dspSet('tempo', bpm.toFixed(1));
            } else {
                sel = (sel + d + NUM_LOOPS) % NUM_LOOPS;
                dspSet('sel', sel);
            }
            break;
        case MoveKnob1:
            selVol = clamp01x2(selVol + d * 0.02);
            dspSet('vol', sel + ':' + selVol.toFixed(3));
            break;
        case MoveKnob2:
            monitor = clamp01(monitor + d * 0.02);
            dspSet('monitor', monitor.toFixed(3));
            break;
        case MoveKnob8:
        case MoveMaster:
            masterVol = clamp01x2(masterVol + d * 0.02);
            dspSet('master_vol', masterVol.toFixed(3));
            break;
    }
}

function clamp01(v)   { return v < 0 ? 0 : (v > 1 ? 1 : v); }
function clamp01x2(v) { return v < 0 ? 0 : (v > 2 ? 2 : v); }

/* ---------- lifecycle ---------- */
globalThis.init = function () {
    frame = 0;
    ledInitIndex = 0;
    ledReady = false;
    shiftHeld = false;
    deleteHeld = false;
    /* Push defaults into the engine. */
    dspSet('quant', quant);
    dspSet('tempo', bpm.toFixed(1));
    dspSet('master_vol', masterVol.toFixed(3));
    dspSet('monitor', monitor.toFixed(3));
    clear_screen();
    print(2, 26, 'Looperoni', 1);
    print(2, 42, 'loading...', 1);
};

globalThis.tick = function () {
    frame++;

    /* Progressive LED clear/init for the first ~5 frames. */
    if (!ledReady) {
        const end = Math.min(ledInitIndex + LEDS_PER_FRAME, NUM_LOOPS);
        for (let i = ledInitIndex; i < end; i++) setLED(MovePads[i], Black);
        ledInitIndex = end;
        if (ledInitIndex >= NUM_LOOPS) ledReady = true;
        return;
    }

    /* Poll engine state (cheap; one string). */
    pollDsp();

    /* LEDs refresh a few times a second is enough; pads animate on blink. */
    refreshPadLeds();
    refreshButtonLeds();

    /* Display: throttle to ~10 Hz to save the 1-bit panel some churn. */
    if (frame % 3 === 0) drawDisplay();
};

/* Internal hardware MIDI (pads, knobs, buttons). */
globalThis.onMidiMessageInternal = function (data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    if (status === MidiNoteOn && d2 > 0) {
        if (d1 >= MovePads[0] && d1 <= MovePads[NUM_LOOPS - 1]) { onPad(d1); return; }
    }
    if (status === MidiCC) {
        /* Knobs / jog are relative encoders; buttons are 127/0. */
        if (d1 === MoveMainKnob || d1 === MoveKnob1 || d1 === MoveKnob2 ||
            d1 === MoveKnob8 || d1 === MoveMaster) {
            onKnob(d1, d2);
        } else {
            onButton(d1, d2 >= 64);
        }
    }
};

/* External USB MIDI — ignored for now (could map a footswitch later). */
globalThis.onMidiMessageExternal = function (data) { /* reserved */ };
