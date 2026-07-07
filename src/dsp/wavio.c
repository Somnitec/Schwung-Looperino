/*
 * wavio.c — canonical 44-byte-header PCM16 WAV read/write. Control context
 * only (set_param on the SPI thread): fopen/fwrite here is the accepted
 * "load hiccup" (REALTIME_SAFETY.md) — NEVER call from render_block. Loop
 * save/load runs while not performing (documented rough edge).
 *
 * Reader handles PCM 8/16/24/32 + float32, mono/stereo, linear-resamples to
 * 44.1 kHz, folds/duplicates to the requested channel count. Enough to reload
 * our own writes and import external one-shots for drum slots.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "looperino.h"

static void wr_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wr_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static uint32_t rd_u32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_u16(const uint8_t *p){ return p[0]|(p[1]<<8); }

int wav_write_stereo16(const char *path, const int16_t *buf, int frames) {
    if (frames < 0) frames = 0;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data_bytes = (uint32_t)frames * 2 * 2;   /* stereo, 16-bit */
    uint8_t h[44];
    memcpy(h, "RIFF", 4);       wr_u32(h+4, 36 + data_bytes);
    memcpy(h+8, "WAVE", 4);     memcpy(h+12, "fmt ", 4);
    wr_u32(h+16, 16);           wr_u16(h+20, 1);       /* PCM */
    wr_u16(h+22, 2);            wr_u32(h+24, SR);
    wr_u32(h+28, SR*2*2);       wr_u16(h+32, 4);       /* block align */
    wr_u16(h+34, 16);           memcpy(h+36, "data", 4);
    wr_u32(h+40, data_bytes);
    size_t w = fwrite(h, 1, 44, f);
    if (frames > 0) w += fwrite(buf, 1, data_bytes, f);
    fclose(f);
    return (w == 44 + data_bytes) ? frames : -1;
}

/* read one source frame's L/R as float from raw sample bytes */
static void decode_sample(const uint8_t *d, int fmt, int bits, int ch,
                          float *l, float *r) {
    int bytes = bits/8;
    float v[2] = {0,0};
    for (int c = 0; c < ch && c < 2; c++) {
        const uint8_t *s = d + c*bytes;
        float x = 0;
        if (fmt == 3 && bits == 32) {                 /* float32 */
            uint32_t u = rd_u32(s); float fv; memcpy(&fv,&u,4); x = fv;
        } else if (bits == 16) {
            x = (int16_t)rd_u16(s) / 32768.0f;
        } else if (bits == 8) {
            x = ((int)s[0] - 128) / 128.0f;
        } else if (bits == 24) {
            int32_t u = (s[0]<<8)|(s[1]<<16)|((int32_t)s[2]<<24); x = u / 2147483648.0f;
        } else if (bits == 32) {
            int32_t u = (int32_t)rd_u32(s); x = u / 2147483648.0f;
        }
        v[c] = x;
    }
    if (ch == 1) { *l = *r = v[0]; } else { *l = v[0]; *r = v[1]; }
}

int wav_read_into(const char *path, int16_t *dst, int max_frames, int dst_channels) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[12];
    if (fread(hdr,1,12,f)!=12 || memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"WAVE",4)) { fclose(f); return -1; }
    int fmt=1, ch=2, bits=16; uint32_t srate=SR, data_off=0, data_len=0;
    uint8_t ck[8];
    while (fread(ck,1,8,f)==8) {
        uint32_t csz = rd_u32(ck+4);
        if (!memcmp(ck,"fmt ",4)) {
            uint8_t fb[40]; uint32_t n = csz>40?40:csz;
            if (fread(fb,1,n,f)!=n) break;
            fmt=rd_u16(fb); ch=rd_u16(fb+2); srate=rd_u32(fb+4); bits=rd_u16(fb+14);
            if (csz>n) fseek(f, csz-n, SEEK_CUR);
        } else if (!memcmp(ck,"data",4)) {
            data_off = ftell(f); data_len = csz;
            fseek(f, csz + (csz&1), SEEK_CUR);
        } else {
            fseek(f, csz + (csz&1), SEEK_CUR);
        }
    }
    if (!data_off || ch<1 || bits<8) { fclose(f); return -1; }
    if (ch > 2) ch = 2;
    int fbytes = (bits/8) * ch;
    if (fbytes < 1) { fclose(f); return -1; }
    uint32_t src_frames = data_len / fbytes;

    /* resample ratio */
    double ratio = (double)srate / SR;
    int out_frames = (int)(src_frames / ratio);
    if (out_frames > max_frames) out_frames = max_frames;

    /* stream: read source frame-by-frame with linear interp. To keep it
     * simple and control-context, buffer the whole source region. */
    uint8_t *raw = (uint8_t*)malloc((size_t)src_frames * fbytes);
    if (!raw) { fclose(f); return -1; }
    fseek(f, data_off, SEEK_SET);
    size_t got = fread(raw, 1, (size_t)src_frames*fbytes, f);
    fclose(f);
    src_frames = (uint32_t)(got / fbytes);

    for (int i = 0; i < out_frames; i++) {
        double sp = i * ratio;
        uint32_t i0 = (uint32_t)sp; if (i0 >= src_frames) i0 = src_frames?src_frames-1:0;
        uint32_t i1 = i0+1<src_frames ? i0+1 : i0;
        float fr = (float)(sp - i0);
        float l0,r0,l1,r1;
        decode_sample(raw + (size_t)i0*fbytes, fmt, bits, ch, &l0,&r0);
        decode_sample(raw + (size_t)i1*fbytes, fmt, bits, ch, &l1,&r1);
        float l = l0+(l1-l0)*fr, r = r0+(r1-r0)*fr;
        if (dst_channels == 1) {
            dst[i] = f2i16((l+r)*0.5f);
        } else {
            dst[2*i] = f2i16(l); dst[2*i+1] = f2i16(r);
        }
    }
    free(raw);
    return out_frames;
}
