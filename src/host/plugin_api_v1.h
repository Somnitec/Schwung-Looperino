/*
 * Schwung Plugin API v1 / v2  (vendored)
 *
 * Stable ABI for DSP modules loaded by the Schwung host runtime on Ableton Move.
 * Modules are .so files loaded via dlopen() and export move_plugin_init_v2().
 *
 * This is a verbatim copy of the header shipped with Schwung
 * (src/host/plugin_api_v1.h). It is vendored here so the module can be
 * cross-compiled standalone without a full Schwung checkout. Keep it in sync
 * with upstream when bumping api_version.
 */

#ifndef MOVE_PLUGIN_API_V1_H
#define MOVE_PLUGIN_API_V1_H

#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1

/* Audio constants */
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_AUDIO_OUT_OFFSET 256
#define MOVE_AUDIO_IN_OFFSET (2048 + 256)
#define MOVE_AUDIO_BYTES_PER_BLOCK 512

/* MIDI source identifiers */
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2
#define MOVE_MIDI_SOURCE_HOST 3
#define MOVE_MIDI_SOURCE_FX_BROADCAST 4

/* Clock status identifiers for host_api_v1.get_clock_status() */
#define MOVE_CLOCK_STATUS_UNAVAILABLE 0
#define MOVE_CLOCK_STATUS_STOPPED 1
#define MOVE_CLOCK_STATUS_RUNNING 2

typedef int (*move_mod_emit_value_fn)(void *ctx,
                                      const char *source_id,
                                      const char *target,
                                      const char *param,
                                      float signal,
                                      float depth,
                                      float offset,
                                      int bipolar,
                                      int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

/*
 * Host API - provided by host to plugin during initialization
 */
typedef struct host_api_v1 {
    uint32_t api_version;

    int sample_rate;
    int frames_per_block;

    /* Direct mailbox access. Audio I/O lives here:
     *   input  block = (int16_t*)(mapped_memory + audio_in_offset)
     *   layout = interleaved stereo int16 [L0,R0,L1,R1,...], frames_per_block frames
     */
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;

    void (*log)(const char *msg);

    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);

    int (*get_clock_status)(void);

    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;

    /* Tempo query — returns current BPM (120.0 default). NULL if unsupported. */
    float (*get_bpm)(void);

    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
} host_api_v1_t;

/*
 * Plugin API v2 - Instance-based API for multi-instance support
 */
#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;

    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);

    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);

#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"

/* v1 (legacy, single-instance) kept for completeness */
typedef struct plugin_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    int (*get_error)(char *buf, int buf_len);
    void (*render_block)(int16_t *out_interleaved_lr, int frames);
} plugin_api_v1_t;

typedef plugin_api_v1_t* (*move_plugin_init_v1_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_SYMBOL "move_plugin_init_v1"

#endif /* MOVE_PLUGIN_API_V1_H */
