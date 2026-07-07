/*
 * chains.c — the input FX chain (Dub-FX model: performer hears voice through
 * this, and it's what gets printed into loops). One live instance, re-preset
 * when the routed track changes. Stages: gate -> comp -> 3-band EQ -> drive ->
 * delay -> reverb. All feel params come from CHAIN_PRESETS.
 *
 * Coeff computation (biquads, delay times) happens in chain_set_preset, which
 * runs in control context (set_param on the SPI thread — accepted as a param
 * change, not per-sample). chain_process is per-sample and allocation-free.
 *
 * Reverb is a Schroeder/Freeverb topology (8 combs + 4 allpass per channel).
 * Memory is carved from one caller-provided block (freed by the instance).
 */

#include <math.h>
#include <string.h>
#include "looperino.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Freeverb tuning (44.1 kHz reference), stereo spread on right ---- */
static const int COMB_TUNING[REV_COMBS] = {1116,1188,1277,1356,1422,1491,1557,1617};
static const int AP_TUNING[REV_APS]     = {556, 441, 341, 225};
#define REV_SPREAD 23
#define REV_MEM_FLOATS CHAIN_REV_MEM_FLOATS  /* carve budget; sum(comb+ap)*2+spread < this */

/* ================= biquad (RBJ cookbook) ================= */
static void bq_reset(biquad_t *b) { b->x1=b->x2=b->y1=b->y2=0; }

static void bq_peaking(biquad_t *b, float fs, float f0, float dbgain, float q) {
    float A = powf(10.0f, dbgain/40.0f);
    float w0 = 2.0f*(float)M_PI*f0/fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw/(2.0f*q);
    float a0 = 1 + alpha/A;
    b->b0 = (1 + alpha*A)/a0;
    b->b1 = (-2*cw)/a0;
    b->b2 = (1 - alpha*A)/a0;
    b->a1 = (-2*cw)/a0;
    b->a2 = (1 - alpha/A)/a0;
}
static void bq_lowshelf(biquad_t *b, float fs, float f0, float dbgain, float s) {
    float A = powf(10.0f, dbgain/40.0f);
    float w0 = 2.0f*(float)M_PI*f0/fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw/2.0f*sqrtf((A+1/A)*(1/s-1)+2);
    float twoSqrtAal = 2*sqrtf(A)*alpha;
    float a0 = (A+1)+(A-1)*cw+twoSqrtAal;
    b->b0 = A*((A+1)-(A-1)*cw+twoSqrtAal)/a0;
    b->b1 = 2*A*((A-1)-(A+1)*cw)/a0;
    b->b2 = A*((A+1)-(A-1)*cw-twoSqrtAal)/a0;
    b->a1 = -2*((A-1)+(A+1)*cw)/a0;
    b->a2 = ((A+1)+(A-1)*cw-twoSqrtAal)/a0;
}
static void bq_highshelf(biquad_t *b, float fs, float f0, float dbgain, float s) {
    float A = powf(10.0f, dbgain/40.0f);
    float w0 = 2.0f*(float)M_PI*f0/fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw/2.0f*sqrtf((A+1/A)*(1/s-1)+2);
    float twoSqrtAal = 2*sqrtf(A)*alpha;
    float a0 = (A+1)-(A-1)*cw+twoSqrtAal;
    b->b0 = A*((A+1)+(A-1)*cw+twoSqrtAal)/a0;
    b->b1 = -2*A*((A-1)+(A+1)*cw)/a0;
    b->b2 = A*((A+1)+(A-1)*cw-twoSqrtAal)/a0;
    b->a1 = 2*((A-1)-(A+1)*cw)/a0;
    b->a2 = ((A+1)-(A-1)*cw-twoSqrtAal)/a0;
}
static inline float bq_run(biquad_t *b, float x) {
    float y = b->b0*x + b->b1*b->x1 + b->b2*b->x2 - b->a1*b->y1 - b->a2*b->y2;
    b->x2=b->x1; b->x1=x; b->y2=b->y1; b->y1=y;
    if (y < 1e-20f && y > -1e-20f) y = 0.0f;   /* denormal flush */
    return y;
}

/* ================= presets ================= */
/* name, gate{thr,att,rel}, comp{thr,ratio,makeup}, eq{lowdb,lowhz, middb,midhz,midq, highdb,highhz}, drive, dly{beats,fb,mix}, revwet */
const chain_preset_t CHAIN_PRESETS[N_LOOP_TRACKS] = {
    /* t1 kick+snare: tight gate, punch comp, low boost, no verb */
    { "kick+snare", 0.010f, 0.002f, 0.08f,  0.30f, 4.0f, 1.6f,
      4.0f, 90.0f,  -2.0f, 500.0f, 0.9f,  1.0f, 6000.0f,
      0.0f,  0.0f, 0.0f, 0.0f,  0.0f },
    /* t2 percussion: gate, comp, hp-ish (low cut via neg low shelf), slap */
    { "percussion", 0.008f, 0.002f, 0.10f,  0.35f, 3.0f, 1.4f,
      -3.0f, 200.0f,  1.0f, 3000.0f, 1.0f,  2.0f, 9000.0f,
      0.0f,  0.5f, 0.25f, 0.12f,  0.06f },
    /* t3 bass: LPF-ish (neg high shelf), drive, comp, no verb */
    { "bass", 0.006f, 0.003f, 0.12f,  0.25f, 5.0f, 1.8f,
      3.0f, 80.0f,  0.0f, 700.0f, 0.7f,  -8.0f, 3500.0f,
      0.35f, 0.0f, 0.0f, 0.0f,  0.0f },
    /* t4 vocal/spacy: gate, gentle comp, presence EQ, long reverb + dotted delay */
    { "vocal", 0.005f, 0.005f, 0.20f,  0.28f, 3.0f, 1.5f,
      -1.0f, 150.0f,  3.0f, 3500.0f, 0.9f,  2.0f, 10000.0f,
      0.0f,  0.75f, 0.30f, 0.28f,  0.32f },
};

/* ================= lifecycle ================= */
void chain_init(input_chain_t *c, float *rev_mem, float *dly_mem) {
    memset(c, 0, sizeof(*c));
    /* carve reverb memory */
    float *p = rev_mem;
    for (int i = 0; i < REV_COMBS; i++) {
        c->rev.comb_len[i] = COMB_TUNING[i];
        c->rev.comb[0][i] = p; p += COMB_TUNING[i];
        c->rev.comb[1][i] = p; p += COMB_TUNING[i] + REV_SPREAD;
    }
    for (int i = 0; i < REV_APS; i++) {
        c->rev.ap_len[i] = AP_TUNING[i];
        c->rev.ap[0][i] = p; p += AP_TUNING[i];
        c->rev.ap[1][i] = p; p += AP_TUNING[i] + REV_SPREAD;
    }
    /* p - rev_mem must stay < REV_MEM_FLOATS; asserted by allocation size */
    c->rev.feedback = 0.84f;
    c->rev.damp = 0.35f;
    c->rev.wet = 0.0f;
    c->dly.buf = dly_mem;
    c->dly.pos = 0;
    c->preset_idx = -1;
    c->bypass = 1;                    /* M1: record dry until FX is tuned on device */
    c->dcb_x1 = c->dcb_y1 = 0.0f;
    chain_set_preset(c, 3, 100.0f);   /* default: vocal */
}

/* clear delay/reverb tails and filter state (used on preset switch) */
static void chain_clear_tails(input_chain_t *c) {
    memset(c->dly.buf, 0, sizeof(float) * 2 * DLY_MAX);
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < REV_COMBS; i++) {
            memset(c->rev.comb[ch][i], 0, sizeof(float)*(c->rev.comb_len[i]+ (ch?REV_SPREAD:0)));
            c->rev.comb_lp[ch][i] = 0.0f;
            c->rev.comb_pos[ch][i] = 0;
        }
        for (int i = 0; i < REV_APS; i++) {
            memset(c->rev.ap[ch][i], 0, sizeof(float)*(c->rev.ap_len[i]+ (ch?REV_SPREAD:0)));
            c->rev.ap_pos[ch][i] = 0;
        }
    }
    bq_reset(&c->eq_low); bq_reset(&c->eq_mid); bq_reset(&c->eq_high);
    c->gate.env = 0; c->gate.gain = 0; c->comp.env = 0;
}

void chain_set_preset(input_chain_t *c, int flavor, float bpm) {
    if (flavor < 0) flavor = 0;
    if (flavor >= N_LOOP_TRACKS) flavor = N_LOOP_TRACKS-1;
    const chain_preset_t *pr = &CHAIN_PRESETS[flavor];
    int changed = (c->preset_idx != flavor);
    c->preset = pr;
    c->preset_idx = flavor;

    bq_lowshelf(&c->eq_low, (float)SR, pr->eq_low_hz, pr->eq_low_db, 0.7f);
    bq_peaking(&c->eq_mid, (float)SR, pr->eq_mid_hz, pr->eq_mid_db, pr->eq_mid_q);
    bq_highshelf(&c->eq_high, (float)SR, pr->eq_high_hz, pr->eq_high_db, 0.7f);

    if (pr->dly_beats > 0.0f) {
        float secs = pr->dly_beats * 60.0f / (bpm > 20.0f ? bpm : 100.0f);
        int t = (int)(secs * SR);
        if (t < 1) t = 1;
        if (t > DLY_MAX-1) t = DLY_MAX-1;
        c->dly.time = t;
        c->dly.fb = pr->dly_fb;
        c->dly.mix = pr->dly_mix;
    } else {
        c->dly.time = 0; c->dly.mix = 0; c->dly.fb = 0;
    }
    c->rev.wet = pr->rev_wet;

    if (changed) chain_clear_tails(c);
}

/* ================= per-sample stages ================= */
static inline float gate_run(gate_t *g, float x, const chain_preset_t *pr) {
    float a = fabsf(x);
    /* envelope follower: FAST attack, SLOW release (a gate must open quickly and
     * close gently, else it chops speech into clicks). Fixed coeffs; the preset
     * only sets the open threshold. */
    float coef = a > g->env ? 0.30f : 0.0015f;
    g->env += coef * (a - g->env);
    float target = g->env > pr->gate_thresh ? 1.0f : 0.0f;
    g->gain += 0.02f * (target - g->gain);   /* ~1 ms gain glide, click-free */
    return x * g->gain;
}

static inline float comp_run(comp_t *c, float x, const chain_preset_t *pr) {
    float a = fabsf(x);
    c->env += (a > c->env ? 0.02f : 0.002f) * (a - c->env);
    float gain = 1.0f;
    if (c->env > pr->comp_thresh) {
        /* feed-forward, hard knee, ratio */
        float over = c->env / pr->comp_thresh;              /* >1 */
        float compressed = powf(over, 1.0f/pr->comp_ratio); /* <over */
        gain = compressed / over;
    }
    return x * gain * pr->comp_makeup;
}

/* one comb: lowpass in the feedback path (damping) */
static inline float comb_run(reverb_t *r, int ch, int i, float in) {
    float *buf = r->comb[ch][i];
    int len = r->comb_len[i] + (ch?REV_SPREAD:0);
    int pos = r->comb_pos[ch][i];
    float y = buf[pos];
    r->comb_lp[ch][i] = y*(1.0f-r->damp) + r->comb_lp[ch][i]*r->damp;
    buf[pos] = in + r->comb_lp[ch][i]*r->feedback;
    if (++pos >= len) pos = 0;
    r->comb_pos[ch][i] = pos;
    return y;
}
static inline float ap_run(reverb_t *r, int ch, int i, float in) {
    float *buf = r->ap[ch][i];
    int len = r->ap_len[i] + (ch?REV_SPREAD:0);
    int pos = r->ap_pos[ch][i];
    float bufout = buf[pos];
    float y = -in + bufout;
    buf[pos] = in + bufout*0.5f;
    if (++pos >= len) pos = 0;
    r->ap_pos[ch][i] = pos;
    return y;
}
static inline void reverb_run(reverb_t *r, float inl, float inr, float *outl, float *outr) {
    float in = (inl + inr) * 0.015f;   /* freeverb fixed input gain */
    float acc[2] = {0,0};
    for (int ch = 0; ch < 2; ch++) {
        float s = 0;
        for (int i = 0; i < REV_COMBS; i++) s += comb_run(r, ch, i, in);
        for (int i = 0; i < REV_APS; i++) s = ap_run(r, ch, i, s);
        acc[ch] = s;
    }
    *outl = acc[0]; *outr = acc[1];
}

/* Mono through gate/comp/EQ/drive (Move's mic is mono; line-in vocals usually
 * are too — stereo line-in collapses to mono in v1, an accepted limitation),
 * stereo width emerges only from the reverb/delay. Halves filter cost and
 * drops the per-channel-state bookkeeping. */
void chain_process(input_chain_t *c, const int16_t *in, float *out_l,
                   float *out_r, int frames, float bpm) {
    (void)bpm;
    /* Bypass: record/monitor essentially what came in — sum to mono, block DC
     * (~14 Hz), unity gain. This is the M1 default so loops sound like the
     * source; the flavored FX chain below is opt-in (set_param chain_fx=1). */
    if (c->bypass) {
        for (int i = 0; i < frames; i++) {
            float x = (in[2*i] + in[2*i+1]) * (0.5f/32768.0f);
            float y = x - c->dcb_x1 + 0.998f * c->dcb_y1;
            c->dcb_x1 = x; c->dcb_y1 = y;
            if (y < 1e-20f && y > -1e-20f) y = 0.0f;
            out_l[i] = y; out_r[i] = y;
        }
        return;
    }
    const chain_preset_t *pr = c->preset;
    delay_t *d = &c->dly;
    for (int i = 0; i < frames; i++) {
        float m = (in[2*i] + in[2*i+1]) * (0.5f/32768.0f);
        m = gate_run(&c->gate, m, pr);
        m = comp_run(&c->comp, m, pr);
        m = bq_run(&c->eq_low, m); m = bq_run(&c->eq_mid, m); m = bq_run(&c->eq_high, m);
        if (pr->drive_amt > 0.0f)
            m = soft_clip(m * (1.0f + pr->drive_amt*6.0f));

        float l = m, r = m;
        if (d->time > 0) {                 /* mono delay, same tap both sides */
            int rp = d->pos - d->time; if (rp < 0) rp += DLY_MAX;
            float dl = d->buf[rp];
            d->buf[d->pos] = m + dl*d->fb;
            if (++d->pos >= DLY_MAX) d->pos = 0;
            l += dl*d->mix; r += dl*d->mix;
        }
        if (c->rev.wet > 0.0f) {           /* reverb decorrelates L/R */
            float rl, rr; reverb_run(&c->rev, m, m, &rl, &rr);
            l += rl*c->rev.wet; r += rr*c->rev.wet;
        }
        out_l[i] = l; out_r[i] = r;
    }
}
