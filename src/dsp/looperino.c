/*
 * looperino.c — the core: timeline/grid/tempo, clip state machines + layered
 * undo, pad-gesture classification (audio-thread punch-in path), the audio
 * graph (input chain -> loopers + instruments + punch FX -> master), sequencer
 * and chord clocks, and the JS<->DSP param bridge.
 *
 * Realtime: render_block / on_midi / set_param all run on the SPI thread. No
 * allocation, no locks. The only file I/O is save/load, reached from set_param
 * (accepted hiccup, "save while not performing").
 *
 * The design contract lives in docs/v1-spec.md. Everything feel-related is a
 * constant in looperino.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "looperino.h"
#include "tonalmap.h"

static const host_api_v1_t *g_host = NULL;

/* xorshift32 — cheap RT-safe randomness for step probability */
float lp_randf(looperino_t *lp) {
    uint32_t x = lp->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    lp->rng = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

/* ============================ lifecycle ============================ */
static void lp_destroy(void *inst);
static int16_t *cz16(size_t n) { return (int16_t*)calloc(n, sizeof(int16_t)); }
static float   *czf(size_t n)  { return (float*)calloc(n, sizeof(float)); }

static void *lp_create(const char *module_dir, const char *json) {
    (void)json;
    looperino_t *lp = (looperino_t*)calloc(1, sizeof(looperino_t));
    if (!lp) return NULL;
    if (module_dir) { strncpy(lp->module_dir, module_dir, sizeof(lp->module_dir)-1); }

    /* clip bases + layer pool */
    int ok = 1;
    for (int i = 0; i < N_CLIPS; i++) {
        lp->clips[i].base = cz16((size_t)CLIP_FRAMES*2);
        lp->clips[i].rec_layer = -1;
        lp->clips[i].copy_src = -1;
        if (!lp->clips[i].base) ok = 0;
    }
    for (int i = 0; i < LAYER_POOL; i++) {
        lp->pool[i] = cz16((size_t)CLIP_FRAMES*2);
        if (!lp->pool[i]) ok = 0;
    }
    /* drum slots (all instr tracks, all slots) */
    for (int t = 0; t < N_INSTR_TRACKS; t++)
        for (int s = 0; s < KIT_SLOTS; s++) {
            lp->instr[t].slots[s].buf = cz16((size_t)SLOT_FRAMES);
            lp->instr[t].slots[s].gain = 1.0f;
            if (!lp->instr[t].slots[s].buf) ok = 0;
        }
    /* chain + punch memory */
    float *rev_mem = czf(CHAIN_REV_MEM_FLOATS);
    float *dly_mem = czf(CHAIN_DLY_MEM_FLOATS);
    float *pdly    = czf(PUNCH_DLY_MEM_FLOATS);
    int16_t *pfreeze = cz16((size_t)PUNCH_FREEZE_FRAMES*2);
    if (!rev_mem || !dly_mem || !pdly || !pfreeze) ok = 0;
    if (!ok) {
        /* ~74 MB didn't fit; free what we have (chain/punch mem too) and fail
         * the load cleanly rather than NULL-deref in render. */
        free(rev_mem); free(dly_mem); free(pdly); free(pfreeze);
        lp->chain.dly.buf = NULL; lp->chain.rev.comb[0][0] = NULL; lp->pfx.dly.buf = NULL; lp->pfx.ring = NULL;
        lp_destroy(lp);
        return NULL;
    }
    chain_init(&lp->chain, rev_mem, dly_mem);
    punch_init(&lp->pfx, pfreeze, pdly);

    /* defaults */
    lp->monitor_gain = 1.0f;
    lp->master_gain = 1.0f;
    lp->rng = 0x1234567u;
    lp->sel = 0;
    lp->input_route = 3;              /* vocal until a track is selected */
    lp->fixed_bpm = 100.0f;
    lp->cur_step = -1;
    lp->rec_clip = -1;
    lp->chord_node = 0; lp->home_node = 0; lp->chord_pending = TM_NONE;
    for (int i = 0; i < N_STEPS; i++) { lp->chordlane[i] = 0xFF; lp->fxlane[i] = 0xFF; }
    for (int t = 0; t < N_TRACKS; t++) { lp->gain[t] = 1.0f; lp->gain_cur[t] = 1.0f; }
    /* default instrument types: 0,1 drums; 2,3 synth */
    lp->instr[0].type = IT_DRUM; lp->instr[1].type = IT_DRUM;
    lp->instr[2].type = IT_SYNTH; lp->instr[3].type = IT_SYNTH;
    lp->instr[2].follow = 2; lp->instr[3].follow = 2;
    /* synth steps default to 0xFF = "use chord root" (else calloc's 0 = sub-bass) */
    for (int t = 0; t < N_INSTR_TRACKS; t++)
        for (int s = 0; s < N_STEPS; s++) lp->instr[t].synth_note[s] = 0xFF;
    for (int i = 0; i < 16; i++) lp->note_pads[i] = 0;
    tonal_pad_notes(lp->chord_node, 48, lp->note_pads);   /* base C3 */

    for (int i = 0; i < 32; i++) lp->pad[i].clip = -1;
    return lp;
}

static void lp_destroy(void *inst) {
    looperino_t *lp = (looperino_t*)inst;
    if (!lp) return;
    for (int i = 0; i < N_CLIPS; i++) free(lp->clips[i].base);
    for (int i = 0; i < LAYER_POOL; i++) free(lp->pool[i]);
    for (int t = 0; t < N_INSTR_TRACKS; t++)
        for (int s = 0; s < KIT_SLOTS; s++) free(lp->instr[t].slots[s].buf);
    free(lp->chain.dly.buf);
    /* reverb mem: comb[0][0] is the head of the carved block */
    free(lp->chain.rev.comb[0][0]);
    free(lp->pfx.dly.buf);
    free(lp->pfx.ring);
    free(lp);
}

/* ============================ pool ============================ */
static int pool_alloc(looperino_t *lp) {
    for (int i = 0; i < LAYER_POOL; i++)
        if (!lp->pool_used[i]) { lp->pool_used[i] = 1;
            memset(lp->pool[i], 0, (size_t)CLIP_FRAMES*2*sizeof(int16_t));
            return i; }
    return -1;
}
static void pool_free(looperino_t *lp, int16_t *buf) {
    for (int i = 0; i < LAYER_POOL; i++)
        if (lp->pool[i] == buf) { lp->pool_used[i] = 0; return; }
}

/* ============================ grid / tempo ============================ */
static int bars_from_len(int frames, float *out_bar_frames) {
    /* pick bars in {1,2,4,8,16} whose implied BPM is closest to target,
     * clamped to [min,max]. bar = 4 beats. */
    static const int CAND[] = {1,2,4,8,16};
    float sec = (float)frames / SR;
    int best_bars = 1; float best_err = 1e9f; float best_bf = frames;
    for (unsigned i = 0; i < sizeof(CAND)/sizeof(CAND[0]); i++) {
        int bars = CAND[i];
        float bpm = (bars * 4.0f * 60.0f) / sec;      /* beats/sec*60 */
        if (bpm < FIRSTLOOP_MIN_BPM || bpm > FIRSTLOOP_MAX_BPM) continue;
        float err = fabsf(bpm - FIRSTLOOP_TARGET_BPM);
        if (err < best_err) { best_err = err; best_bars = bars; best_bf = (float)frames/bars; }
    }
    if (best_err > 1e8f) {   /* nothing in range: pick the closer legal extreme */
        float bpm1 = (1 * 4.0f * 60.0f) / sec;   /* 1-bar implied tempo */
        if (bpm1 < FIRSTLOOP_MIN_BPM) { best_bars = 16; best_bf = (float)frames/16; }
        else { best_bars = 1; best_bf = (float)frames; }
    }
    *out_bar_frames = best_bf;
    return best_bars;
}

/* returns the chosen bar count so the caller doesn't re-derive it (avoids a
 * floor/round mismatch that dropped a bar off the first loop). */
static int establish_grid(looperino_t *lp, int rec_frames, int64_t rec_start) {
    float bf;
    int bars = bars_from_len(rec_frames, &bf);
    lp->bar_frames = (int)(bf + 0.5f);
    if (lp->bar_frames < 1) lp->bar_frames = 1;
    lp->grid_origin = rec_start;
    lp->grid = 1;
    lp->bpm = (4.0f * 60.0f * SR) / lp->bar_frames;
    lp->last_step_abs = -1; lp->last_bar_abs = -1;
    return bars;
}

/* ============================ clips ============================ */
/* current play position of a clip, phase-locked to the grid */
static int clip_pos(const looperino_t *lp, const clip_t *c) {
    if (c->len <= 0) return 0;
    int64_t d = lp->timeline - c->anchor;
    int64_t p = d % c->len;
    if (p < 0) p += c->len;
    return (int)p;
}

static void clip_stop_other_in_row(looperino_t *lp, int track, int keep_slot) {
    for (int s = 0; s < CLIPS_PER_TRACK; s++) {
        if (s == keep_slot) continue;
        clip_t *c = &lp->clips[track*CLIPS_PER_TRACK + s];
        if (c->playing) c->playing = 0;   /* gain_cur ramps it out */
    }
}

/* begin an initial-take recording on an empty clip */
static void rec_begin(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (c->len > 0 || lp->rec_clip >= 0) return;
    lp->rec_clip = clipidx;
    c->rec_layer = -1;
    c->rec_write = 0;
    c->fresh = 1;
    if (!lp->grid) {
        c->state = CS_RECORDING;
        c->rec_start = lp->timeline;
    } else {
        /* quantize start to next bar */
        int64_t rel = lp->timeline - lp->grid_origin;
        int64_t next_bar = ((rel / lp->bar_frames) + 1) * lp->bar_frames;
        c->rec_start = lp->grid_origin + next_bar;
        c->state = CS_ARMED;
    }
}

/* finalize an initial take: called from the end-tap */
static void rec_end_tap(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (lp->rec_clip != clipidx) return;
    if (c->state == CS_ARMED) {          /* never actually started; cancel */
        c->state = CS_EMPTY; lp->rec_clip = -1; return;
    }
    if (!lp->grid) {
        /* first loop defines the grid */
        int n = c->rec_write;
        if (n < SR/4) { c->state = CS_EMPTY; c->len = 0; lp->rec_clip = -1; return; }
        int bars = establish_grid(lp, n, c->rec_start);   /* use the chosen count */
        if (bars < 1) bars = 1;
        c->len = bars * lp->bar_frames;
        if (c->len > CLIP_FRAMES) c->len = CLIP_FRAMES;
        c->bars = bars;
        c->anchor = c->rec_start;
        c->state = CS_PLAYING; c->playing = 1; c->gain_cur = 0.0f;
        lp->rec_clip = -1;
    } else {
        /* round to nearest bar boundary */
        int written = c->rec_write;
        int bar = lp->bar_frames;
        int lo = (written / bar) * bar;
        int hi = lo + bar;
        int nearer_lo = (written - lo < hi - written && lo >= bar);
        if (hi > CLIP_FRAMES) nearer_lo = 1;   /* no room to extend: truncate */
        if (nearer_lo) {
            /* nearer the lower boundary (or no buffer room): truncate now */
            c->len = lo; c->bars = lo/bar;
            c->anchor = c->rec_start;
            c->state = CS_PLAYING; c->playing = 1; c->gain_cur = 0.0f;
            clip_stop_other_in_row(lp, clipidx/CLIPS_PER_TRACK, clipidx%CLIPS_PER_TRACK);
            lp->rec_clip = -1;
        } else {
            /* nearer the upper boundary: keep recording to it */
            c->state = CS_ENDQ;
            c->endq_target = hi;
        }
    }
}

/* begin an overdub layer on a filled clip (called when a hold is confirmed) */
static int overdub_begin(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (c->len <= 0 || lp->rec_clip >= 0) return 0;
    if (c->n_layers >= MAX_LAYERS) return 0;       /* v1: deny when full */
    int pi = pool_alloc(lp);
    if (pi < 0) return 0;                            /* v1: deny on pool exhaustion */
    int li = c->n_layers;
    /* a prior undo kept the old top buffer for redo; a new overdub replaces it,
     * so release it back to the pool before reassigning (else it's orphaned). */
    if (c->layers[li].buf) pool_free(lp, c->layers[li].buf);
    c->redo_valid = 0;
    c->layers[li].buf = lp->pool[pi];
    c->layers[li].in_mix = 0;                        /* silent until committed */
    c->layers[li].start_pos = clip_pos(lp, c);
    c->layers[li].written = 0;
    c->rec_layer = li;
    c->redo_valid = 0;                               /* new overdub kills redo */
    lp->rec_clip = clipidx;
    c->state = CS_OVERDUB;
    if (!c->playing) { c->playing = 1; }
    return 1;
}

static void overdub_commit(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (lp->rec_clip != clipidx || c->rec_layer < 0) return;
    c->layers[c->rec_layer].in_mix = 1;
    c->n_layers++;
    c->fresh = 0;
    c->rec_layer = -1;
    lp->rec_clip = -1;
    c->state = CS_PLAYING;
}

static void clip_undo(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (c->len <= 0) return;
    if (c->n_layers > 0) {
        /* peel the top layer (toggle out); keep for redo */
        c->layers[c->n_layers-1].in_mix = 0;
        c->n_layers--;
        c->redo_valid = 1;
    } else if (c->fresh) {
        /* undo on a fresh just-recorded clip clears it */
        pool_free(lp, NULL); /* no-op safety */
        c->len = 0; c->state = CS_EMPTY; c->playing = 0;
    }
}
static void clip_redo(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (c->redo_valid && c->n_layers < MAX_LAYERS && c->layers[c->n_layers].buf) {
        c->layers[c->n_layers].in_mix = 1;
        c->n_layers++;
        c->redo_valid = 0;
    }
}

static void clip_delete(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    for (int i = 0; i < MAX_LAYERS; i++)
        if (c->layers[i].buf) { pool_free(lp, c->layers[i].buf); c->layers[i].buf = NULL; }
    c->len = 0; c->n_layers = 0; c->redo_valid = 0; c->playing = 0;
    c->state = CS_EMPTY; c->fresh = 0; c->rec_layer = -1;
    if (lp->rec_clip == clipidx) lp->rec_clip = -1;
}

static void clip_toggle_play(looperino_t *lp, int clipidx) {
    clip_t *c = &lp->clips[clipidx];
    if (c->len <= 0) return;
    if (c->playing) { c->playing = 0; }
    else {
        clip_stop_other_in_row(lp, clipidx/CLIPS_PER_TRACK, clipidx%CLIPS_PER_TRACK);
        c->playing = 1;
    }
}

/* Queue a copy of source -> empty target. The O(len) flatten is NOT done here
 * (this is reachable from on_midi, the audio thread) — it's amortized across
 * render blocks by copy_tick(). The target claims its length immediately but
 * stays silent (CS_COPYING) until materialized. */
static void clip_copy(looperino_t *lp, int src, int dst) {
    if (src == dst || src < 0 || src >= N_CLIPS || dst < 0 || dst >= N_CLIPS) return;
    clip_t *s = &lp->clips[src], *d = &lp->clips[dst];
    if (s->len <= 0 || d->len > 0) return;
    d->len = s->len; d->bars = s->bars; d->anchor = lp->grid_origin;
    d->n_layers = 0; d->fresh = 0; d->playing = 0; d->gain_cur = 0.0f;
    d->copy_src = src; d->copy_cursor = 0; d->state = CS_COPYING;
}

/* materialize pending copies, COPY_FRAMES_PER_BLOCK frames/block (RT-bounded) */
static void copy_tick(looperino_t *lp) {
    for (int i = 0; i < N_CLIPS; i++) {
        clip_t *d = &lp->clips[i];
        if (d->state != CS_COPYING) continue;
        int src = d->copy_src;
        if (src < 0 || src >= N_CLIPS || lp->clips[src].len < d->len) {
            /* source vanished/shrank mid-copy: abandon */
            d->state = CS_EMPTY; d->len = 0; d->copy_src = -1; continue;
        }
        clip_t *s = &lp->clips[src];
        int end = d->copy_cursor + COPY_FRAMES_PER_BLOCK;
        if (end > d->len) end = d->len;
        for (int f = d->copy_cursor; f < end; f++) {
            int32_t l = s->base[2*f], r = s->base[2*f+1];
            for (int k = 0; k < MAX_LAYERS; k++)
                if (s->layers[k].buf && s->layers[k].in_mix) {
                    l += s->layers[k].buf[2*f]; r += s->layers[k].buf[2*f+1];
                }
            d->base[2*f]   = l> 32767?32767:(l<-32768?-32768:(int16_t)l);
            d->base[2*f+1] = r> 32767?32767:(r<-32768?-32768:(int16_t)r);
        }
        d->copy_cursor = end;
        if (end >= d->len) { d->state = CS_STOPPED; d->copy_src = -1; }
    }
}

/* ============================ MIDI ============================ */
static void handle_clock(looperino_t *lp, uint8_t b) {
    switch (b) {
    case 0xF8:
        if (lp->last_tick_frame > 0) {
            float iv = (float)(lp->timeline - lp->last_tick_frame);
            if (iv > 4.0f && iv < SR) {
                if (lp->tick_interval <= 0) lp->tick_interval = iv;
                else lp->tick_interval += 0.15f * (iv - lp->tick_interval);
                lp->ext_bpm = (SR * 60.0f) / (lp->tick_interval * 24.0f);
            }
        }
        lp->last_tick_frame = lp->timeline;
        lp->ticks_seen++;
        break;
    case 0xFA: case 0xFB: lp->tick_interval = 0; lp->last_tick_frame = 0; break;
    case 0xFC: break;
    }
}

/* pad gesture entry points (called from on_midi note on/off) */
static void pad_on(looperino_t *lp, int note);
static void pad_off(looperino_t *lp, int note);

static void lp_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    looperino_t *lp = (looperino_t*)inst;
    if (!lp) return;
    if (len == 1 && source == MOVE_MIDI_SOURCE_EXTERNAL) { handle_clock(lp, msg[0]); return; }
    if (len == 3 && source == MOVE_MIDI_SOURCE_INTERNAL) {
        uint8_t st = msg[0] & 0xF0, note = msg[1], vel = msg[2];
        if (note < 68 || note > 99) return;
        if (st == 0x90 && vel > 0) pad_on(lp, note);
        else if (st == 0x80 || (st == 0x90 && vel == 0)) pad_off(lp, note);
    }
}

/* ---- gesture classification ---- */
static void act_clip_press(looperino_t *lp, int clipidx, int padi);

static void pad_on(looperino_t *lp, int note) {
    int padi = note - 68;
    uint8_t role = lp->padmap[padi] >> 4;
    uint8_t val  = lp->padmap[padi] & 0x0F;
    lp->pad[padi].down = 1;
    lp->pad[padi].down_frame = lp->timeline;
    lp->pad[padi].was_hold = 0;
    lp->pad[padi].consumed = 0;

    switch (role) {
    case ROLE_CLIP: {
        int clipidx = val;
        lp->pad[padi].clip = clipidx;
        /* copy gesture: another clip pad already held past HOLD? */
        for (int j = 0; j < 32; j++) {
            if (j == padi || !lp->pad[j].down) continue;
            if ((lp->padmap[j] >> 4) == ROLE_CLIP && lp->pad[j].was_hold) {
                clip_copy(lp, lp->pad[j].clip, clipidx);
                lp->pad[j].consumed = 1;      /* abort that hold's overdub */
                if (lp->rec_clip == lp->pad[j].clip) { /* cancel pending overdub */
                    clip_t *sc = &lp->clips[lp->pad[j].clip];
                    if (sc->rec_layer >= 0) { pool_free(lp, sc->layers[sc->rec_layer].buf);
                        sc->layers[sc->rec_layer].buf = NULL; sc->rec_layer = -1;
                        sc->state = CS_PLAYING; }
                    lp->rec_clip = -1;
                }
                lp->pad[padi].consumed = 1;
                return;
            }
        }
        act_clip_press(lp, clipidx, padi);
        break;
    }
    case ROLE_DRUM: {
        int slot = val;
        int track = (lp->sel >= N_LOOP_TRACKS && lp->sel < N_TRACKS) ? lp->sel - N_LOOP_TRACKS : 0;
        lp->instr[track].focus_slot = slot;         /* focus for step-edit + sampling */
        kit_trigger_drum(lp, track, slot, 1.0f, 0);
        break;
    }
    case ROLE_NOTE: {
        int track = (lp->sel >= N_LOOP_TRACKS && lp->sel < N_TRACKS) ? lp->sel - N_LOOP_TRACKS : 0;
        int gi = val;
        if (gi >= 0 && gi < 16) kit_synth_on(lp, track, lp->note_pads[gi], 1.0f, -1);
        break;
    }
    case ROLE_FX: {
        if (val < PFX_COUNT)                          /* only 0..5 are real FX */
            punch_press(&lp->pfx, val, 8, lp->bar_frames > 0 ? lp->bar_frames : SR);
        break;
    }
    default: break;
    }
}

static void act_clip_press(looperino_t *lp, int clipidx, int padi) {
    clip_t *c = &lp->clips[clipidx];
    lp->last_touched_clip = clipidx;
    /* end-tap of an in-progress initial take on this clip */
    if (lp->rec_clip == clipidx &&
        (c->state == CS_RECORDING || c->state == CS_ARMED || c->state == CS_ENDQ)) {
        rec_end_tap(lp, clipidx);
        lp->pad[padi].consumed = 1;
        return;
    }
    if (c->len <= 0) {
        /* empty: press begins recording (release irrelevant) */
        rec_begin(lp, clipidx);
        lp->pad[padi].consumed = 1;
        return;
    }
    /* filled clip: hold vs 1/2/3-tap. We do NOT act on this press — the action
     * is deferred to gesture resolution (release + TAP_MS window / HOLD_MS) so
     * there is nothing to un-toggle. Cost: single-tap play/stop lags by up to
     * TAP_MS. Lower TAP_MS if that feels sluggish on device. */
    int64_t now = lp->timeline;
    if (now - lp->pad[padi].last_tap_frame <= MS2FRAMES(TAP_MS))
        lp->pad[padi].taps++;
    else
        lp->pad[padi].taps = 1;
    lp->pad[padi].last_tap_frame = now;
}

static void pad_off(looperino_t *lp, int note) {
    int padi = note - 68;
    if (!lp->pad[padi].down) return;
    lp->pad[padi].down = 0;
    lp->pad[padi].up_frame = lp->timeline;
    uint8_t role = lp->padmap[padi] >> 4;
    uint8_t val  = lp->padmap[padi] & 0x0F;

    if (role == ROLE_NOTE) {
        int track = (lp->sel >= N_LOOP_TRACKS && lp->sel < N_TRACKS) ? lp->sel - N_LOOP_TRACKS : 0;
        if (val < 16) kit_synth_off(lp, track, lp->note_pads[val]);
        return;
    }
    if (role == ROLE_FX) { punch_release(&lp->pfx, val); return; }
    if (role != ROLE_CLIP) return;
    if (lp->pad[padi].consumed) return;

    int clipidx = lp->pad[padi].clip;
    if (clipidx < 0 || clipidx >= N_CLIPS) return;   /* padmap remapped mid-hold */
    clip_t *c = &lp->clips[clipidx];
    int64_t held = lp->pad[padi].up_frame - lp->pad[padi].down_frame;

    if (lp->pad[padi].was_hold) {
        /* an overdub was in progress: commit it */
        if (lp->rec_clip == clipidx && c->rec_layer >= 0) overdub_commit(lp, clipidx);
        lp->pad[padi].was_hold = 0;
        lp->pad[padi].taps = 0;             /* a hold is not a tap */
        return;
    }
    if (held >= MS2FRAMES(HOLD_MS)) {
        /* held past HOLD_MS but no overdub started (denied: pool/layer full) —
         * not a tap, do nothing (no spurious play/stop toggle). */
        lp->pad[padi].taps = 0;
        return;
    }
    /* genuine short tap: taps counted on press; gestures_tick finalizes after
     * the TAP_MS window closes. */
}

/* promote presses that have been held long enough into overdubs; finalize
 * multi-taps whose window has closed. Called once per block from render. */
static void gestures_tick(looperino_t *lp) {
    int64_t now = lp->timeline;
    for (int padi = 0; padi < 32; padi++) {
        uint8_t role = lp->padmap[padi] >> 4;
        if (role != ROLE_CLIP) continue;
        int clipidx = lp->pad[padi].clip;
        if (clipidx < 0) continue;
        clip_t *c = &lp->clips[clipidx];
        /* held past HOLD_MS on a filled clip, not yet overdubbing -> start.
         * No transport was toggled on press, so nothing to revert. */
        if (lp->pad[padi].down && !lp->pad[padi].was_hold && !lp->pad[padi].consumed &&
            c->len > 0 && (now - lp->pad[padi].down_frame) >= MS2FRAMES(HOLD_MS) &&
            lp->rec_clip < 0) {
            if (overdub_begin(lp, clipidx)) {
                lp->pad[padi].was_hold = 1;
                lp->pad[padi].taps = 0;
            }
        }
        /* multi-tap window closed -> act now (deferred; no reverts) */
        if (!lp->pad[padi].down && lp->pad[padi].taps > 0 &&
            (now - lp->pad[padi].last_tap_frame) > MS2FRAMES(TAP_MS)) {
            int taps = lp->pad[padi].taps;
            lp->pad[padi].taps = 0;
            if (taps == 1)      clip_toggle_play(lp, clipidx);   /* play/stop */
            else if (taps == 2) { if (c->redo_valid) clip_redo(lp, clipidx);
                                  else clip_undo(lp, clipidx); } /* undo <-> redo */
            else                clip_delete(lp, clipidx);        /* 3+ = delete */
        }
    }
}

/* ============================ params ============================ */
static int hexval(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }

static void set_padmap(looperino_t *lp, const char *hex) {
    for (int i = 0; i < 32 && hex[2*i] && hex[2*i+1]; i++)
        lp->padmap[i] = (uint8_t)((hexval(hex[2*i])<<4) | hexval(hex[2*i+1]));
}

static void set_steplane(steplane_t *ln, const char *hex) {
    /* 16 steps: 2 hex per step for prob? Use: first 16 hex nibbles = on bits in
     * 4 hex chars (16 bits), then optional per-step prob as 32 hex chars.
     * Simpler wire: on = 4 hex (16 bits), prob = 32 hex (2 each). */
    uint16_t on = 0;
    for (int i = 0; i < 4 && hex[i]; i++) on = (on<<4) | hexval(hex[i]);
    for (int s = 0; s < N_STEPS; s++) {
        ln->on[s] = (on >> (15 - s)) & 1;
        ln->prob[s] = 100;
    }
    const char *p = hex + 4;
    if (*p == ':') {
        p++;
        for (int s = 0; s < N_STEPS && p[0] && p[1]; s++, p += 2)
            ln->prob[s] = (uint8_t)((hexval(p[0])<<4)|hexval(p[1]));
    }
}

static void lp_set_param(void *inst, const char *key, const char *val) {
    looperino_t *lp = (looperino_t*)inst;
    if (!lp || !key || !val) return;

    if (!strcmp(key,"padmap")) { set_padmap(lp, val); return; }
    if (!strcmp(key,"sel")) {
        int s = atoi(val); lp->sel = s;
        lp->input_route = (s >= 0 && s < N_LOOP_TRACKS) ? s : 3;
        chain_set_preset(&lp->chain, lp->input_route, lp->bpm>20?lp->bpm:100.0f);
        return;
    }
    if (!strcmp(key,"mon")) { lp->monitor = atoi(val)?1:0; return; }
    if (!strcmp(key,"allow_monitor")) { lp->allow_monitor = atoi(val)?1:0; return; }
    if (!strcmp(key,"monitor_gain")) { float g=strtof(val,0); if(g>=0&&g<=4) lp->monitor_gain=g; return; }
    if (!strcmp(key,"master_gain")) { float g=strtof(val,0); if(g>=0&&g<=2) lp->master_gain=g; return; }
    if (!strcmp(key,"metro")) { lp->metro = atoi(val)?1:0; return; }
    if (!strcmp(key,"play")) {
        lp->seq_run = atoi(val)?1:0;
        if (!lp->seq_run) kit_all_notes_off(lp);
        return;
    }
    if (!strcmp(key,"stopall")) {
        for (int i=0;i<N_CLIPS;i++) lp->clips[i].playing = 0;
        return;
    }
    if (!strcmp(key,"clear_all")) {
        for (int i=0;i<N_CLIPS;i++) clip_delete(lp,i);
        lp->grid=0; lp->bar_frames=0; lp->bpm=0; lp->cur_step=-1;
        kit_all_notes_off(lp);
        return;
    }
    if (!strcmp(key,"tempo_mode")) { lp->tempo_fixed = atoi(val)?1:0; return; }
    if (!strcmp(key,"tempo")) {
        float b=strtof(val,0); if(b>=40&&b<=240){ lp->fixed_bpm=b;
            if (lp->tempo_fixed && !lp->grid){ lp->bar_frames=(int)(4.0f*60.0f*SR/b+0.5f);
                lp->grid=1; lp->grid_origin=lp->timeline; lp->bpm=b;
                lp->last_step_abs=-1; lp->last_bar_abs=-1; } }
        return;
    }
    if (!strcmp(key,"gain")) {           /* "track:value" */
        int t=atoi(val); const char*c=strchr(val,':');
        if (c && t>=0 && t<N_TRACKS) lp->gain[t]=strtof(c+1,0);
        return;
    }
    if (!strcmp(key,"mute")) {           /* "track:0/1" */
        int t=atoi(val); const char*c=strchr(val,':');
        if (c && t>=0 && t<N_TRACKS) lp->mute[t]=atoi(c+1)?1:0;
        return;
    }
    if (!strcmp(key,"follow")) {         /* "track:mode" instr-relative 0-3 */
        int t=atoi(val); const char*c=strchr(val,':');
        if (c && t>=0 && t<N_INSTR_TRACKS) lp->instr[t].follow=atoi(c+1);
        return;
    }
    if (!strcmp(key,"octave")) {         /* "track:oct" */
        int t=atoi(val); const char*c=strchr(val,':');
        if (c && t>=0 && t<N_INSTR_TRACKS) lp->instr[t].octave=atoi(c+1);
        return;
    }
    if (!strncmp(key,"seq",3)) {         /* seq<t>_<slot> = steplane; drums */
        int t = key[3]-'0';
        const char *us = strchr(key+4,'_');
        if (t>=0 && t<N_INSTR_TRACKS) {
            if (us) { int slot=atoi(us+1); if(slot>=0&&slot<KIT_SLOTS) set_steplane(&lp->instr[t].drums[slot], val); }
            else set_steplane(&lp->instr[t].synth, val);
        }
        return;
    }
    if (!strncmp(key,"note",4)) {        /* note<t> = 16 bytes hex, synth step notes */
        int t = key[4]-'0';
        if (t>=0 && t<N_INSTR_TRACKS)
            for (int s=0;s<N_STEPS && val[2*s] && val[2*s+1];s++)
                lp->instr[t].synth_note[s] = (uint8_t)((hexval(val[2*s])<<4)|hexval(val[2*s+1]));
        return;
    }
    if (!strcmp(key,"chord")) {          /* commit chord node now (or pending) */
        int n=atoi(val); if(n>=0&&n<TONAL_NODES){ lp->chord_pending=(uint8_t)n; }
        return;
    }
    if (!strcmp(key,"home")) { int n=atoi(val); if(n>=0&&n<TONAL_NODES) lp->home_node=(uint8_t)n; return; }
    if (!strcmp(key,"chordlane")) {      /* 32 hex = 16 bars node ids */
        for (int s=0;s<N_STEPS && val[2*s] && val[2*s+1];s++)
            lp->chordlane[s]=(uint8_t)((hexval(val[2*s])<<4)|hexval(val[2*s+1]));
        return;
    }
    if (!strcmp(key,"fxlane")) {
        for (int s=0;s<N_STEPS && val[2*s] && val[2*s+1];s++)
            lp->fxlane[s]=(uint8_t)((hexval(val[2*s])<<4)|hexval(val[2*s+1]));
        return;
    }
    if (!strcmp(key,"fxtarget")) { lp->fx_lane_target=atoi(val); return; }
    if (!strcmp(key,"undo")) { int c=atoi(val); if(c>=0&&c<N_CLIPS){ clip_t*cl=&lp->clips[c];
        if (cl->redo_valid) clip_redo(lp,c); else clip_undo(lp,c);} return; }
    if (!strcmp(key,"delete")) { int c=atoi(val); if(c>=0&&c<N_CLIPS) clip_delete(lp,c); return; }
    if (!strcmp(key,"copy")) { int s=atoi(val); const char*c=strchr(val,':');
        if(c) clip_copy(lp,s,atoi(c+1));
        return; }
    if (!strcmp(key,"itype")) { int t=atoi(val); const char*c=strchr(val,':');
        if(c&&t>=0&&t<N_INSTR_TRACKS) lp->instr[t].type=atoi(c+1)?IT_SYNTH:IT_DRUM;
        return; }

    /* drum-slot sampling */
    if (!strcmp(key,"sample_slot")) {    /* "track:slot" arm threshold capture */
        int t=atoi(val); const char*c=strchr(val,':');
        int s = c ? atoi(c+1) : -1;
        if (c && t>=0 && t<N_INSTR_TRACKS && s>=0 && s<KIT_SLOTS) {
            lp->samp_track=t; lp->samp_slot=s;
            lp->samp_state=1; lp->samp_pos=0; lp->samp_quiet=0; lp->samp_pre_pos=0; }
        return;
    }
    if (!strcmp(key,"sample_cancel")) { lp->samp_state=0; return; }

    /* save/load (accepted hiccup) */
    if (!strcmp(key,"save_clip")) {      /* "clip:/path" flatten+write */
        int c=atoi(val); const char*p=strchr(val,':');
        if (p && c>=0 && c<N_CLIPS && lp->clips[c].len>0) {
            /* flatten into a scratch via layers; write base only for v1 */
            wav_write_stereo16(p+1, lp->clips[c].base, lp->clips[c].len);
        }
        return;
    }
    if (!strcmp(key,"load_clip")) {      /* "clip:/path" */
        int c=atoi(val); const char*p=strchr(val,':');
        if (p && c>=0 && c<N_CLIPS) {
            int n = wav_read_into(p+1, lp->clips[c].base, CLIP_FRAMES, 2);
            if (n > 0) { clip_t*cl=&lp->clips[c]; cl->len=n; cl->n_layers=0;
                cl->bars = lp->bar_frames>0? (n+lp->bar_frames/2)/lp->bar_frames : 1;
                cl->anchor=lp->grid_origin; cl->state=CS_STOPPED; cl->playing=0; }
        }
        return;
    }
    if (!strcmp(key,"save_slot")) {      /* "track:slot:/path" */
        int t=atoi(val); const char*a=strchr(val,':');
        if (a){ int s=atoi(a+1); const char*b=strchr(a+1,':');
            if (b && t>=0 && t<N_INSTR_TRACKS && s>=0 && s<KIT_SLOTS && lp->instr[t].slots[s].len>0)
                wav_write_stereo16(b+1, lp->instr[t].slots[s].buf, lp->instr[t].slots[s].len/2); }
        return;
    }
    if (!strcmp(key,"load_slot")) {
        int t=atoi(val); const char*a=strchr(val,':');
        if (a){ int s=atoi(a+1); const char*b=strchr(a+1,':');
            if (b && t>=0 && t<N_INSTR_TRACKS && s>=0 && s<KIT_SLOTS){
                int n=wav_read_into(b+1, lp->instr[t].slots[s].buf, SLOT_FRAMES, 1);
                if (n>0){ lp->instr[t].slots[s].len=n; lp->instr[t].slots[s].gain=1.0f; } } }
        return;
    }
}

static int lp_get_param(void *inst, const char *key, char *buf, int buf_len) {
    looperino_t *lp = (looperino_t*)inst;
    if (!lp || !key || !buf) return -1;
    if (!strcmp(key,"status")) {
        /* clips: 3 hex each = state,pos(0-15),layers */
        char clips[N_CLIPS*3+1]; int ci=0;
        for (int i=0;i<N_CLIPS;i++){
            clip_t*c=&lp->clips[i];
            int pos = (c->len>0)? (clip_pos(lp,c)*16/c->len) : 0; if(pos>15)pos=15;
            int st = c->state;
            const char*H="0123456789abcdef";
            clips[ci++]=H[st&15]; clips[ci++]=H[pos&15]; clips[ci++]=H[c->n_layers&15];
        }
        clips[ci]=0;
        char kits[N_INSTR_TRACKS*4+1]; int ki=0;
        for (int t=0;t<N_INSTR_TRACKS;t++){ uint16_t m=0;
            for(int s=0;s<KIT_SLOTS;s++) if(lp->instr[t].slots[s].len>0) m|=(1<<s);
            const char*H="0123456789abcdef";
            kits[ki++]=H[(m>>12)&15];kits[ki++]=H[(m>>8)&15];kits[ki++]=H[(m>>4)&15];kits[ki++]=H[m&15];
        }
        kits[ki]=0;
        int fitrk = (lp->sel>=N_LOOP_TRACKS && lp->sel<N_TRACKS) ? lp->sel-N_LOOP_TRACKS : 0;
        int n = snprintf(buf, buf_len,
            "bpm=%.1f;bar=%d;grid=%d;run=%d;step=%d;peak=%d;out=%d;mon=%d;samp=%d;"
            "chord=%d;pend=%d;home=%d;fx=%d;sel=%d;ext=%.1f;rec=%d;focus=%d;"
            "clips=%s;kits=%s",
            lp->bpm, lp->bar_frames, lp->grid, lp->seq_run, lp->cur_step,
            lp->peak_in, lp->peak_out, lp->monitor, lp->samp_state,
            lp->chord_node, lp->chord_pending==TM_NONE?-1:lp->chord_pending, lp->home_node,
            lp->pfx.active, lp->sel, lp->ext_bpm, lp->rec_clip, lp->instr[fitrk].focus_slot,
            clips, kits);
        return (n>0 && n<buf_len)? n : -1;
    }
    if (!strcmp(key,"chordname")) {
        int n = lp->chord_node < TONAL_NODES ? lp->chord_node : 0;
        int w = snprintf(buf, buf_len, "%s", TONAL_NAMES[n]);
        return w>0&&w<buf_len?w:-1;
    }
    if (!strcmp(key,"moves")) {
        /* legal moves from current node: "node:dotted,..." for the UI */
        tonal_move_t mv[TONAL_MAX_MOVES];
        int m = tonal_moves(lp->chord_node, mv);
        int off=0;
        for (int i=0;i<m;i++){ int w=snprintf(buf+off,buf_len-off,"%s%d:%d",i?",":"",mv[i].node,mv[i].dotted);
            if(w<0||w>=buf_len-off) break;
            off+=w; }
        if (m==0){ buf[0]=0; return 0; }
        return off;
    }
    return -1;
}

static int lp_get_error(void *inst, char *buf, int n){ (void)inst;(void)buf;(void)n; return 0; }

/* ============================ render ============================ */
static void capture_input(looperino_t *lp, const int16_t *in, int frames);
static void render_clips(looperino_t *lp, int frames);
static void run_clocks(looperino_t *lp, int frames);

static void lp_render(void *inst, int16_t *out, int frames) {
    looperino_t *lp = (looperino_t*)inst;
    if (!lp || !out || !g_host || !g_host->mapped_memory) { if(out) memset(out,0,frames*2*sizeof(int16_t)); return; }
    const int16_t *in = (const int16_t*)(g_host->mapped_memory + g_host->audio_in_offset);

    /* input peak meter */
    int peak=0; for (int i=0;i<frames*2;i++){ int a=in[i]<0?-in[i]:in[i]; if(a>peak)peak=a; }
    lp->peak_in = peak>lp->peak_in?peak:(int)(lp->peak_in*0.9f);

    /* input chain -> wet */
    chain_process(&lp->chain, in, lp->wet_l, lp->wet_r, frames, lp->bpm>20?lp->bpm:100.0f);

    /* gestures + clocks (fire sequencer/chord at boundaries within this block) */
    gestures_tick(lp);
    run_clocks(lp, frames);
    copy_tick(lp);                 /* amortized clip-copy materialization */

    /* capture recording (initial take / overdub) from wet */
    capture_input(lp, in, frames);

    /* clear track buffers */
    for (int t=0;t<N_TRACKS;t++) memset(lp->tbuf[t], 0, sizeof(float)*2*frames);

    /* loop clips -> tbuf[0..3] */
    render_clips(lp, frames);
    /* instruments -> tbuf[4..7] */
    kit_render(lp, frames);

    int64_t grid_pos = lp->grid ? (lp->timeline - lp->grid_origin) : 0;

    /* mix master */
    float *m = lp->master;
    memset(m, 0, sizeof(float)*2*frames);
    for (int t=0;t<N_TRACKS;t++){
        float tg = lp->mute[t]?0.0f:lp->gain[t];
        lp->gain_cur[t] += 0.01f*(tg - lp->gain_cur[t]);
        float g = lp->gain_cur[t];
        for (int i=0;i<frames*2;i++) m[i] += lp->tbuf[t][i]*g;
    }

    /* v1 punch FX target the master bus: keep the history ring warm, then
     * apply the active FX in place. */
    punch_feed(&lp->pfx, m, frames);
    if (lp->pfx.active != PFX_NONE || lp->fx_step_active)
        punch_process(&lp->pfx, m, frames, lp->bar_frames>0?lp->bar_frames:SR, grid_pos);

    /* monitor (forced during recording so you hear what you print) */
    int rec_active = lp->rec_clip >= 0;
    int mon = (lp->monitor || rec_active) && lp->allow_monitor;
    if (mon) {
        float g = lp->monitor_gain;
        for (int i=0;i<frames;i++){ m[2*i]+=lp->wet_l[i]*g; m[2*i+1]+=lp->wet_r[i]*g; }
    }

    /* metronome */
    if (lp->metro && lp->grid) {
        int beat = lp->bar_frames/4; if (beat<1) beat=1;
        for (int i=0;i<frames;i++){
            int64_t rel = lp->timeline + i - lp->grid_origin;
            if (rel>=0 && (rel % beat)==0) lp->metro_left = MS2FRAMES(25);
            if (lp->metro_left>0){ lp->metro_phase += 2.0f*3.14159f*1500.0f/SR;
                float s = sinf(lp->metro_phase)*METRO_GAIN*(lp->metro_left/(float)MS2FRAMES(25));
                m[2*i]+=s; m[2*i+1]+=s; lp->metro_left--; }
        }
    }

    /* master gain + soft clip -> out */
    int opeak=0;
    for (int i=0;i<frames*2;i++){
        float v = soft_clip(m[i]*lp->master_gain);
        int16_t s = f2i16(v);
        out[i]=s; int a=s<0?-s:s; if(a>opeak)opeak=a;
    }
    lp->peak_out = opeak>lp->peak_out?opeak:(int)(lp->peak_out*0.9f);

    lp->timeline += frames;
}

/* record wet input into the active clip (initial take or overdub layer) */
static void capture_input(looperino_t *lp, const int16_t *in, int frames) {
    (void)in;
    /* drum-slot sampling first (uses raw input, mono) */
    if (lp->samp_state) {
        const int16_t *raw = in;
        slot_t *sl = &lp->instr[lp->samp_track].slots[lp->samp_slot];
        for (int i=0;i<frames;i++){
            int16_t mono = (int16_t)((raw[2*i]+raw[2*i+1])/2);
            int a = mono<0?-mono:mono;
            if (lp->samp_state==1){
                /* keep a preroll ring; arm -> capture on threshold. samp_pre_pos
                 * points at the oldest sample (next to overwrite). */
                lp->samp_pre[lp->samp_pre_pos] = mono;
                lp->samp_pre_pos = (lp->samp_pre_pos+1)%SAMP_PREROLL;
                if (a > SAMP_THRESH){ lp->samp_state=2; lp->samp_pos=0; lp->samp_quiet=0;
                    /* flush preroll oldest->newest */
                    for (int k=0;k<SAMP_PREROLL && lp->samp_pos<SLOT_FRAMES;k++){
                        int idx=(lp->samp_pre_pos+k)%SAMP_PREROLL;
                        sl->buf[lp->samp_pos++]=lp->samp_pre[idx];
                    }
                }
            }
            if (lp->samp_state==2){
                if (lp->samp_pos<SLOT_FRAMES) sl->buf[lp->samp_pos++]=mono;
                if (a<SAMP_STOP_LVL) lp->samp_quiet++; else lp->samp_quiet=0;
                if (lp->samp_quiet>MS2FRAMES(SAMP_STOP_MS) || lp->samp_pos>=SLOT_FRAMES){
                    sl->len = lp->samp_pos; lp->samp_state=0;
                }
            }
        }
    }

    if (lp->rec_clip < 0) return;
    clip_t *c = &lp->clips[lp->rec_clip];

    if (c->state == CS_ARMED) {
        /* waiting for the quantized bar start */
        if (lp->timeline + frames <= c->rec_start) return;
        /* start mid-block */
    }
    if (c->rec_layer < 0) {
        /* initial take: write wet into base */
        int start = 0;
        if (c->state == CS_ARMED) {
            if (c->rec_start > lp->timeline) start = (int)(c->rec_start - lp->timeline);
            c->state = CS_RECORDING;
        }
        for (int i=start;i<frames;i++){
            if (c->rec_write >= CLIP_FRAMES){ rec_end_tap(lp, lp->rec_clip); break; }
            c->base[2*c->rec_write]   = f2i16(lp->wet_l[i]);
            c->base[2*c->rec_write+1] = f2i16(lp->wet_r[i]);
            c->rec_write++;
        }
        /* deferred end (ENDQ) reached? */
        if (c->state == CS_ENDQ && c->rec_write >= c->endq_target){
            c->len = c->endq_target; c->bars = c->len/lp->bar_frames;
            c->anchor = c->rec_start; c->state=CS_PLAYING; c->playing=1; c->gain_cur=0.0f;
            clip_stop_other_in_row(lp, lp->rec_clip/CLIPS_PER_TRACK, lp->rec_clip%CLIPS_PER_TRACK);
            lp->rec_clip=-1;
        }
    } else {
        /* overdub: add wet into the layer at the clip's rolling position */
        layer_t *ly = &c->layers[c->rec_layer];
        for (int i=0;i<frames;i++){
            int64_t d = (lp->timeline + i) - c->anchor; int64_t pp = d % c->len; if(pp<0)pp+=c->len;
            int pos=(int)pp;
            ly->buf[2*pos]   = sat_add16(ly->buf[2*pos],   f2i16(lp->wet_l[i]));
            ly->buf[2*pos+1] = sat_add16(ly->buf[2*pos+1], f2i16(lp->wet_r[i]));
        }
        ly->written += frames;
    }
}

/* advance sequencer + chord lane, firing at boundaries inside this block */
static void run_clocks(looperino_t *lp, int frames) {
    if (!lp->grid || lp->bar_frames < 16) { lp->cur_step=-1; return; }
    int step_len = lp->bar_frames/16;
    int64_t rel0 = lp->timeline - lp->grid_origin;
    int64_t rel1 = rel0 + frames;

    /* bar boundary -> apply pending chord + advance chord lane */
    int64_t bar1 = (rel1-1)>=0? (rel1-1)/lp->bar_frames : -1;
    if (bar1 != lp->last_bar_abs && bar1>=0) {
        lp->last_bar_abs = bar1;
        /* chord lane entry for this bar (wrap 16) */
        uint8_t laneslot = lp->chordlane[(int)(bar1 % N_STEPS)];
        if (laneslot != 0xFF && laneslot < TONAL_NODES) lp->chord_node = laneslot;
        if (lp->chord_pending != TM_NONE){ lp->chord_node = lp->chord_pending; lp->chord_pending=TM_NONE; }
        tonal_pad_notes(lp->chord_node, 48, lp->note_pads);
    }

    if (!lp->seq_run) { lp->cur_step = (int)((rel0/step_len)%16); return; }

    /* step boundary */
    int64_t step_idx1 = rel1>0 ? (rel1-1)/step_len : -1;
    if (step_idx1 != lp->last_step_abs && step_idx1>=0) {
        /* fire the step at its offset within this block */
        int64_t boundary = step_idx1*step_len;   /* absolute frames since origin */
        int off = (int)(boundary - rel0); if(off<0)off=0; if(off>=frames)off=frames-1;
        lp->last_step_abs = step_idx1;
        int step = (int)(step_idx1 % 16);
        lp->cur_step = step;
        /* fx lane */
        if (lp->fxlane[step] != 0xFF && lp->fxlane[step] < PFX_COUNT) {
            punch_press(&lp->pfx, lp->fxlane[step], lp->fx_lane_target==8?8:lp->fx_lane_target,
                        lp->bar_frames);
            lp->fx_step_active = 1;
        } else if (lp->fx_step_active) {
            punch_release(&lp->pfx, lp->pfx.active); lp->fx_step_active=0;
        }
        kit_seq_step(lp, step, off);
    }
}

/* render playing clips (base + active layers) with a loop-seam crossfade */
static void render_clips(looperino_t *lp, int frames) {
    for (int t=0;t<N_LOOP_TRACKS;t++){
        for (int s=0;s<CLIPS_PER_TRACK;s++){
            clip_t *c = &lp->clips[t*CLIPS_PER_TRACK+s];
            if (c->len<=0) continue;
            /* declick gain toward playing target */
            float target = c->playing?1.0f:0.0f;
            if (c->gain_cur==target && target==0.0f) continue;
            float *out = lp->tbuf[t];
            int len = c->len;
            int seam = SEAM_XFADE; if (seam > len/2) seam = len/2;
            for (int i=0;i<frames;i++){
                int64_t d = (lp->timeline+i) - c->anchor; int64_t pp=d%len; if(pp<0)pp+=len;
                int pos=(int)pp;
                float l = c->base[2*pos]/32768.0f, r = c->base[2*pos+1]/32768.0f;
                for (int k=0;k<MAX_LAYERS;k++) if(c->layers[k].buf && c->layers[k].in_mix){
                    l += c->layers[k].buf[2*pos]/32768.0f; r += c->layers[k].buf[2*pos+1]/32768.0f;
                }
                /* seam crossfade: blend tail with head */
                if (pos >= len-seam && seam>0){
                    int hp = pos-(len-seam);           /* 0..seam */
                    float f = (float)hp/seam;
                    float hl=c->base[2*hp]/32768.0f, hr=c->base[2*hp+1]/32768.0f;
                    for (int k=0;k<MAX_LAYERS;k++) if(c->layers[k].buf && c->layers[k].in_mix){
                        hl += c->layers[k].buf[2*hp]/32768.0f; hr += c->layers[k].buf[2*hp+1]/32768.0f;
                    }
                    l = l*(1.0f-f)+hl*f; r = r*(1.0f-f)+hr*f;
                }
                c->gain_cur += STOP_FADE_COEF*(target-c->gain_cur);
                out[2*i]   += l*c->gain_cur;
                out[2*i+1] += r*c->gain_cur;
            }
        }
    }
}

/* ============================ entry ============================ */
static plugin_api_v2_t g_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = lp_create,
    .destroy_instance = lp_destroy,
    .on_midi = lp_on_midi,
    .set_param = lp_set_param,
    .get_param = lp_get_param,
    .get_error = lp_get_error,
    .render_block = lp_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
