/*
 * Tonal Map — navigable chord-movement graph, ported from Omniphone's
 * Harmonic Journey (variants/teensy40-12pad-screen/tonalmap.h).
 *
 * Chord-flower diagram and concept: @briancalli.music. Related "Tonal Map of
 * Chord Sequences": William R Thomas (2025), diagram by @purpasteur.
 *
 * 12 roots x 4 types (maj, min, dom7, aug) = 48 flower nodes, plus an 8-node
 * inner ring (ids 48-55: G7 C E7 A Db7 Gb Bb7 Eb) with its own moves. The
 * move table is hand-drawn, not a formula — port it verbatim, don't "fix" it.
 *
 * Looperino additions vs the Omniphone original:
 *   - tonal_pad_notes(): 16 ascending MIDI notes for the 4x4 note-pad grid
 *     (same chord-tones + one-passing-tone voicing walk, MIDI instead of Hz)
 *   - tonal_chord_mask(): pitch-class bitmask for chord-follow remapping
 *   - tonal_nearest_tone(): smallest-signed-move remap of a note into a chord
 */
#ifndef LOOPERINO_TONALMAP_H
#define LOOPERINO_TONALMAP_H

#include <stdint.h>

enum { TM_MAJ = 0, TM_MIN = 1, TM_DOM7 = 2, TM_AUG = 3 };

#define TONAL_FLOWER_NODES 48
#define TONAL_NODES        56
#define TONAL_MAX_MOVES    4
#define TM_NONE            0xFF

typedef struct {
    uint8_t node;   /* destination */
    uint8_t dotted; /* 1 = optional/tension edge, 0 = functional resolution */
} tonal_move_t;

extern const char *const TONAL_NAMES[TONAL_NODES];

uint8_t tonal_root(uint8_t node);                       /* pitch class 0-11 */
uint8_t tonal_type(uint8_t node);                       /* TM_* */
uint8_t tonal_moves(uint8_t node, tonal_move_t out[TONAL_MAX_MOVES]);
uint16_t tonal_chord_mask(uint8_t node);                /* bit n = pc n in chord */
/* 16 ascending MIDI notes for the note-pad grid; base_midi = pad 0 anchor
 * (chord root lands at or above it). */
void tonal_pad_notes(uint8_t node, int base_midi, uint8_t out[16]);
/* Remap note into the chord: nearest chord tone, smallest signed move,
 * ties resolved downward. */
uint8_t tonal_nearest_tone(uint8_t midi_note, uint16_t chord_mask);

#endif
