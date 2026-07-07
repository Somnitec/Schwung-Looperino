/*
 * punchfx.c — momentary punch-in FX (OP-Z model). One live instance; a new
 * press steals it. v1 operates on the MASTER bus only (per-track targeting is
 * later) — this keeps it simple and lets stutter/tape-stop read from a single
 * continuously-fed history ring.
 *
 * The ring (p->ring, PUNCH_FREEZE_FRAMES stereo) is written every block by
 * punch_feed() with the pre-FX master, so a stutter/tape-stop press has real
 * recent audio to loop — no per-press bulk copy (that used to blow the block
 * budget and only ever captured 128 real samples). While a stutter/tape-stop
 * is held, feeding pauses so the slice stays frozen.
 *
 * v1 set: stutter 1/16, stutter 1/4, filter sweep down, tape-stop, delay
 * throw, duck/pump. An activation crossfade (p->xfade) prevents clicks.
 */

#include <string.h>
#include <math.h>
#include "looperino.h"

void punch_init(punch_t *p, int16_t *ring_mem, float *dly_mem) {
    memset(p, 0, sizeof(*p));
    p->ring = ring_mem;                 /* PUNCH_FREEZE_FRAMES stereo int16 */
    p->active = PFX_NONE;
    p->rate = 1.0;
    p->dly.buf = dly_mem;               /* stereo interleaved, DLY_MAX frames */
}

/* keep the history ring warm with the pre-FX master (called every block) */
void punch_feed(punch_t *p, const float *bus, int frames) {
    int is_slice = (p->active == PFX_STUT16 || p->active == PFX_STUT4 || p->active == PFX_TAPESTOP);
    if (is_slice && !p->released) return;   /* freeze the slice while held */
    for (int i = 0; i < frames; i++) {
        p->ring[2*p->ring_pos]   = f2i16(bus[2*i]);
        p->ring[2*p->ring_pos+1] = f2i16(bus[2*i+1]);
        if (++p->ring_pos >= PUNCH_FREEZE_FRAMES) p->ring_pos = 0;
    }
}

void punch_press(punch_t *p, int fx, int target, int bar_frames) {
    if (fx < 0 || fx >= PFX_COUNT) return;
    p->active = fx;
    p->target = target;             /* retained for future per-track targeting */
    p->xfade = 0.0f;
    p->released = 0;
    p->rate = 1.0;
    p->freeze_read = 0.0;
    p->filt_lp_l = p->filt_lp_r = 0.0f;
    int slice = 0;
    if (fx == PFX_STUT16) slice = bar_frames / 16;
    else if (fx == PFX_STUT4) slice = bar_frames / 4;
    else if (fx == PFX_TAPESTOP) slice = bar_frames / 4;
    if (slice > PUNCH_FREEZE_FRAMES - BLOCK) slice = PUNCH_FREEZE_FRAMES - BLOCK;
    if (slice < 1) slice = 1;
    p->loop_len = slice;
    p->slice_start = p->ring_pos;   /* window is [start-loop_len, start) */
    if (fx == PFX_DELAY) {
        p->dly.time = bar_frames / 4;               /* 1/4-note throw */
        if (p->dly.time > DLY_MAX-1) p->dly.time = DLY_MAX-1;
        if (p->dly.time < 1) p->dly.time = 1;
        p->dly.fb = 0.6f; p->dly.mix = 0.9f;
    }
}

void punch_release(punch_t *p, int fx) {
    if (p->active != fx) return;
    if (fx == PFX_DELAY) { p->released = 1; p->tail_blocks = 0; }  /* let the tail ring */
    else p->active = PFX_NONE;
}

/* read the frozen slice from the ring at the current phase */
static inline void ring_slice(punch_t *p, float *l, float *r) {
    int base = p->slice_start - p->loop_len;
    int idx = base + (int)p->freeze_read;
    idx %= PUNCH_FREEZE_FRAMES; if (idx < 0) idx += PUNCH_FREEZE_FRAMES;
    *l = p->ring[2*idx] / 32768.0f;
    *r = p->ring[2*idx+1] / 32768.0f;
}

/* returns 1 while FX (incl. delay tail) still contributes */
int punch_process(punch_t *p, float *bus, int frames, int bar_frames,
                  int64_t grid_pos) {
    if (p->active == PFX_NONE && !p->released) return 0;
    int fx = p->active;

    for (int i = 0; i < frames; i++) {
        float dry_l = bus[2*i], dry_r = bus[2*i+1];
        float wl = dry_l, wr = dry_r;

        switch (fx) {
        case PFX_STUT16:
        case PFX_STUT4:
            ring_slice(p, &wl, &wr);
            p->freeze_read += 1.0;
            if (p->freeze_read >= p->loop_len) p->freeze_read -= p->loop_len;
            break;
        case PFX_TAPESTOP:
            ring_slice(p, &wl, &wr);
            wl *= (float)p->rate; wr *= (float)p->rate;
            p->freeze_read += p->rate;
            if (p->freeze_read >= p->loop_len) p->freeze_read -= p->loop_len;
            p->rate -= 1.0 / (0.6 * SR);              /* ~600 ms to a halt */
            if (p->rate < 0.0) p->rate = 0.0;
            break;
        case PFX_FILTER: {
            /* lowpass sweep: cutoff coeff ramps down over ~1 bar */
            float t = bar_frames > 0 ? (float)((grid_pos + i) % bar_frames) / bar_frames : 0.0f;
            float cut = 1.0f - 0.92f * t;
            if (cut < 0.02f) cut = 0.02f;
            p->filt_lp_l += cut * (dry_l - p->filt_lp_l);
            p->filt_lp_r += cut * (dry_r - p->filt_lp_r);
            wl = p->filt_lp_l; wr = p->filt_lp_r;
            break;
        }
        case PFX_DELAY: {
            int rp = p->dly.pos - p->dly.time; if (rp < 0) rp += DLY_MAX;
            float dl = p->dly.buf[2*rp], dr = p->dly.buf[2*rp+1];
            float feed_l = p->released ? 0.0f : dry_l;
            float feed_r = p->released ? 0.0f : dry_r;
            float wdl = feed_l + dl*p->dly.fb, wdr = feed_r + dr*p->dly.fb;
            if (wdl < 1e-20f && wdl > -1e-20f) wdl = 0.0f;   /* denormal flush */
            if (wdr < 1e-20f && wdr > -1e-20f) wdr = 0.0f;
            p->dly.buf[2*p->dly.pos]   = wdl;
            p->dly.buf[2*p->dly.pos+1] = wdr;
            if (++p->dly.pos >= DLY_MAX) p->dly.pos = 0;
            if (p->released) { wl = dl*p->dly.mix; wr = dr*p->dly.mix; }  /* wet tail only */
            else { wl = dry_l + dl*p->dly.mix; wr = dry_r + dr*p->dly.mix; }
            break;
        }
        case PFX_DUCK: {
            int beat = bar_frames/4 > 0 ? bar_frames/4 : SR/2;
            float ph = (float)((grid_pos + i) % beat) / beat;
            float env = 0.15f + 0.85f * ph;            /* dip to 0.15, recover */
            wl = dry_l * env; wr = dry_r * env;
            break;
        }
        default: break;
        }

        if (p->xfade < 1.0f) { p->xfade += 1.0f/MS2FRAMES(8); if (p->xfade>1.0f) p->xfade=1.0f; }
        float x = p->xfade;
        bus[2*i]   = dry_l*(1.0f-x) + wl*x;
        bus[2*i+1] = dry_r*(1.0f-x) + wr*x;
    }

    /* delay tail termination (~ fb^n < 1e-3) */
    if (p->released && fx == PFX_DELAY) {
        p->tail_blocks++;
        if (p->tail_blocks > (int)(3.0f / (1.0f - p->dly.fb + 0.001f))) {
            p->active = PFX_NONE; p->released = 0; p->tail_blocks = 0;
            return 0;
        }
    }
    return p->active != PFX_NONE || p->released;
}
