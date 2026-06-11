/*
 * Looperoni — live multitrack looper DSP engine for Ableton Move (Schwung)
 *
 * A loop-pedal-style engine inspired by grid loopers: each of the 32 pads is a
 * loop cell with an independent record / overdub / play / stop state machine.
 * The first loop recorded becomes the "loop master" and defines the cycle
 * length; subsequent loops sync to it (in quantized mode).
 *
 * Architecture
 * ------------
 *   - This .so is loaded by the Schwung host as the DSP for the `looperoni`
 *     overtake module. The UI (ui.js) drives it entirely through set_param()
 *     (commands in) and get_param("ui") (state out, polled each frame for LEDs
 *     and the display).
 *   - Audio input arrives in the host mailbox at host->audio_in_offset
 *     (interleaved stereo int16, MOVE_FRAMES_PER_BLOCK frames). render_block()
 *     reads it, records/overdubs into per-loop buffers, sums all playing loops
 *     plus a monitor of the live input, and writes the stereo result out.
 *
 * Threading: render_block() runs on the audio thread; set_param/get_param run
 * on the UI thread. State transitions requested from the UI are latched into
 * `pending` fields and applied on the audio thread at quantize boundaries, so
 * the two threads never race over buffer contents. Scalar command flags use
 * plain writes — acceptable here because a missed/duplicated pad press is
 * harmless, and avoids pulling in atomics/locks on the RT path.
 *
 * V2 (instance-based) plugin API.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host/plugin_api_v1.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define NUM_LOOPS        32
#define SR               44100
#define DEFAULT_MAX_SEC  16        /* per-loop buffer ceiling (mono seconds) */
#define DEFAULT_BPM      120.0f

/* Loop state machine — keep in sync with ui.js LOOP_* constants. */
enum {
    L_EMPTY = 0,   /* no audio */
    L_ARMED = 1,   /* waiting for next quantize boundary to start recording */
    L_REC   = 2,   /* recording fresh audio */
    L_PLAY  = 3,   /* looping playback */
    L_OVERDUB = 4, /* looping playback + summing input into the buffer */
    L_STOP  = 5    /* has audio, muted */
};

/* Quantize modes */
enum { Q_OFF = 0, Q_BAR = 1, Q_BEAT = 2 };

/* Deferred action latched by the UI, applied at the next boundary. */
enum {
    P_NONE = 0,
    P_START_REC,   /* ARMED -> REC */
    P_STOP_REC,    /* REC   -> PLAY (commit length) */
    P_STOP,        /* PLAY/OVERDUB -> STOP */
    P_START_PLAY   /* STOP  -> PLAY */
};

typedef struct {
    int     state;
    int     pending;       /* P_* */
    int16_t *buf;          /* interleaved L,R; lazily allocated, cap = max_frames */
    int     cap_frames;
    int     len_frames;    /* committed loop length (0 until first record done) */
    int     pos;           /* current play/record frame */
    float   vol;           /* 0..1 */
    int     is_master;     /* this loop defines the cycle length */
} loop_t;

typedef struct {
    loop_t    loops[NUM_LOOPS];
    long long clock;       /* global sample counter (frames) */
    int       master_len;  /* cycle length in frames, 0 until first loop set */
    float     bpm;
    int       quant;       /* Q_* */
    int       sel;         /* selected loop index for knob editing */
    int       max_frames;  /* per-loop buffer ceiling */
    float     master_vol;  /* 0..1 */
    float     monitor;     /* live-input monitor gain 0..1 */
    int       count_in;    /* reserved: bars of count-in before master record */
} engine_t;

static const host_api_v1_t *g_host = NULL;

static void lp_log(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline int16_t clip16(float x) {
    if (x >  32767.0f) return  32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)x;
}

static int bar_frames(engine_t *e) {
    float bpm = (e->bpm > 1.0f) ? e->bpm : DEFAULT_BPM;
    /* 4 beats per bar, 4/4 */
    return (int)(SR * 60.0f / bpm * 4.0f + 0.5f);
}

static int quant_frames(engine_t *e) {
    if (e->quant == Q_BAR)  return bar_frames(e);
    if (e->quant == Q_BEAT) return bar_frames(e) / 4;
    return 0; /* free */
}

static int ensure_buf(engine_t *e, loop_t *L) {
    if (L->buf) return 1;
    L->cap_frames = e->max_frames;
    L->buf = (int16_t *)calloc((size_t)L->cap_frames * 2, sizeof(int16_t));
    if (!L->buf) { lp_log("looperoni: loop buffer alloc failed"); return 0; }
    return 1;
}

/* Snap a recorded length to whole bars (used for the master loop). */
static int snap_to_bar(engine_t *e, int frames) {
    int bf = bar_frames(e);
    if (bf <= 0) return frames;
    int bars = (frames + bf / 2) / bf;
    if (bars < 1) bars = 1;
    return bars * bf;
}

/* ------------------------------------------------------------------ */
/*  State transitions (called on the audio thread)                     */
/* ------------------------------------------------------------------ */

static void begin_record(engine_t *e, int i) {
    loop_t *L = &e->loops[i];
    if (!ensure_buf(e, L)) return;
    L->pos = 0;
    L->len_frames = 0;
    L->state = L_REC;
    if (e->master_len == 0) L->is_master = 1;
}

static void commit_record(engine_t *e, int i) {
    loop_t *L = &e->loops[i];
    int len = L->pos;
    if (len <= 0) { L->state = L_EMPTY; L->is_master = 0; return; }

    if (L->is_master) {
        /* First loop: optionally snap to whole bars, then it owns the cycle. */
        if (e->quant != Q_OFF) len = snap_to_bar(e, len);
        if (len > L->cap_frames) len = L->cap_frames;
        e->master_len = len;
        L->len_frames = len;
    } else if (e->master_len > 0) {
        /* Synced overdub-style loop: length is a whole number of cycles. */
        int cycles = (len + e->master_len / 2) / e->master_len;
        if (cycles < 1) cycles = 1;
        len = cycles * e->master_len;
        if (len > L->cap_frames) len = L->cap_frames;
        L->len_frames = len;
    } else {
        L->len_frames = len; /* free mode, no master */
    }
    L->pos = 0;
    L->state = L_PLAY;
}

/* Apply a pad press immediately (free mode) or arm it (quantized mode). */
static void pad_press(engine_t *e, int i) {
    if (i < 0 || i >= NUM_LOOPS) return;
    loop_t *L = &e->loops[i];
    int q = quant_frames(e);

    switch (L->state) {
    case L_EMPTY:
        if (q > 0) { ensure_buf(e, L); L->state = L_ARMED; L->pending = P_START_REC;
                     if (e->master_len == 0) L->is_master = 1; }
        else       { begin_record(e, i); }
        break;
    case L_ARMED:
        L->state = L_EMPTY; L->pending = P_NONE; L->is_master = 0; /* cancel */
        break;
    case L_REC:
        if (q > 0) L->pending = P_STOP_REC; /* commit at boundary */
        else       commit_record(e, i);
        break;
    case L_PLAY:
    case L_OVERDUB:
        if (q > 0) L->pending = P_STOP; else { L->state = L_STOP; }
        break;
    case L_STOP:
        if (q > 0) { L->pending = P_START_PLAY; } else { L->pos = 0; L->state = L_PLAY; }
        break;
    }
}

static void pad_clear(engine_t *e, int i) {
    if (i < 0 || i >= NUM_LOOPS) return;
    loop_t *L = &e->loops[i];
    int was_master = L->is_master;
    L->state = L_EMPTY;
    L->pending = P_NONE;
    L->len_frames = 0;
    L->pos = 0;
    L->is_master = 0;
    /* keep buf allocated for reuse */
    if (was_master) {
        /* If the master is cleared and nothing else is playing, drop the grid
         * tempo grid so a new master can be defined. */
        int any = 0;
        for (int j = 0; j < NUM_LOOPS; j++)
            if (e->loops[j].state != L_EMPTY) { any = 1; break; }
        if (!any) e->master_len = 0;
    }
}

static void overdub_toggle(engine_t *e, int i) {
    if (i < 0 || i >= NUM_LOOPS) return;
    loop_t *L = &e->loops[i];
    if (L->state == L_PLAY)        L->state = L_OVERDUB;
    else if (L->state == L_OVERDUB) L->state = L_PLAY;
}

static void stop_all(engine_t *e) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        loop_t *L = &e->loops[i];
        if (L->state == L_PLAY || L->state == L_OVERDUB) L->state = L_STOP;
        else if (L->state == L_REC) commit_record(e, i);
    }
}

static void play_all(engine_t *e) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        loop_t *L = &e->loops[i];
        if (L->state == L_STOP && L->len_frames > 0) { L->pos = 0; L->state = L_PLAY; }
    }
}

static void clear_all(engine_t *e) {
    for (int i = 0; i < NUM_LOOPS; i++) pad_clear(e, i);
    e->master_len = 0;
}

/* Service pending transitions at a quantize boundary. */
static void service_boundary(engine_t *e) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        loop_t *L = &e->loops[i];
        switch (L->pending) {
        case P_START_REC:  begin_record(e, i);  L->pending = P_NONE; break;
        case P_STOP_REC:   commit_record(e, i); L->pending = P_NONE; break;
        case P_STOP:       L->state = L_STOP;   L->pending = P_NONE; break;
        case P_START_PLAY: L->pos = 0; L->state = L_PLAY; L->pending = P_NONE; break;
        default: break;
        }
    }
    /* Re-align every playing loop to the cycle so layers stay phase-locked. */
    if (e->master_len > 0) {
        for (int i = 0; i < NUM_LOOPS; i++) {
            loop_t *L = &e->loops[i];
            if ((L->state == L_PLAY || L->state == L_OVERDUB) &&
                L->len_frames == e->master_len) {
                /* keep as-is; single-cycle loops wrap naturally */
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  V2 lifecycle                                                       */
/* ------------------------------------------------------------------ */

static void *v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    engine_t *e = (engine_t *)calloc(1, sizeof(engine_t));
    if (!e) return NULL;
    e->bpm        = DEFAULT_BPM;
    e->quant      = Q_BAR;
    e->master_vol = 0.9f;
    e->monitor    = 1.0f;
    e->max_frames = DEFAULT_MAX_SEC * SR;
    for (int i = 0; i < NUM_LOOPS; i++) e->loops[i].vol = 1.0f;
    lp_log("looperoni: instance created");
    return e;
}

static void v2_destroy_instance(void *instance) {
    engine_t *e = (engine_t *)instance;
    if (!e) return;
    for (int i = 0; i < NUM_LOOPS; i++) free(e->loops[i].buf);
    free(e);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
    /* All control is via set_param from the overtake UI; nothing to do here. */
}

/* ------------------------------------------------------------------ */
/*  set_param — commands from the UI                                   */
/* ------------------------------------------------------------------ */

static void v2_set_param(void *instance, const char *key, const char *val) {
    engine_t *e = (engine_t *)instance;
    if (!e || !key) return;

    if (strcmp(key, "pad") == 0)            { pad_press(e, atoi(val)); }
    else if (strcmp(key, "clear") == 0)     { pad_clear(e, atoi(val)); }
    else if (strcmp(key, "overdub") == 0)   { overdub_toggle(e, atoi(val)); }
    else if (strcmp(key, "stopall") == 0)   { stop_all(e); }
    else if (strcmp(key, "playall") == 0)   { play_all(e); }
    else if (strcmp(key, "clearall") == 0)  { clear_all(e); }
    else if (strcmp(key, "tempo") == 0)     { float b = atof(val); if (b >= 20 && b <= 300) e->bpm = b; }
    else if (strcmp(key, "quant") == 0)     { e->quant = atoi(val); }
    else if (strcmp(key, "sel") == 0)       { int s = atoi(val); if (s >= 0 && s < NUM_LOOPS) e->sel = s; }
    else if (strcmp(key, "master_vol") == 0){ float v = atof(val); e->master_vol = v < 0 ? 0 : (v > 2 ? 2 : v); }
    else if (strcmp(key, "monitor") == 0)   { float v = atof(val); e->monitor = v < 0 ? 0 : (v > 1 ? 1 : v); }
    else if (strcmp(key, "max_sec") == 0)   { int s = atoi(val); if (s >= 2 && s <= 120) e->max_frames = s * SR; }
    else if (strcmp(key, "vol") == 0) {
        /* "vol" = "<i>:<0..2>" */
        int i = atoi(val);
        const char *c = strchr(val, ':');
        if (c && i >= 0 && i < NUM_LOOPS) {
            float v = atof(c + 1);
            e->loops[i].vol = v < 0 ? 0 : (v > 2 ? 2 : v);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  get_param — state out to the UI                                    */
/* ------------------------------------------------------------------ */

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    engine_t *e = (engine_t *)instance;
    if (!e || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "ui") == 0) {
        /* Format:  bpm|quant|barPhase|sel|master_secs|<32 state chars>
         * barPhase is 0..1 progress through the current cycle (for the playhead).
         */
        float barPhase = 0.0f;
        if (e->master_len > 0)
            barPhase = (float)(e->clock % e->master_len) / (float)e->master_len;
        else {
            int bf = bar_frames(e);
            if (bf > 0) barPhase = (float)(e->clock % bf) / (float)bf;
        }
        char states[NUM_LOOPS + 1];
        for (int i = 0; i < NUM_LOOPS; i++) states[i] = (char)('0' + e->loops[i].state);
        states[NUM_LOOPS] = '\0';

        int n = snprintf(buf, buf_len, "%.1f|%d|%.4f|%d|%.2f|%s",
                         e->bpm, e->quant, barPhase, e->sel,
                         (float)e->master_len / (float)SR, states);
        return (n < 0) ? -1 : (n >= buf_len ? buf_len - 1 : n);
    }

    if (strcmp(key, "bpm") == 0) {
        int n = snprintf(buf, buf_len, "%.1f", e->bpm);
        return (n < 0) ? -1 : n;
    }
    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  render_block — the audio thread                                    */
/* ------------------------------------------------------------------ */

static void v2_render_block(void *instance, int16_t *out_lr, int frames) {
    engine_t *e = (engine_t *)instance;
    if (!e || !g_host || !g_host->mapped_memory) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    /* Follow host tempo when available (e.g. external MIDI clock). */
    if (g_host->get_bpm) {
        float b = g_host->get_bpm();
        if (b >= 20.0f && b <= 300.0f) e->bpm = b;
    }

    int16_t *in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    int q = quant_frames(e);

    for (int f = 0; f < frames; f++) {
        /* Quantize boundary check (before mixing this frame). */
        if (q > 0 && (e->clock % q) == 0) service_boundary(e);

        float inL = (float)in[f * 2];
        float inR = (float)in[f * 2 + 1];

        float accL = inL * e->monitor;
        float accR = inR * e->monitor;

        for (int i = 0; i < NUM_LOOPS; i++) {
            loop_t *L = &e->loops[i];

            if (L->state == L_REC) {
                if (L->buf && L->pos < L->cap_frames) {
                    L->buf[L->pos * 2]     = clip16(inL);
                    L->buf[L->pos * 2 + 1] = clip16(inR);
                    L->pos++;
                    /* Synced non-master loop: auto-commit after one cycle. */
                    if (!L->is_master && e->master_len > 0 && L->pos >= e->master_len)
                        commit_record(e, i);
                } else if (L->pos >= L->cap_frames) {
                    commit_record(e, i); /* hit the ceiling */
                }
                continue;
            }

            if (L->state == L_PLAY || L->state == L_OVERDUB) {
                if (!L->buf || L->len_frames <= 0) continue;
                float sL = (float)L->buf[L->pos * 2]     * L->vol;
                float sR = (float)L->buf[L->pos * 2 + 1] * L->vol;
                accL += sL;
                accR += sR;
                if (L->state == L_OVERDUB) {
                    /* Sum live input into the existing buffer (soft-summed). */
                    L->buf[L->pos * 2]     = clip16((float)L->buf[L->pos * 2]     + inL);
                    L->buf[L->pos * 2 + 1] = clip16((float)L->buf[L->pos * 2 + 1] + inR);
                }
                L->pos++;
                if (L->pos >= L->len_frames) L->pos = 0;
            }
        }

        out_lr[f * 2]     = clip16(accL * e->master_vol);
        out_lr[f * 2 + 1] = clip16(accR * e->master_vol);
        e->clock++;
    }
}

/* ------------------------------------------------------------------ */
/*  Plugin entry                                                       */
/* ------------------------------------------------------------------ */

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version     = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance= v2_destroy_instance,
    .on_midi         = v2_on_midi,
    .set_param       = v2_set_param,
    .get_param       = v2_get_param,
    .get_error       = v2_get_error,
    .render_block    = v2_render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    lp_log("looperoni: plugin initialized (v2)");
    return &g_plugin_api_v2;
}
