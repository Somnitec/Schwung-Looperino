/* See tonalmap.h for provenance and credits. The move table below is a
 * verbatim port of Omniphone's hand-drawn TONAL_MOVE_TABLE (2026-07-03
 * transcription) — treat as data, not code. */

#include "tonalmap.h"

typedef struct { uint8_t root; uint8_t type; } cycle8_chord_t;

/* Inner ring order matches TONAL_NAMES[48..55]: G7 C E7 A Db7 Gb Bb7 Eb. */
static const cycle8_chord_t CYCLE8_CHORD[8] = {
    { 7, TM_DOM7 }, { 0, TM_MAJ }, { 4, TM_DOM7 }, { 9, TM_MAJ },
    { 1, TM_DOM7 }, { 6, TM_MAJ }, { 10, TM_DOM7 }, { 3, TM_MAJ },
};

const char *const TONAL_NAMES[TONAL_NODES] = {
    "C",  "Cm",  "C7",  "C+",
    "Db", "Dbm", "Db7", "Db+",
    "D",  "Dm",  "D7",  "D+",
    "Eb", "Ebm", "Eb7", "Eb+",
    "E",  "Em",  "E7",  "E+",
    "F",  "Fm",  "F7",  "F+",
    "Gb", "Gbm", "Gb7", "Gb+",
    "G",  "Gm",  "G7",  "G+",
    "Ab", "Abm", "Ab7", "Ab+",
    "A",  "Am",  "A7",  "A+",
    "Bb", "Bbm", "Bb7", "Bb+",
    "B",  "Bm",  "B7",  "B+",
    "G7", "C", "E7", "A", "Db7", "Gb", "Bb7", "Eb",
};

typedef struct { uint8_t node; uint8_t dotted; } move_def_t;

static const move_def_t TONAL_MOVE_TABLE[TONAL_NODES][TONAL_MAX_MOVES] = {
    /* C    */ { { 37, 0 }, {  9, 0 }, { 18, 0 }, { 17, 0 } },
    /* Cm   */ { { 12, 0 }, { 22, 0 }, { 29, 0 }, { 55, 0 } },
    /* C7   */ { {  3, 0 }, { 22, 0 }, { 20, 0 }, { TM_NONE, 0 } },
    /* C+   */ { { 43, 0 }, { 11, 0 }, { 20, 0 }, { TM_NONE, 0 } },
    /* Db   */ { { 41, 0 }, {  7, 1 }, {  6, 1 }, { TM_NONE, 0 } },
    /* Dbm  */ { { 36, 0 }, { 33, 0 }, { 16, 0 }, { 26, 0 } },
    /* Db7  */ { { 24, 0 }, { 26, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* Db+  */ { { 47, 0 }, { 15, 0 }, { 24, 0 }, { TM_NONE, 0 } },
    /* D    */ { { 45, 0 }, { 11, 1 }, { 10, 1 }, { TM_NONE, 0 } },
    /* Dm   */ { { 37, 0 }, {  0, 0 }, { 20, 0 }, { 30, 0 } },
    /* D7   */ { { 11, 0 }, { 30, 0 }, { 28, 0 }, { TM_NONE, 0 } },
    /* D+   */ { {  3, 0 }, { 19, 0 }, { 28, 0 }, { TM_NONE, 0 } },
    /* Eb   */ { {  1, 0 }, { 21, 0 }, { 30, 0 }, { 29, 0 } },
    /* Ebm  */ { { 34, 0 }, { 41, 0 }, { 24, 0 }, { 53, 0 } },
    /* Eb7  */ { { 34, 0 }, { 15, 0 }, { 32, 0 }, { TM_NONE, 0 } },
    /* Eb+  */ { { 32, 0 }, {  7, 0 }, { 23, 0 }, { TM_NONE, 0 } },
    /* E    */ { {  5, 0 }, { 19, 1 }, { 18, 1 }, { TM_NONE, 0 } },
    /* Em   */ { { 38, 0 }, { 45, 0 }, {  0, 0 }, { 28, 0 } },
    /* E7   */ { { 36, 0 }, { 38, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* E+   */ { { 36, 0 }, { 11, 0 }, { 27, 0 }, { TM_NONE, 0 } },
    /* F    */ { {  9, 0 }, { 23, 1 }, { 22, 1 }, { TM_NONE, 0 } },
    /* Fm   */ { { 32, 0 }, { 42, 0 }, {  1, 0 }, { 12, 0 } },
    /* F7   */ { { 42, 0 }, { 23, 0 }, { 40, 0 }, { TM_NONE, 0 } },
    /* F+   */ { { 40, 0 }, { 15, 0 }, { 31, 0 }, { TM_NONE, 0 } },
    /* Gb   */ { { 33, 0 }, { 42, 0 }, { 41, 0 }, { 13, 0 } },
    /* Gbm  */ { { 36, 0 }, { 46, 0 }, {  5, 0 }, { 51, 0 } },
    /* Gb7  */ { { 46, 0 }, { 27, 0 }, { 44, 0 }, { TM_NONE, 0 } },
    /* Gb+  */ { { 35, 0 }, { 44, 0 }, { 19, 0 }, { TM_NONE, 0 } },
    /* G    */ { { 17, 0 }, { 31, 1 }, { 30, 1 }, { TM_NONE, 0 } },
    /* Gm   */ { { 40, 0 }, {  2, 0 }, {  9, 0 }, { 12, 0 } },
    /* G7   */ { {  0, 0 }, {  2, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* G+   */ { { 39, 0 }, {  0, 0 }, { 23, 0 }, { TM_NONE, 0 } },
    /* Ab   */ { { 35, 1 }, { 34, 1 }, { 21, 0 }, { TM_NONE, 0 } },
    /* Abm  */ { { 44, 0 }, {  6, 0 }, { 13, 0 }, { 24, 0 } },
    /* Ab7  */ { { 35, 0 }, {  6, 0 }, {  4, 0 }, { TM_NONE, 0 } },
    /* Ab+  */ { { 43, 0 }, {  4, 0 }, { 27, 0 }, { TM_NONE, 0 } },
    /* A    */ { { 45, 0 }, {  6, 0 }, {  5, 0 }, { 25, 0 } },
    /* Am   */ { {  0, 0 }, { 10, 0 }, { 17, 0 }, { 49, 0 } },
    /* A7   */ { { 39, 0 }, { 10, 0 }, {  8, 0 }, { TM_NONE, 0 } },
    /* A+   */ { { 47, 0 }, {  8, 0 }, { 31, 0 }, { TM_NONE, 0 } },
    /* Bb   */ { { 43, 1 }, { 42, 1 }, { 29, 0 }, { TM_NONE, 0 } },
    /* Bbm  */ { {  4, 0 }, { 14, 0 }, { 21, 0 }, { 24, 0 } },
    /* Bb7  */ { { 12, 0 }, { 14, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* Bb+  */ { { 35, 0 }, {  3, 0 }, { 12, 0 }, { TM_NONE, 0 } },
    /* B    */ { { 33, 0 }, { 47, 1 }, { 46, 1 }, { TM_NONE, 0 } },
    /* Bm   */ { { 36, 0 }, {  8, 0 }, { 18, 0 }, { 25, 0 } },
    /* B7   */ { { 47, 0 }, { 18, 0 }, { 16, 0 }, { TM_NONE, 0 } },
    /* B+   */ { { 39, 0 }, {  7, 0 }, { 16, 0 }, { TM_NONE, 0 } },
    /* inner G7  */ { { 49, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner C   */ { { 37, 0 }, { 50, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner E7  */ { { 51, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner A   */ { { 25, 0 }, { 52, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner Db7 */ { { 53, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner Gb  */ { { 13, 0 }, { 54, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner Bb7 */ { { 55, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
    /* inner Eb  */ { {  1, 0 }, { 48, 0 }, { TM_NONE, 0 }, { TM_NONE, 0 } },
};

uint8_t tonal_root(uint8_t n) {
    return (n < TONAL_FLOWER_NODES) ? (uint8_t)(n >> 2)
                                    : CYCLE8_CHORD[n - TONAL_FLOWER_NODES].root;
}

uint8_t tonal_type(uint8_t n) {
    return (n < TONAL_FLOWER_NODES) ? (uint8_t)(n & 3)
                                    : CYCLE8_CHORD[n - TONAL_FLOWER_NODES].type;
}

uint8_t tonal_moves(uint8_t node, tonal_move_t out[TONAL_MAX_MOVES]) {
    if (node >= TONAL_NODES) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < TONAL_MAX_MOVES; i++) {
        const move_def_t *m = &TONAL_MOVE_TABLE[node][i];
        if (m->node == TM_NONE) break;
        out[n].node = m->node;
        out[n].dotted = m->dotted;
        n++;
    }
    return n;
}

static const uint8_t IV_MAJ[3]  = { 0, 4, 7 };
static const uint8_t IV_MIN[3]  = { 0, 3, 7 };
static const uint8_t IV_DOM7[4] = { 0, 4, 7, 10 };
static const uint8_t IV_AUG[3]  = { 0, 4, 8 };

static const uint8_t *chord_intervals(uint8_t node, uint8_t *nt) {
    switch (tonal_type(node)) {
    case TM_MIN:  *nt = 3; return IV_MIN;
    case TM_DOM7: *nt = 4; return IV_DOM7;
    case TM_AUG:  *nt = 3; return IV_AUG;
    default:      *nt = 3; return IV_MAJ;
    }
}

uint16_t tonal_chord_mask(uint8_t node) {
    uint8_t nt;
    const uint8_t *iv = chord_intervals(node, &nt);
    uint8_t root = tonal_root(node);
    uint16_t mask = 0;
    for (uint8_t i = 0; i < nt; i++)
        mask |= (uint16_t)(1u << ((root + iv[i]) % 12));
    return mask;
}

/* Same walk as Omniphone's tonalMapFreqs: chord tones plus ONE passing tone
 * dropped into the widest gap, cycled upward with octave bumps — but 16 core
 * pitches (one per pad) instead of 6 octave-paired ones, and MIDI not Hz. */
void tonal_pad_notes(uint8_t node, int base_midi, uint8_t out[16]) {
    uint8_t nt;
    const uint8_t *iv = chord_intervals(node, &nt);

    uint8_t widest = 0, gap = 0;
    for (uint8_t i = 0; i < nt; i++) {
        uint8_t next = (uint8_t)((i + 1 < nt) ? iv[i + 1] : 12);
        uint8_t g = (uint8_t)(next - iv[i]);
        if (g > gap) { gap = g; widest = i; }
    }

    uint8_t seq[5], ns = 0;
    for (uint8_t i = 0; i < nt; i++) {
        seq[ns++] = iv[i];
        if (i == widest && gap >= 2) seq[ns++] = (uint8_t)(iv[i] + gap / 2);
    }

    uint8_t oct_up = 0, idx = 0;
    for (uint8_t k = 0; k < 16; k++) {
        if (idx == ns) { idx = 0; oct_up++; }
        int note = base_midi + tonal_root(node) + seq[idx] + 12 * oct_up;
        if (note > 127) note = 127;
        out[k] = (uint8_t)note;
        idx++;
    }
}

uint8_t tonal_nearest_tone(uint8_t midi_note, uint16_t chord_mask) {
    if (!chord_mask) return midi_note;
    for (int d = 0; d <= 6; d++) {
        /* tie resolves downward: try -d before +d */
        int down = (int)midi_note - d;
        if (down >= 0 && (chord_mask & (1u << (down % 12)))) return (uint8_t)down;
        int up = (int)midi_note + d;
        if (up <= 127 && (chord_mask & (1u << (up % 12)))) return (uint8_t)up;
    }
    return midi_note;
}
