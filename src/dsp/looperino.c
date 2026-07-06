/*
 * Looperino DSP — M0 input/latency spike.
 *
 * Exercises every integration point the looper will depend on, before any
 * looper code exists:
 *   - live input read from the SPI mailbox (line-in / mic)
 *   - input -> output monitoring (wet-vocal feasibility)
 *   - round-trip latency self-measurement (impulse out, detect on input;
 *     patch a cable from output to input, or use speaker->mic in a pinch)
 *   - MIDI clock (0xF8 on cable 0) -> BPM, the sample-accurate grid source
 *   - pad hits on the audio thread (the low-latency punch-in path)
 *   - the JS<->DSP param bridge (packed status string)
 *
 * Realtime rules (schwung docs/REALTIME_SAFETY.md): render_block/on_midi/
 * set_param run on the SPI thread — no allocation, no file I/O, no locks.
 * All state is fixed-size and set up in create_instance.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "schwung_plugin_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PING_THRESHOLD 4000      /* int16 input level that counts as impulse arrival */
#define PING_TIMEOUT_FRAMES (2 * MOVE_SAMPLE_RATE)
#define PING_BURST_FRAMES 8
#define TONE_HZ 440.0f
#define TONE_AMP 6000

static const host_api_v1_t *g_host = NULL;

enum ping_state { PING_IDLE, PING_ARMED, PING_WAITING, PING_DONE, PING_TIMEOUT };

typedef struct {
    /* monitoring */
    int monitor;            /* user intent (pad toggle) */
    int allow_monitor;      /* JS-side feedback guard; default 0 = blocked */
    float monitor_gain;

    /* test tone */
    int tone;
    float tone_phase;

    /* input metering (block peak, decayed for display) */
    int peak_in;

    /* latency ping */
    int ping_state;
    int64_t ping_wait_frames;
    int64_t latency_frames;  /* -1 = none/timeout */

    /* clock (magneto recipe: count ticks against a sample counter) */
    int64_t clk_frame;       /* frames rendered since load */
    int64_t last_tick_frame;
    float tick_interval;     /* smoothed frames per 0xF8 tick */
    float bpm;
    int ticks_seen;
    int transport_running;

    /* last pad seen on the audio thread (proves the punch-in path) */
    int last_pad;
    int64_t last_pad_frame;
} looperino_t;

static void *lp_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    looperino_t *lp = calloc(1, sizeof(looperino_t));
    if (!lp) return NULL;
    lp->monitor_gain = 1.0f;
    lp->latency_frames = -1;
    lp->last_pad = -1;
    lp->bpm = 0.0f;
    return lp;
}

static void lp_destroy_instance(void *instance) {
    free(instance);
}

static void lp_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    looperino_t *lp = instance;
    if (!lp) return;

    if (len == 1 && source == MOVE_MIDI_SOURCE_EXTERNAL) {
        switch (msg[0]) {
        case 0xF8: { /* clock tick, 24 ppqn */
            if (lp->last_tick_frame > 0) {
                float iv = (float)(lp->clk_frame - lp->last_tick_frame);
                if (iv > 0) {
                    if (lp->tick_interval <= 0) lp->tick_interval = iv;
                    else lp->tick_interval += 0.15f * (iv - lp->tick_interval);
                    lp->bpm = (MOVE_SAMPLE_RATE * 60.0f) / (lp->tick_interval * 24.0f);
                }
            }
            lp->last_tick_frame = lp->clk_frame;
            lp->ticks_seen++;
            break;
        }
        case 0xFA: case 0xFB: lp->transport_running = 1; break;
        case 0xFC: lp->transport_running = 0; break;
        }
        return;
    }

    if (len == 3 && source == MOVE_MIDI_SOURCE_INTERNAL) {
        uint8_t status = msg[0] & 0xF0;
        uint8_t note = msg[1];
        uint8_t vel = msg[2];
        if (status == 0x90 && vel > 0 && note >= 68 && note <= 99) {
            lp->last_pad = note;
            lp->last_pad_frame = lp->clk_frame;
            /* M0 control pads (bottom row). DSP owns the state; JS only paints. */
            if (note == 68) lp->monitor = !lp->monitor;
            else if (note == 69 && lp->ping_state != PING_WAITING) lp->ping_state = PING_ARMED;
            else if (note == 70) lp->tone = !lp->tone;
        }
    }
}

static void lp_set_param(void *instance, const char *key, const char *val) {
    looperino_t *lp = instance;
    if (!lp || !key || !val) return;
    if (!strcmp(key, "monitor")) lp->monitor = atoi(val) ? 1 : 0;
    else if (!strcmp(key, "allow_monitor")) lp->allow_monitor = atoi(val) ? 1 : 0;
    else if (!strcmp(key, "monitor_gain")) {
        float g = strtof(val, NULL);
        if (g >= 0.0f && g <= 4.0f) lp->monitor_gain = g;
    }
    else if (!strcmp(key, "tone")) lp->tone = atoi(val) ? 1 : 0;
    else if (!strcmp(key, "ping") && lp->ping_state != PING_WAITING) lp->ping_state = PING_ARMED;
    else if (!strcmp(key, "reset_latency")) { lp->latency_frames = -1; lp->ping_state = PING_IDLE; }
}

static int lp_get_param(void *instance, const char *key, char *buf, int buf_len) {
    looperino_t *lp = instance;
    if (!lp || !key || !buf || buf_len <= 0) return -1;
    if (!strcmp(key, "status")) {
        float lat_ms = lp->latency_frames >= 0
            ? (float)lp->latency_frames * 1000.0f / MOVE_SAMPLE_RATE : -1.0f;
        int n = snprintf(buf, (size_t)buf_len,
            "mon=%d;allow=%d;gain=%.2f;tone=%d;peak=%d;"
            "bpm=%.1f;ticks=%d;run=%d;"
            "ping=%d;lat_ms=%.2f;pad=%d",
            lp->monitor, lp->allow_monitor, (double)lp->monitor_gain, lp->tone, lp->peak_in,
            (double)lp->bpm, lp->ticks_seen, lp->transport_running,
            lp->ping_state, (double)lat_ms, lp->last_pad);
        return n > 0 && n < buf_len ? n : -1;
    }
    return -1;
}

static int lp_get_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void lp_render_block(void *instance, int16_t *out, int frames) {
    looperino_t *lp = instance;
    if (!lp || !out || !g_host || !g_host->mapped_memory) return;

    const int16_t *in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));

    /* input block peak, decayed ~6dB/s for the display meter */
    int peak = 0;
    for (int i = 0; i < frames * 2; i++) {
        int a = in[i] < 0 ? -in[i] : in[i];
        if (a > peak) peak = a;
    }
    lp->peak_in = peak > lp->peak_in ? peak : (int)(lp->peak_in * 0.995f);

    /* latency ping: emit burst, then count frames until it shows up on input.
     * Monitor is suppressed while waiting so a loopback cable can't feed back. */
    int monitoring = lp->monitor && lp->allow_monitor && lp->ping_state != PING_WAITING;

    if (lp->ping_state == PING_WAITING) {
        for (int i = 0; i < frames; i++) {
            int a = in[2 * i] < 0 ? -in[2 * i] : in[2 * i];
            int b = in[2 * i + 1] < 0 ? -in[2 * i + 1] : in[2 * i + 1];
            if (a > PING_THRESHOLD || b > PING_THRESHOLD) {
                lp->latency_frames = lp->ping_wait_frames + i;
                lp->ping_state = PING_DONE;
                break;
            }
        }
        if (lp->ping_state == PING_WAITING) {
            lp->ping_wait_frames += frames;
            if (lp->ping_wait_frames > PING_TIMEOUT_FRAMES) {
                lp->ping_state = PING_TIMEOUT;
                lp->latency_frames = -1;
            }
        }
    } else if (lp->ping_state == PING_ARMED) {
        for (int i = 0; i < PING_BURST_FRAMES && i < frames; i++) {
            int16_t s = (i & 1) ? -24000 : 24000;
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
        lp->ping_wait_frames = 0;
        lp->ping_state = PING_WAITING;
    }

    if (monitoring) {
        for (int i = 0; i < frames * 2; i++)
            out[i] = clamp16((int32_t)out[i] + (int32_t)(in[i] * lp->monitor_gain));
    }

    if (lp->tone) {
        float step = 2.0f * (float)M_PI * TONE_HZ / MOVE_SAMPLE_RATE;
        for (int i = 0; i < frames; i++) {
            int16_t s = (int16_t)(sinf(lp->tone_phase) * TONE_AMP);
            lp->tone_phase += step;
            if (lp->tone_phase > 2.0f * (float)M_PI) lp->tone_phase -= 2.0f * (float)M_PI;
            out[2 * i] = clamp16((int32_t)out[2 * i] + s);
            out[2 * i + 1] = clamp16((int32_t)out[2 * i + 1] + s);
        }
    }

    lp->clk_frame += frames;
}

static plugin_api_v2_t g_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = lp_create_instance,
    .destroy_instance = lp_destroy_instance,
    .on_midi = lp_on_midi,
    .set_param = lp_set_param,
    .get_param = lp_get_param,
    .get_error = lp_get_error,
    .render_block = lp_render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
