/*
 * Looperino DSP — shared types & tuning constants.
 *
 * Everything feel-related is a constant here so it can be tweaked per
 * docs/v1-spec.md. Realtime rules apply to render_block/on_midi/set_param
 * (all run on the SPI thread for overtake DSPs): no allocation, no file I/O
 * except the accepted-hiccup save/load paths, no locks.
 */
#ifndef LOOPERINO_H
#define LOOPERINO_H

#include <stdint.h>
#include <stddef.h>

#include "schwung_plugin_api.h"

/* ---------- dimensions ---------- */
#define SR              44100
#define BLOCK           128
#define N_LOOP_TRACKS   4
#define CLIPS_PER_TRACK 4
#define N_CLIPS         (N_LOOP_TRACKS * CLIPS_PER_TRACK)
#define N_INSTR_TRACKS  4
#define N_TRACKS        (N_LOOP_TRACKS + N_INSTR_TRACKS)
#define SEL_MASTER      8

/* Memory driver. 16 clips + 6-layer pool, each CLIP_FRAMES stereo int16:
 * (16+6) * 16*44100 * 4 B ~= 62 MB, memset-prefaulted at load. 16 s covers
 * 4 bars down to 60 BPM (~6.6 bars at 100). Raise if longer loops are wanted
 * and the device has headroom. */
#define MAX_CLIP_SEC    16
#define CLIP_FRAMES     (MAX_CLIP_SEC * SR)
#define LAYER_POOL      6                       /* shared overdub-layer buffers */
#define MAX_LAYERS      4                       /* per-clip stack depth */

#define KIT_SLOTS       16
#define SLOT_SEC        2                        /* per drum-slot sample cap */
#define SLOT_FRAMES     (SLOT_SEC * SR)          /* mono */

#define N_STEPS         16
#define N_DRUM_VOICES   12
#define N_SYNTH_VOICES  8

/* ---------- feel constants (tweak on device) ---------- */
#define TAP_MS          250      /* multi-tap window */
#define HOLD_MS         330      /* press becomes hold/record/overdub */
#define QUANT_GRACE_MS  120      /* press just after a bar snaps back to it */
#define STOP_FADE_COEF  0.0022f  /* one-pole clip gain, ~10 ms (magneto) */
#define LOOP_EDGE_FADE  160      /* ~3.6 ms in/out fade per loop so the seam
                                  * always passes through zero (click-free) */
#define FIRSTLOOP_TARGET_BPM 100.0f
#define FIRSTLOOP_MIN_BPM    65.0f
#define FIRSTLOOP_MAX_BPM    180.0f
#define SAMP_THRESH     1000     /* int16 |x| that arms drum-slot capture */
#define SAMP_STOP_MS    300      /* silence that ends a capture */
#define SAMP_STOP_LVL   250
#define SAMP_PREROLL    512      /* frames kept before the threshold hit */
#define COPY_FRAMES_PER_BLOCK 65536   /* amortized clip-copy budget (~1.4 s for 16 s) */
#define METRO_GAIN      0.25f

/* ---------- pad roles (padmap protocol, see v1-spec.md) ---------- */
#define ROLE_NONE  0
#define ROLE_CLIP  1   /* value = track*4 + slot */
#define ROLE_DRUM  2   /* value = kit slot 0-15 of selected drum track */
#define ROLE_NOTE  3   /* value = note-grid index 0-15 */
#define ROLE_FX    4   /* value = punch fx index */

/* ---------- clip states (status protocol) ---------- */
enum clip_state {
    CS_EMPTY = 0, CS_STOPPED, CS_PLAYING, CS_ARMED, CS_RECORDING,
    CS_OVERDUB, CS_ENDQ,     /* recording, end queued at next bar */
    CS_COPYING,              /* destination materializing */
};

/* ---------- punch FX ids ---------- */
enum punch_fx {
    PFX_NONE = -1, PFX_STUT16 = 0, PFX_STUT4, PFX_FILTER, PFX_TAPESTOP,
    PFX_DELAY, PFX_DUCK, PFX_COUNT
};

/* ---------- instrument track types ---------- */
enum instr_type { IT_DRUM = 0, IT_SYNTH = 1 };

/* ---------- DSP building blocks (chains.c) ---------- */
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } biquad_t;
typedef struct { float env, gain; } gate_t;
typedef struct { float env; } comp_t;

#define REV_COMBS 8
#define REV_APS   4
typedef struct {
    float *comb[2][REV_COMBS];      /* per channel */
    int    comb_len[REV_COMBS], comb_pos[2][REV_COMBS];
    float  comb_lp[2][REV_COMBS];
    float *ap[2][REV_APS];
    int    ap_len[REV_APS], ap_pos[2][REV_APS];
    float  feedback, damp, wet;
} reverb_t;

#define DLY_MAX (2 * SR)
typedef struct {
    float *buf;                      /* DLY_MAX floats (mono) or 2*DLY_MAX */
    int    pos, time;                /* time in frames */
    float  fb, mix, send;            /* send used by punch delay-throw */
} delay_t;

/* memory the instance allocates and hands to chain_init (floats). */
#define CHAIN_REV_MEM_FLOATS 32768
#define CHAIN_DLY_MEM_FLOATS (2 * DLY_MAX)
#define PUNCH_DLY_MEM_FLOATS (2 * DLY_MAX)

/* per-flavor input-chain preset (all stages optional) */
typedef struct {
    const char *name;
    float gate_thresh, gate_att, gate_rel;      /* linear env coeffs */
    float comp_thresh, comp_ratio, comp_makeup;
    float eq_low_db, eq_low_hz, eq_mid_db, eq_mid_hz, eq_mid_q,
          eq_high_db, eq_high_hz;
    float drive_amt;                             /* 0 = off */
    float dly_beats, dly_fb, dly_mix;            /* beats=0 -> off */
    float rev_wet;                               /* 0 = off */
} chain_preset_t;

typedef struct {
    gate_t   gate;
    comp_t   comp;
    biquad_t eq_low, eq_mid, eq_high;
    delay_t  dly;
    reverb_t rev;
    const chain_preset_t *preset;
    int      preset_idx;
    int      bypass;                 /* 1 = record/monitor dry (M1 default) */
    float    dcb_x1, dcb_y1;         /* DC blocker state (bypass path) */
} input_chain_t;

/* ---------- punch FX state (punchfx.c) ---------- */
#define PUNCH_FREEZE_FRAMES SR           /* history ring = 1 s; max slice too */
typedef struct {
    int16_t *ring;                   /* continuous master history, stereo int16 */
    int      ring_pos;               /* ring write head */
    int      loop_len;               /* stutter/tape slice length (frames) */
    int      slice_start;            /* ring index at press (window end) */
    double   freeze_read;            /* slice playback phase */
    int      active;                 /* PFX_NONE or id */
    int      target;                 /* retained for future per-track targeting */
    double   rate;                   /* tape-stop multiplier */
    float    filt_lp_l, filt_lp_r;   /* filter-sweep one-pole state */
    float    xfade;                  /* activation ramp 0..1 */
    delay_t  dly;                    /* delay-throw line (always warm) */
    int      released;               /* released, tail ringing (delay only) */
    int      tail_blocks;            /* delay-tail termination counter */
} punch_t;

/* ---------- clips ---------- */
typedef struct {
    int16_t *buf;        /* pool buffer, stereo interleaved */
    int      start_pos;  /* clip position where writing began */
    int      written;    /* frames written (<= clip len) */
    int      in_mix;     /* audible (undo toggles this) */
    int      merge_cursor; /* frames merged into base; -1 = not merging */
} layer_t;

typedef struct {
    int16_t *base;                    /* CLIP_FRAMES stereo int16 */
    int      len;                     /* frames; 0 = empty */
    int64_t  anchor;                  /* timeline frame of clip pos 0 */
    int      bars;
    layer_t  layers[MAX_LAYERS];
    int      n_layers;
    int      redo_valid;              /* top layer popped but recoverable */
    int      state;                   /* clip_state */
    float    gain_cur;                /* declick one-pole toward playing? 1:0 */
    int      playing;
    /* recording */
    int      rec_write;               /* frames recorded so far (initial take) */
    int64_t  rec_start;               /* timeline frame recording started */
    int      endq_target;             /* frames target when end queued */
    int      fresh;                   /* just recorded, no overdubs yet */
    int      rec_layer;               /* overdub layer index, -1 = initial take */
    /* copy materialization */
    int      copy_src;                /* clip index or -1 */
    int      copy_cursor;
} clip_t;

/* ---------- drum kit / synth ---------- */
typedef struct {
    int16_t *buf;                     /* SLOT_FRAMES mono int16 */
    int      len;
    float    gain;                    /* normalize compensation */
    float    pitch;                   /* semitones */
} slot_t;

typedef struct {
    int    active, slot, track;
    double pos, step;
    float  gain, env, env_target;     /* 5 ms declick envelope */
} drum_voice_t;

typedef struct {
    int    active, note, track;
    int64_t on_frame;
    float  phase1, phase2, freq;
    float  env; int env_stage;        /* 0=off 1=att 2=dec/sus 3=rel */
    float  svf_lp, svf_bp;
    float  vel;
    int    gate_frames;               /* >0 seq one-shot countdown; <0 = held */
} synth_voice_t;

typedef struct {
    uint8_t on[N_STEPS];              /* bit set = trigger */
    uint8_t prob[N_STEPS];            /* 0-100 */
} steplane_t;

typedef struct {
    int        type;                  /* instr_type */
    slot_t     slots[KIT_SLOTS];
    steplane_t drums[KIT_SLOTS];      /* per-slot lanes (drum kits) */
    steplane_t synth;                 /* synth on/off lane */
    uint8_t    synth_note[N_STEPS];   /* MIDI note per step, 0xFF = default */
    int        follow;                /* 0 ignore, 1 root, 2 chord */
    int        octave;
    int        focus_slot;            /* last-pressed drum slot (edit focus) */
} instr_track_t;

/* ---------- the instance ---------- */
typedef struct looperino {
    /* timeline */
    int64_t timeline;                 /* frames since load, never stops */
    int64_t grid_origin;              /* timeline frame of bar 0 */
    int     grid;                     /* grid established */
    int     bar_frames;
    float   bpm;
    int     tempo_fixed;              /* fixed-tempo mode */
    float   fixed_bpm;
    int     seq_run;                  /* Play state */
    int     metro;

    /* external clock (display only) */
    int64_t last_tick_frame; float tick_interval, ext_bpm; int ticks_seen;

    /* pads */
    uint8_t padmap[32];               /* role bytes, index = note-68 */
    struct {
        int64_t down_frame, up_frame, last_tap_frame;
        int down, taps, consumed, was_hold;
        int clip;                     /* clip index the press targets */
    } pad[32];
    int last_touched_clip;

    /* clips & layers */
    clip_t  clips[N_CLIPS];
    int16_t *pool[LAYER_POOL];
    int      pool_used[LAYER_POOL];

    /* input */
    input_chain_t chain;
    float   wet_l[BLOCK], wet_r[BLOCK];
    int     monitor, allow_monitor;
    float   monitor_gain;
    int     peak_in, peak_out;
    int     input_route;              /* looper track the input runs through */

    /* recording target */
    int     rec_clip;                 /* clip currently recording, -1 */
    int16_t inring[2 * 8192];         /* input history for grace snapback */
    int     inring_pos;

    /* instruments */
    instr_track_t instr[N_INSTR_TRACKS];
    drum_voice_t  dvoice[N_DRUM_VOICES];
    synth_voice_t svoice[N_SYNTH_VOICES];
    int     cur_step;                 /* sequencer step 0-15, -1 before grid */
    int64_t last_step_abs;            /* absolute step index last fired */
    int64_t last_bar_abs;             /* absolute bar index last processed */

    /* drum-slot sampling */
    int     samp_state;               /* 0 idle, 1 armed, 2 capturing */
    int     samp_track, samp_slot;
    int     samp_pos, samp_quiet;
    int16_t samp_pre[SAMP_PREROLL * 2];
    int     samp_pre_pos;

    /* punch fx */
    punch_t pfx;
    uint8_t fxlane[N_STEPS];          /* 0xFF = none */
    int     fx_lane_target;
    int     fx_step_active;           /* lane-triggered fx running */

    /* chords */
    uint8_t chord_node, chord_pending, home_node;
    uint8_t chordlane[N_STEPS];       /* per-bar, 0xFF = hold */
    int     chord_bar_idx;
    uint8_t note_pads[16];            /* current NOTE-pad MIDI notes */

    /* mixer */
    float   gain[N_TRACKS], gain_cur[N_TRACKS];
    int     mute[N_TRACKS];
    float   master_gain;

    /* misc */
    uint32_t rng;
    int     sel;                      /* selected track / SEL_MASTER */
    float   metro_phase; int metro_left;
    char    module_dir[256];

    /* scratch (float work buffers) */
    float   tbuf[N_TRACKS][2 * BLOCK];
    float   master[2 * BLOCK];
} looperino_t;

/* chains.c */
void chain_init(input_chain_t *c, float *rev_mem, float *dly_mem);
void chain_set_preset(input_chain_t *c, int flavor, float bpm);
void chain_process(input_chain_t *c, const int16_t *in, float *out_l,
                   float *out_r, int frames, float bpm);
extern const chain_preset_t CHAIN_PRESETS[N_LOOP_TRACKS];

/* punchfx.c */
void punch_init(punch_t *p, int16_t *ring_mem, float *dly_mem);
void punch_press(punch_t *p, int fx, int target, int bar_frames);
void punch_release(punch_t *p, int fx);
void punch_feed(punch_t *p, const float *bus, int frames);  /* keep ring warm */
/* process the master bus in place; grid_pos = timeline-relative frame for
 * beat-synced FX (duck). Returns 1 while the FX (incl. tail) is still live. */
int  punch_process(punch_t *p, float *bus, int frames, int bar_frames,
                   int64_t grid_pos);

/* kit.c */
void kit_trigger_drum(looperino_t *lp, int track, int slot, float vel,
                      int frame_offset);
void kit_synth_on(looperino_t *lp, int track, int note, float vel, int gate_frames);
void kit_synth_off(looperino_t *lp, int track, int note);
void kit_render(looperino_t *lp, int frames);
void kit_seq_step(looperino_t *lp, int step, int frame_offset);
void kit_all_notes_off(looperino_t *lp);

/* wavio.c — control-context only (accepted SPI hiccup) */
int wav_write_stereo16(const char *path, const int16_t *buf, int frames);
int wav_read_into(const char *path, int16_t *dst, int max_frames,
                  int dst_channels);

/* looperino.c helpers shared with kit.c */
float lp_randf(looperino_t *lp);

static inline float f2f_clamp(float x) {
    return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
}
static inline int16_t f2i16(float x) {
    x = f2f_clamp(x) * 32767.0f;
    return (int16_t)(x < 0 ? x - 0.5f : x + 0.5f);
}
static inline float soft_clip(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    if (x > 1.0f) { float t = x - 1.0f; return 1.0f - t * t * 0.5f + t; } /* knee */
    if (x < -1.0f) { float t = -x - 1.0f; return -(1.0f - t * t * 0.5f + t); }
    return x - x * x * x * (1.0f / 3.0f);
}
static inline int16_t sat_add16(int16_t a, int16_t b) {
    int32_t s = (int32_t)a + (int32_t)b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}
#define MS2FRAMES(ms) ((int)((ms) * (SR / 1000.0f)))

#endif
