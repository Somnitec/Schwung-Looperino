/*
 * Looperino UI — M0 input/latency spike.
 *
 * The DSP owns all audio state (pads toggle it on the audio thread); this file
 * only paints LEDs/display, runs the feedback guard, and maps encoder 1 to
 * monitor gain. Keep everything in this one file while iterating — imported
 * project files are cached for the life of shadow_ui (schwung-module-guide §6.2);
 * host shared/ modules are stable and safe to import.
 *
 * Bottom pad row: [68]=monitor  [69]=latency ping  [70]=test tone
 * Knob 1 (CC71): monitor gain. Back (CC51): exit.
 */

import { shouldFilterMessage, setLED, setButtonLED, decodeDelta }
    from '/data/UserData/schwung/shared/input_filter.mjs';

// ---- feel/tuning constants ----
const STATUS_POLL_TICKS = 16;   // ~30x/sec at the ~500 Hz overtake tick
const JACK_POLL_TICKS = 256;    // feedback-guard re-check, ~2x/sec
const GAIN_STEP = 0.02;

// LED palette (indexed, shared/constants.mjs)
const LED = {
    Black: 0, BrightRed: 1, BrightOrange: 3, VividYellow: 7,
    BrightGreen: 8, Cyan: 14, AzureBlue: 15, Purple: 22,
    White: 120, DarkGrey: 124, PureBlue: 125, Green: 126, Red: 127,
};

const PAD_MONITOR = 68;
const PAD_PING = 69;
const PAD_TONE = 70;
const CC_KNOB1 = 71;
const CC_BACK = 51;

const PING_STATE = { IDLE: 0, ARMED: 1, WAITING: 2, DONE: 3, TIMEOUT: 4 };

let tickCount = 0;
let status = null;          // parsed DSP status
let gain = 1.0;             // JS-side echo of monitor_gain (encoder target)
let allowMonitor = null;    // null = not yet decided
let displayDirty = true;

function parseStatus(s) {
    if (!s) return null;
    const out = {};
    for (const part of s.split(';')) {
        const eq = part.indexOf('=');
        if (eq > 0) out[part.slice(0, eq)] = part.slice(eq + 1);
    }
    return out;
}

function paintPads() {
    for (let note = 68; note <= 99; note++) {
        let color = LED.Black;
        if (note === PAD_MONITOR) {
            color = status && status.mon === '1'
                ? (status.allow === '1' ? LED.BrightGreen : LED.BrightRed)
                : LED.DarkGrey;
        } else if (note === PAD_PING) {
            const p = status ? Number(status.ping) : PING_STATE.IDLE;
            color = p === PING_STATE.WAITING ? LED.VividYellow
                : p === PING_STATE.DONE ? LED.AzureBlue
                : p === PING_STATE.TIMEOUT ? LED.BrightOrange
                : LED.DarkGrey;
        } else if (note === PAD_TONE) {
            color = status && status.tone === '1' ? LED.Purple : LED.DarkGrey;
        }
        setLED(note, color);
    }
}

function drawDisplay() {
    clear_screen();
    print(2, 0, 'LOOPERINO M0', 1);

    const mon = status && status.mon === '1';
    const blocked = mon && status && status.allow !== '1';
    print(2, 12, 'mon:' + (mon ? (blocked ? 'BLOCKED' : 'ON') : 'off')
        + ' g:' + gain.toFixed(2), 1);
    if (blocked) print(2, 22, 'speaker on - feedback', 1);

    const bpm = status ? Number(status.bpm) : 0;
    const run = status && status.run === '1';
    print(2, 32, 'bpm:' + (bpm > 0 ? bpm.toFixed(1) : '--')
        + (run ? ' >' : ' .') + ' t:' + (status ? status.ticks : 0), 1);

    const latMs = status ? Number(status.lat_ms) : -1;
    const p = status ? Number(status.ping) : 0;
    let latText = 'lat: --';
    if (p === PING_STATE.WAITING) latText = 'lat: pinging...';
    else if (p === PING_STATE.TIMEOUT) latText = 'lat: timeout';
    else if (latMs >= 0) latText = 'lat: ' + latMs.toFixed(2) + ' ms';
    print(2, 42, latText, 1);

    // input peak meter, bottom edge
    const peak = status ? Number(status.peak) : 0;
    const w = Math.min(124, Math.round((peak / 32767) * 124));
    draw_rect(2, 56, 124, 6, 1);
    if (w > 0) fill_rect(2, 56, w, 6, 1);
}

function refreshFeedbackGuard(force) {
    const safe = !(host_speaker_active() && !host_line_in_connected());
    const allow = safe ? 1 : 0;
    if (force || allowMonitor === null || allow !== allowMonitor) {
        allowMonitor = allow;
        host_module_set_param_blocking('allow_monitor', String(allow), 100);
    }
}

globalThis.init = function () {
    refreshFeedbackGuard(true);
    host_module_set_param_blocking('monitor_gain', gain.toFixed(2), 100);
    paintPads();
    setButtonLED(CC_BACK, 0x40); // Back is live (white LED, medium)
    displayDirty = true;
};

globalThis.onResume = function () {
    // host cleared LEDs while suspended; repaint everything
    paintPads();
    setButtonLED(CC_BACK, 0x40);
    displayDirty = true;
};

globalThis.tick = function () {
    tickCount++;
    if (tickCount % STATUS_POLL_TICKS === 0) {
        const s = parseStatus(host_module_get_param('status'));
        if (s) {
            status = s;
            paintPads();
            displayDirty = true; // peak meter animates; redraw at poll rate
        }
    }
    if (tickCount % JACK_POLL_TICKS === 0) refreshFeedbackGuard(false);
    if (displayDirty) {
        drawDisplay();
        displayDirty = false;
    }
};

globalThis.onMidiMessageInternal = function (data) {
    if (shouldFilterMessage(data)) return;
    const [statusByte, d1, d2] = data;
    const type = statusByte & 0xF0;

    if (type === 0xB0) {
        if (d1 === CC_KNOB1 && d2 !== 0) {
            const delta = decodeDelta(d2);
            gain = Math.min(4, Math.max(0, gain + delta * GAIN_STEP));
            host_module_set_param('monitor_gain', gain.toFixed(2));
            displayDirty = true;
        } else if (d1 === CC_BACK && d2 === 127) {
            host_exit_module();
        }
    }
    // Pad presses are handled by the DSP on the audio thread; the next status
    // poll picks up the change. Nothing to do here yet.
};

globalThis.onMidiMessageExternal = function (_data) { };
