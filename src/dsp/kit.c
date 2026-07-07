/*
 * kit.c — instrument tracks: drum sampler voices, a small subtractive synth,
 * and the 16-step sequencer trigger. Drum tracks play 16 sampled slots; synth
 * tracks play chord-aware notes. Follow modes (root/chord) are applied at
 * sequencer trigger time via the tonal map. Live pad notes are already
 * chord-mapped by the UI/core before they reach kit_synth_on.
 *
 * Voices render into lp->tbuf[4+track] (float stereo interleaved). All voice
 * pools are fixed-size; oldest-voice stealing, no allocation.
 */

#include <math.h>
#include <string.h>
#include "looperino.h"
#include "tonalmap.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float midi_hz(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* ---------------- drum voices ---------------- */
static drum_voice_t *alloc_drum(looperino_t *lp) {
    for (int i = 0; i < N_DRUM_VOICES; i++)
        if (!lp->dvoice[i].active) return &lp->dvoice[i];
    /* steal oldest (lowest env or furthest along) — pick i=0 fallback */
    drum_voice_t *best = &lp->dvoice[0];
    for (int i = 1; i < N_DRUM_VOICES; i++)
        if (lp->dvoice[i].pos > best->pos) best = &lp->dvoice[i];
    return best;
}

void kit_trigger_drum(looperino_t *lp, int track, int slot, float vel, int frame_offset) {
    if (track < 0 || track >= N_INSTR_TRACKS) return;
    slot_t *s = &lp->instr[track].slots[slot];
    if (s->len <= 0) return;
    drum_voice_t *v = alloc_drum(lp);
    v->active = 1; v->slot = slot; v->track = track;
    v->pos = -(double)frame_offset;      /* negative: waits until offset within block */
    v->step = powf(2.0f, s->pitch / 12.0f);
    v->gain = s->gain * vel;
    v->env = 0.0f; v->env_target = 1.0f;
}

/* ---------------- synth voices ---------------- */
static synth_voice_t *alloc_synth(looperino_t *lp) {
    for (int i = 0; i < N_SYNTH_VOICES; i++)
        if (!lp->svoice[i].active) return &lp->svoice[i];
    synth_voice_t *best = &lp->svoice[0];
    for (int i = 1; i < N_SYNTH_VOICES; i++)
        if (lp->svoice[i].env_stage == 3) return &lp->svoice[i];  /* prefer releasing */
    return best;
}

void kit_synth_on(looperino_t *lp, int track, int note, float vel, int gate_frames) {
    if (track < 0 || track >= N_INSTR_TRACKS) return;
    synth_voice_t *v = alloc_synth(lp);
    v->active = 1; v->note = note; v->track = track;
    v->on_frame = lp->timeline;
    v->phase1 = 0.0f; v->phase2 = 0.3f;
    v->freq = midi_hz(note);
    v->env = 0.0f; v->env_stage = 1;     /* attack */
    v->svf_lp = v->svf_bp = 0.0f;
    v->vel = vel;
    v->gate_frames = gate_frames;        /* <0 held (live), >0 seq one-shot */
}

void kit_synth_off(looperino_t *lp, int track, int note) {
    for (int i = 0; i < N_SYNTH_VOICES; i++) {
        synth_voice_t *v = &lp->svoice[i];
        if (v->active && v->track == track && v->note == note && v->env_stage < 3)
            v->env_stage = 3;            /* release */
    }
}

void kit_all_notes_off(looperino_t *lp) {
    for (int i = 0; i < N_DRUM_VOICES; i++) lp->dvoice[i].active = 0;
    for (int i = 0; i < N_SYNTH_VOICES; i++) lp->svoice[i].active = 0;
}

/* ---------------- sequencer step ---------------- */
void kit_seq_step(looperino_t *lp, int step, int frame_offset) {
    step &= 15;
    uint16_t chord_mask = tonal_chord_mask(lp->chord_node);
    uint8_t chord_root = tonal_root(lp->chord_node);
    for (int t = 0; t < N_INSTR_TRACKS; t++) {
        instr_track_t *it = &lp->instr[t];
        if (it->type == IT_DRUM) {
            for (int slot = 0; slot < KIT_SLOTS; slot++) {
                steplane_t *ln = &it->drums[slot];
                if (!(ln->on[step])) continue;
                if (ln->prob[step] < 100 && lp_randf(lp)*100.0f >= ln->prob[step]) continue;
                kit_trigger_drum(lp, t, slot, 1.0f, frame_offset);
            }
        } else { /* synth */
            if (!it->synth.on[step]) continue;
            if (it->synth.prob[step] < 100 && lp_randf(lp)*100.0f >= it->synth.prob[step]) continue;
            int base = it->synth_note[step];
            if (base == 0xFF) base = 60 + chord_root;      /* default: chord root near C4 */
            int note = base + it->octave*12;
            if (it->follow == 2) note = tonal_nearest_tone(note, chord_mask);
            /* follow==1 (root-follow) transposes by chord-root delta from C */
            else if (it->follow == 1) note += chord_root;
            if (note < 0) note = 0;
            if (note > 127) note = 127;
            int gate = lp->bar_frames > 0 ? (lp->bar_frames/16)*9/10 : MS2FRAMES(120);
            kit_synth_on(lp, t, note, 1.0f, gate);   /* seq note: one-shot gate */
        }
    }
}

/* ---------------- render ---------------- */
void kit_render(looperino_t *lp, int frames) {
    /* drums */
    for (int i = 0; i < N_DRUM_VOICES; i++) {
        drum_voice_t *v = &lp->dvoice[i];
        if (!v->active) continue;
        slot_t *s = &lp->instr[v->track].slots[v->slot];
        float *out = lp->tbuf[N_LOOP_TRACKS + v->track];
        const int16_t *buf = s->buf;
        int len = s->len;
        for (int n = 0; n < frames; n++) {
            if (v->pos < 0) { v->pos += 1.0; continue; }   /* pre-offset wait */
            int i0 = (int)v->pos;
            if (i0 >= len - 1) { v->active = 0; break; }
            float fr = (float)(v->pos - i0);
            float sm = (buf[i0]*(1.0f-fr) + buf[i0+1]*fr) / 32768.0f;
            if (v->env < v->env_target) { v->env += 1.0f/MS2FRAMES(5); if (v->env>1) v->env=1; }
            float o = sm * v->gain * v->env;
            out[2*n] += o; out[2*n+1] += o;
            v->pos += v->step;
        }
    }
    /* synth */
    for (int i = 0; i < N_SYNTH_VOICES; i++) {
        synth_voice_t *v = &lp->svoice[i];
        if (!v->active) continue;
        float *out = lp->tbuf[N_LOOP_TRACKS + v->track];
        float inc1 = v->freq / SR, inc2 = v->freq*1.007f / SR;
        for (int n = 0; n < frames; n++) {
            /* seq one-shot gate: start release when the countdown expires */
            if (v->gate_frames > 0 && v->env_stage < 3) {
                if (--v->gate_frames == 0) v->env_stage = 3;
            }
            /* env */
            switch (v->env_stage) {
            case 1: v->env += 1.0f/MS2FRAMES(5); if (v->env>=1){v->env=1;v->env_stage=2;} break;
            case 2: v->env += (0.7f - v->env)*0.0005f; break;         /* decay to sustain */
            case 3: v->env -= 1.0f/MS2FRAMES(160); if (v->env<=0){v->env=0;v->active=0;} break;
            }
            if (!v->active) break;
            /* two detuned saws */
            v->phase1 += inc1; if (v->phase1>=1) v->phase1-=1;
            v->phase2 += inc2; if (v->phase2>=1) v->phase2-=1;
            float saw1 = 2.0f*v->phase1 - 1.0f;
            float saw2 = 2.0f*v->phase2 - 1.0f;
            float in = (saw1+saw2)*0.4f;
            /* Chamberlin SVF lowpass, cutoff rides the envelope */
            float fc = 900.0f + v->env*2600.0f;
            float f = 2.0f*sinf((float)M_PI*fc/SR);
            float high = in - v->svf_lp - 0.8f*v->svf_bp;
            v->svf_bp += f*high;
            v->svf_lp += f*v->svf_bp;
            if (v->svf_lp<1e-20f && v->svf_lp>-1e-20f) v->svf_lp=0;
            float o = v->svf_lp * v->env * v->vel * 0.5f;
            out[2*n] += o; out[2*n+1] += o;
        }
    }
}
