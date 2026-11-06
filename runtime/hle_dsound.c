/* DirectSound: the XDK DSOUND API replaced by host functions over a software mixer (audio.c plays its output).
 *
 * Xbox DirectSound is a flat C API (the C++ methods are inline wrappers), so replacing the flat functions covers
 * every caller; only streams are also called through their XMediaObject vtable, whose entries are host thunks.
 * Every buffer and stream is a voice: PCM or Xbox ADPCM read straight from guest memory, resampled to 48 kHz with
 * its volume, pitch, play and loop regions, and the volumes of the mixin buffers it feeds (SetOutputBuffer).
 * Positions and stream packet completion come from what the mixer has actually played, so music decoding runs at
 * playback pace (completed at once, the WMA decoder spun flat out and starved the race loader of the guest lock).
 * The mixer runs on the audio thread under g_lock; the API takes the same lock and runs packet callbacks in
 * DirectSoundDoWork, on the game's thread.
 * 3D voices are attenuated by distance (rolloff curve or min/distance) and cone, panned left/right by position, and
 * pitched by Doppler; I3DL2 sets the direct path's level and, through a one-pole low-pass, its high-frequency loss.
 * Mix bins (speaker outputs) fold down to stereo volumes.
 * shortcut: the listener is fixed at the origin (right for head-relative sources, all of Forza's), and there is no
 * reverb (no listener room is ever set), SetFilter (hardware coefficients of unknown format), envelope, LFO,
 * Calculate3D voice data or DSP effect; add them when a sound needs them.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "xbox.h"

#define ARG(i) M32(c->esp + 4 + 4 * (i))
static void ret(X86Ctx *c, uint32_t value, int n) {
    c->eax = value;
    c->esp += 4 + 4 * n;
}

#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4
#define DSBPLAY_LOOPING 1
#define WAVE_FORMAT_XBOX_ADPCM 0x0069

/* guest object: 0 vtable (streams), 4 magic, 8 kind, 0xC refs, 0x10 data/callback, 0x14 size/context,
 * 0x18 voice index + 1, 0x1C max packets */
enum { O_VTABLE = 0, O_MAGIC = 4, O_KIND = 8, O_REFS = 0xC, O_DATA = 0x10, O_SIZE = 0x14, O_VOICE = 0x18,
       O_MAXPKT = 0x1C, O_BYTES = 0x20 };
enum { K_DEVICE = 1, K_BUFFER, K_STREAM };
#define MAGIC 0x4C485344 /* "DSHL" */

static uint32_t now_ms(void) { return (uint32_t)GetTickCount64(); }

/* DSLOG=1: formats of new buffers and streams, and every 10 s how often each setter ran; 2: every Play too */
static int dslog(void) {
    static int on = -1;
    if (on < 0) on = getenv("DSLOG") ? atoi(getenv("DSLOG")) : 0;
    return on;
}
static struct {
    const char *name;
    unsigned calls;
} g_calls[64];
static void count(const char *name) {
    int i;
    for (i = 0; i < 64 && g_calls[i].name && g_calls[i].name != name; i++) {}
    if (i < 64) g_calls[i].name = name, g_calls[i].calls++;
}

/* ---------------- voices ---------------- */

#define MAX_VOICES 4096 /* a race creates about 1,100 buffers */
typedef struct {
    uint32_t obj; /* guest object; 0: free */
    int kind;
    uint16_t tag, channels, align, bits; /* WAVEFORMATEX */
    uint32_t rate, freq;                 /* the format's rate; the playback rate SetFrequency/SetPitch chose (0: rate) */
    int playing, looping, paused;
    double pos; /* in frames: from the play region's start, or into the stream's oldest unplayed packet */
    uint32_t region_start, region_len, loop_start, loop_len; /* bytes, as the API gives them; length 0: to the end;
                                                                the loop counts from the play region's start */
    float volume;
    uint32_t out;        /* the mixin buffer this voice feeds (SetOutputBuffer); 0: the speakers */
    int is3d, mode3d;    /* has a 3D position; DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2 */
    float pos3d[3], min_dist, max_dist;
    int ncurve;          /* SetRolloffCurve: attenuation from min to max distance, replacing min/distance */
    float curve[32];
    float vel3d[3], doppler;                            /* SetVelocity (m/s), SetDopplerFactor */
    float cone_dir[3], cone_in, cone_out, cone_outside; /* SetConeOrientation, SetConeAngles (degrees), outside gain */
    float direct, direct_hf; /* SetI3DL2Source: direct-path gain, and its extra loss at 5 kHz in 1/100 dB */
    float room;              /* and its room (reverb) send, -1: none set */
    float lp[2], px[2];      /* that loss's shelf state, output and input (on the source voice, for its chain) */
    int nbins;           /* mix bins (speaker outputs) and their volumes; none: front left and right at full */
    uint32_t bin[8];
    float bin_volume[8];
    uint32_t cache_addr[2]; /* ADPCM: the last two blocks decoded (resampling reads across a block's edge) */
    int16_t cache[2][65 * 6];
    int cache_next;
    float energy;        /* sum of squares mixed since the last voice report */
} Voice;
static Voice g_voices[MAX_VOICES];
static CRITICAL_SECTION g_lock;

static Voice *voice(uint32_t o) {
    uint32_t i = o ? M32(o + O_VOICE) : 0;
    return i && i <= MAX_VOICES && g_voices[i - 1].obj == o ? &g_voices[i - 1] : NULL;
}

#define ADPCM_FRAMES 65 /* per block */

static uint32_t to_frames(const Voice *v, uint32_t bytes) {
    if (!v->align) return 0;
    return v->tag == WAVE_FORMAT_XBOX_ADPCM ? bytes / v->align * ADPCM_FRAMES : bytes / v->align;
}
static uint32_t to_bytes(const Voice *v, uint32_t frames) {
    return v->tag == WAVE_FORMAT_XBOX_ADPCM ? frames / ADPCM_FRAMES * v->align : frames * v->align;
}

/* Xbox ADPCM: IMA ADPCM in 36-byte blocks per channel (a 4-byte header with the first sample and the step index,
 * then 64 more samples as nibbles, low first), channels interleaved by 4-byte words. The header sample is played:
 * decoded that way the game's wave banks run on smoothly across blocks; skipped, every block boundary jumped (a buzz
 * at the block rate) */
static const int16_t g_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t g_ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static void adpcm_block(const uint8_t *in, int ch, int16_t *out) {
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(in[4 * c] | in[4 * c + 1] << 8), idx = in[4 * c + 2] > 88 ? 88 : in[4 * c + 2];
        out[c] = (int16_t)pred;
        for (int i = 0; i < 64; i++) {
            const uint8_t *word = in + 4 * ch + ((i / 8) * ch + c) * 4;
            int nib = (word[(i % 8) / 2] >> ((i & 1) * 4)) & 15, step = g_ima_step[idx], diff = step >> 3;
            if (nib & 4) diff += step;
            if (nib & 2) diff += step >> 1;
            if (nib & 1) diff += step >> 2;
            pred += nib & 8 ? -diff : diff;
            pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
            idx += g_ima_index[nib];
            idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
            out[(i + 1) * ch + c] = (int16_t)pred;
        }
    }
}

/* frame n of the sound at guest address `data`, as left and right */
static void fetch(Voice *v, uint32_t data, uint32_t n, float *l, float *r) {
    int ch = v->channels < 1 ? 1 : v->channels > 6 ? 6 : v->channels;
    float s[6];
    if (v->tag == WAVE_FORMAT_XBOX_ADPCM) {
        uint32_t block = data + n / ADPCM_FRAMES * v->align;
        int k = v->cache_addr[0] == block ? 0 : v->cache_addr[1] == block ? 1 : -1;
        if (k < 0) k = v->cache_next, v->cache_next ^= 1, adpcm_block(g_mem + block, ch, v->cache[k]), v->cache_addr[k] = block;
        for (int c = 0; c < ch; c++) s[c] = v->cache[k][(n % ADPCM_FRAMES) * ch + c] / 32768.0f;
    } else if (v->bits == 8) {
        const uint8_t *p = g_mem + data + n * v->align;
        for (int c = 0; c < ch; c++) s[c] = (p[c] - 128) / 128.0f;
    } else {
        const int16_t *p = (const int16_t *)(g_mem + data + n * v->align);
        for (int c = 0; c < ch; c++) s[c] = p[c] / 32768.0f;
    }
    if (ch == 1) *l = *r = s[0];
    else if (ch < 6) *l = s[0], *r = s[1];
    else *l = s[0] + 0.7f * (s[2] + s[4]), *r = s[1] + 0.7f * (s[2] + s[5]); /* FL FR C LFE SL SR */
}

/* 3D: attenuation by distance (the rolloff curve, else min/distance) and constant-power panning by x, with the
 * listener at the origin facing +z (DirectSound is left-handed: +x right) */
static float dist3d(const Voice *v, float d) {
    float g;
    float lo = v->min_dist, hi = v->max_dist > lo ? v->max_dist : lo;
    if (v->ncurve) { /* the points sit evenly up to the maximum distance, after an implied 1 at the minimum */
        float t = d <= lo ? 0 : d >= hi ? 1 : (d - lo) / (hi - lo), x = t * v->ncurve;
        int i = (int)x;
        if (i >= v->ncurve) g = v->curve[v->ncurve - 1];
        else g = (i ? v->curve[i - 1] : 1) + (v->curve[i] - (i ? v->curve[i - 1] : 1)) * (x - i);
    } else {
        g = lo / (d < lo ? lo : d > hi ? hi : d);
    }
    if ((v->cone_in < 360 || v->cone_outside != 1) && d > 1e-3f) { /* cone: by the angle off its axis to the listener */
        float dl = sqrtf(v->cone_dir[0] * v->cone_dir[0] + v->cone_dir[1] * v->cone_dir[1] + v->cone_dir[2] * v->cone_dir[2]);
        float cosang = dl > 0 ? -(v->cone_dir[0] * v->pos3d[0] + v->cone_dir[1] * v->pos3d[1] + v->cone_dir[2] * v->pos3d[2]) / (dl * d) : 1;
        float ang = acosf(cosang < -1 ? -1 : cosang > 1 ? 1 : cosang) * 57.29578f, in = v->cone_in / 2, out = v->cone_out / 2;
        float t = ang <= in ? 0 : ang >= out || out <= in ? 1 : (ang - in) / (out - in);
        g *= powf(v->cone_outside, t); /* linear in dB between the inside and outside angles */
    }
    return g;
}

static void gain3d(const Voice *v, float *l, float *r) {
    *l = *r = 1;
    if (!v->is3d || v->mode3d == 2) return;
    float d = sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]), g = dist3d(v, d);
    float pan = d > 1e-3f ? v->pos3d[0] / d : 0, a = (pan + 1) * 0.78539816f; /* 0 .. pi/2 */
    *l = g * cosf(a) * 1.41421356f;
    *r = g * sinf(a) * 1.41421356f;
}

/* Doppler (head-relative, so the velocity is the source's relative to the listener): the pitch falls with the speed
 * away from the listener, f' = f c / (c + v_r * factor) */
static double doppler(const Voice *v) {
    double ratio = 1;
    for (int depth = 0; v && depth < 5; depth++, v = v->out ? voice(v->out) : NULL) {
        float d = sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]);
        if (!v->is3d || v->mode3d == 2 || !v->doppler || d < 1e-3f) continue;
        float vr = (v->vel3d[0] * v->pos3d[0] + v->vel3d[1] * v->pos3d[1] + v->vel3d[2] * v->pos3d[2]) / d * v->doppler;
        vr = vr < -170 ? -170 : vr > 170 ? 170 : vr; /* kept well short of the speed of sound */
        ratio *= 343.3 / (343.3 + vr);
    }
    return ratio;
}

/* I3DL2's high-frequency loss is a level for the highs (reference 5 kHz), so it is a first-order high shelf:
 * y = lp + g (x - lp), lp the bilinear one-pole low-pass at 2.5 kHz (zero at Nyquist): exactly 1 at DC, g at the
 * top, -4 dB at 5 kHz for g = -6 dB. (A plain low-pass matched at 5 kHz kept falling above it and dulled a race's
 * highs five-fold.) Returns g; 1: off */
#define SHELF_B 0.141705f /* K / (1 + K), K = tan(pi 2500 / 48000) */
#define SHELF_C 0.716590f /* (1 - K) / (1 + K) */
static float hf_gain(float mb) { return mb > -1 ? 1 : powf(10, mb / 2000); }

/* the direct path's high-frequency loss down the voice's chain, in 1/100 dB */
static float hf_loss(const Voice *v) {
    float mb = 0;
    for (int depth = 0; v && depth < 5; depth++, v = v->out ? voice(v->out) : NULL) mb += v->direct_hf;
    return mb;
}

/* Mix bins folded to stereo: each side takes its loudest bin (front, back and the 3D "XTLK" front/back bins), the
 * centre counts 0.707 and LFE 0.5 on both; I3DL2 and effect sends (10 and up) are dropped, there are no effects.
 * Taking the loudest bin, not the sum, keeps a submix that feeds several bins of one side at unity. */
static void gain_bins(const Voice *v, float *l, float *r) {
    static const float side[10][2] = {{1, 0}, {0, 1}, {0.707f, 0.707f}, {0.5f, 0.5f}, {1, 0}, {0, 1},
                                      {1, 0}, {0, 1}, {1, 0}, {0, 1}};
    if (!v->nbins) {
        *l = *r = 1;
        return;
    }
    *l = *r = 0;
    for (int i = 0; i < v->nbins; i++)
        if (v->bin[i] < 10) {
            float gl = side[v->bin[i]][0] * v->bin_volume[i], gr = side[v->bin[i]][1] * v->bin_volume[i];
            *l = gl > *l ? gl : *l;
            *r = gr > *r ? gr : *r;
        }
}

/* left and right gain: the voice's volume, mix bins and 3D placement, times those of the mixin buffers it feeds */
static void gain(const Voice *v, float *l, float *r) {
    float gl, gr, bl, br;
    *l = *r = 1;
    for (int depth = 0; v && depth < 5; depth++, v = v->out ? voice(v->out) : NULL) {
        gain3d(v, &gl, &gr);
        gain_bins(v, &bl, &br);
        *l *= v->volume * v->direct * gl * bl;
        *r *= v->volume * v->direct * gr * br;
    }
}

/* DSMIXBINS: dwMixBinCount, lpMixBinVolumePairs -> {dwMixBin, lVolume (1/100 dB)}. replace: SetMixBins (the list
 * itself), else SetMixBinVolumes (only the volumes, of bins already listed) */
static void set_mixbins(Voice *v, uint32_t mb, int replace) {
    uint32_t n = mb ? M32(mb) : 0, pairs = mb ? M32(mb + 4) : 0;
    if (!v || !mb) return;
    if (replace) v->nbins = 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t bin = M32(pairs + 8 * i);
        float vol = powf(10, (int32_t)M32(pairs + 8 * i + 4) / 2000.0f);
        int k;
        for (k = 0; k < v->nbins && v->bin[k] != bin; k++) {}
        if (k == v->nbins && (!replace || k == 8)) continue;
        if (k == v->nbins) v->bin[v->nbins++] = bin;
        v->bin_volume[k] = vol;
    }
    if (dslog() > 1) {
        fprintf(stderr, "[ds] %s %08X:", replace ? "SetMixBins" : "SetMixBinVolumes", v->obj);
        for (uint32_t i = 0; i < n && i < 8; i++) fprintf(stderr, " bin %u vol %d", M32(pairs + 8 * i), (int32_t)M32(pairs + 8 * i + 4));
        fprintf(stderr, "\n");
    }
}

static double step(const Voice *v) { return (double)(v->freq ? v->freq : v->rate) / AUDIO_RATE * doppler(v); }

/* where a voice's sound goes in this mix: n gains (2: left, right; 6: FL FR C LFE BL BR) and its reverb send */
typedef struct {
    int n;
    float g[6], send;
} Gains;

/* Surround, down the chain of mixin buffers: mix bins to their speakers (the loudest bin on each; the 3D bins 6-9
 * are front left, front right, back left, back right), and a 3D source panned by where it is, not by its bins:
 * constant power left-right as in stereo and front-back the same way, the LFE by distance only */
static void gains6(const Voice *v, float *g) {
    static const int speaker[10] = {0, 1, 2, 3, 4, 5, 0, 1, 4, 5};
    for (int k = 0; k < 6; k++) g[k] = 1;
    for (int depth = 0; v && depth < 5; depth++, v = v->out ? voice(v->out) : NULL) {
        float b[6] = {0};
        if (!v->nbins) {
            for (int k = 0; k < 6; k++) b[k] = v->out || k < 2 ? 1.0f : 0.0f; /* a mixin's input, or the front pair */
        } else
            for (int i = 0; i < v->nbins; i++)
                if (v->bin[i] < 10 && v->bin_volume[i] > b[speaker[v->bin[i]]]) b[speaker[v->bin[i]]] = v->bin_volume[i];
        if (v->is3d && v->mode3d != 2) {
            float d = sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]);
            float dg = dist3d(v, d), x = d > 1e-3f ? v->pos3d[0] / d : 0, z = d > 1e-3f ? v->pos3d[2] / d : 1;
            float a = (x + 1) * 0.78539816f, L = cosf(a) * 1.41421356f, R = sinf(a) * 1.41421356f;
            float F = sqrtf((1 + z) / 2), B = sqrtf((1 - z) / 2), lvl = 0;
            for (int k = 0; k < 6; k++)
                if (k != 3 && b[k] > lvl) lvl = b[k];
            float p[6] = {L * F, R * F, 0, 0, L * B, R * B};
            for (int k = 0; k < 6; k++) b[k] = k == 3 ? b[3] * dg : lvl * dg * p[k];
        }
        for (int k = 0; k < 6; k++) g[k] *= v->volume * v->direct * b[k];
    }
}

/* the reverb send: volumes and distance down the chain, times the room level where one is set (else none) */
static float send_gain(const Voice *v) {
    float g = 1;
    int any = 0;
    for (int depth = 0; v && depth < 5; depth++, v = v->out ? voice(v->out) : NULL) {
        g *= v->volume;
        if (v->is3d && v->mode3d != 2)
            g *= dist3d(v, sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]));
        if (v->room >= 0) g *= v->room, any = 1;
    }
    return any ? g : 0;
}

static void gains(const Voice *v, int channels, Gains *G) {
    G->n = channels >= 6 ? 6 : 2;
    if (G->n == 6) gains6(v, G->g);
    else gain(v, &G->g[0], &G->g[1]);
    G->send = fx_enabled() ? send_gain(v) : 0;
}

/* one interpolated frame into the output: through the direct path's high shelf (gain g, 1: none), then the gains,
 * and into the reverb sends when there are any */
static void emit(Voice *v, float *out, float l, float r, const Gains *G, float g, float *send) {
    if (g < 1) {
        float yl = SHELF_B * (l + v->px[0]) + SHELF_C * v->lp[0], yr = SHELF_B * (r + v->px[1]) + SHELF_C * v->lp[1];
        v->px[0] = l, v->px[1] = r, v->lp[0] = yl, v->lp[1] = yr;
        l = yl + g * (l - yl);
        r = yr + g * (r - yr);
    }
    out[0] += G->g[0] * l;
    out[1] += G->g[1] * r;
    v->energy += (G->g[0] * l) * (G->g[0] * l) + (G->g[1] * r) * (G->g[1] * r);
    if (G->n == 6) {
        float m = (l + r) * 0.5f;
        out[2] += G->g[2] * m, out[3] += G->g[3] * m, out[4] += G->g[4] * l, out[5] += G->g[5] * r;
    }
    if (send) send[0] += G->send * l, send[1] += G->send * r;
}

/* The play region is the sound: positions and loop offsets count from its start, which is also where its ADPCM
 * blocks begin (XACT plays wave-bank entries as regions of the whole bank, and a stereo entry may start half a
 * 72-byte block into it). The game sets the region, then SetCurrentPosition(0), then plays. */
static void mix_buffer_hq(Voice *v, float *out, uint32_t frames, int channels, const Gains *G, float a, float *send,
                          uint32_t data, uint32_t end, uint32_t lstart, uint32_t lend);

static void mix_buffer(Voice *v, float *out, uint32_t frames, int channels, float *send) {
    uint32_t data = M32(v->obj + O_DATA), size = M32(v->obj + O_SIZE), skip = v->region_start < size ? v->region_start : 0;
    uint32_t avail = size - skip, end = to_frames(v, v->region_len && v->region_len < avail ? v->region_len : avail);
    if (!data || !end) { /* mixin buffers carry no data of their own */
        v->playing = 0;
        return;
    }
    data += skip;
    uint32_t start = 0, lstart = v->loop_len ? to_frames(v, v->loop_start) : start;
    uint32_t lend = v->loop_len ? lstart + to_frames(v, v->loop_len) : end;
    if (lend > end || lstart >= lend) lstart = start, lend = end;
    double dt = step(v);
    float a = hf_gain(hf_loss(v));
    Gains G;
    gains(v, channels, &G);
    if (fx_enabled() && dt < 16) {
        mix_buffer_hq(v, out, frames, channels, &G, a, send, data, end, lstart, lend);
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t stop = v->looping ? lend : end;
        if (v->pos >= stop) {
            if (!v->looping) {
                v->playing = 0;
                v->pos = start;
                return;
            }
            v->pos = lstart + fmod(v->pos - lstart, lend - lstart);
        }
        uint32_t n = (uint32_t)v->pos, n1 = n + 1 < stop ? n + 1 : v->looping ? lstart : n;
        float f = (float)(v->pos - n), l0, r0, l1, r1;
        fetch(v, data, n, &l0, &r0);
        fetch(v, data, n1, &l1, &r1);
        emit(v, out + channels * i, l0 + (l1 - l0) * f, r0 + (r1 - r0) * f, &G, a, send ? send + 2 * i : NULL);
        v->pos += dt;
    }
}

/* ENHANCED AUDIO's resampling (audiofx.c): the source frames this stretch reads, gathered once in play order (past
 * a loop's end its start again, past the sound's end silence), then each output frame from FX_TAPS of them */
static void mix_buffer_hq(Voice *v, float *out, uint32_t frames, int channels, const Gains *G, float a, float *send,
                          uint32_t data, uint32_t end, uint32_t lstart, uint32_t lend) {
    static float src[2 * 4200];
    double dt = step(v);
    int cut = fx_cut(dt);
    for (uint32_t done = 0; done < frames;) {
        uint32_t stop = v->looping ? lend : end, chunk = frames - done < 256 ? frames - done : 256;
        if (v->pos >= stop) {
            if (!v->looping) {
                v->playing = 0;
                v->pos = 0;
                return;
            }
            v->pos = lstart + fmod(v->pos - lstart, lend - lstart);
        }
        long base = (long)v->pos - (FX_TAPS / 2 - 1), count = (long)(v->pos + chunk * dt) - base + FX_TAPS + 1;
        for (long k = 0; k < count; k++) {
            long idx = base + k;
            if (v->looping && idx >= (long)lend) idx = lstart + (idx - lstart) % (lend - lstart);
            if (idx < 0 || (!v->looping && idx >= (long)end)) src[2 * k] = src[2 * k + 1] = 0;
            else fetch(v, data, (uint32_t)idx, &src[2 * k], &src[2 * k + 1]);
        }
        for (uint32_t i = 0; i < chunk; i++, done++) {
            if (!v->looping && v->pos >= end) {
                v->playing = 0;
                v->pos = 0;
                return;
            }
            long n = (long)v->pos;
            const float *h = fx_taps(cut, v->pos - n), *s = src + 2 * (n - (FX_TAPS / 2 - 1) - base);
            float l = 0, r = 0;
            for (int t = 0; t < FX_TAPS; t++) l += h[t] * s[2 * t], r += h[t] * s[2 * t + 1];
            emit(v, out + (size_t)channels * done, l, r, G, a, send ? send + 2 * done : NULL);
            v->pos += dt;
        }
    }
}

/* ---------------- stream packets ---------------- */

/* XMEDIAPACKET: pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus, hCompletionEvent, prtTimestamp, pContext.
 * Process copies it: callers may pass one on their stack, only the buffer and the pointed-to fields persist. */
enum { P_DATA = 0, P_SIZE, P_COMPLETED, P_STATUS, P_EVENT, P_CONTEXT = 6 };
#define MAX_PENDING 1024
static struct {
    uint32_t stream, pkt[7];
    int played; /* the mixer is done with it; DirectSoundDoWork completes it */
} g_pending[MAX_PENDING]; /* in submission order: a stream's packets play and complete in order */
static int g_npending;

static int next_packet(uint32_t stream, int from) {
    for (int i = from; i < g_npending; i++)
        if (g_pending[i].stream == stream && !g_pending[i].played) return i;
    return -1;
}

static void mix_stream(Voice *v, float *out, uint32_t frames, int channels, float *send) {
    double dt = step(v);
    float a = hf_gain(hf_loss(v));
    Gains G;
    gains(v, channels, &G);
    int head = next_packet(v->obj, 0);
    for (uint32_t i = 0; i < frames && head >= 0;) {
        uint32_t data = g_pending[head].pkt[P_DATA], len = to_frames(v, g_pending[head].pkt[P_SIZE]);
        if (v->pos >= len) {
            g_pending[head].played = 1;
            v->pos -= len;
            head = next_packet(v->obj, head + 1);
            continue;
        }
        uint32_t n = (uint32_t)v->pos, n1 = n + 1 < len ? n + 1 : n;
        float f = (float)(v->pos - n), l0, r0, l1, r1;
        fetch(v, data, n, &l0, &r0);
        fetch(v, data, n1, &l1, &r1);
        emit(v, out + channels * i, l0 + (l1 - l0) * f, r0 + (r1 - r0) * f, &G, a, send ? send + 2 * i : NULL);
        v->pos += dt;
        i++;
    }
    if (head < 0) v->pos = 0; /* starved: the next packet starts from its beginning */
}

static void mix_part(float *out, uint32_t frames, int channels);

/* audio.c's callback: frames of `channels` (2, or 6 as FL FR C LFE BL BR) interleaved floats */
static void mix(float *out, uint32_t frames, int channels) {
    for (uint32_t done = 0; done < frames;) { /* in parts the reverb sends' buffer holds */
        uint32_t n = frames - done < 4096 ? frames - done : 4096;
        mix_part(out + (size_t)done * channels, n, channels);
        done += n;
    }
}

static void mix_part(float *out, uint32_t frames, int channels) {
    static float sends[2 * 4096];
    int fx = fx_enabled();
    memset(out, 0, (size_t)frames * channels * sizeof *out);
    if (fx) memset(sends, 0, frames * 2 * sizeof *sends);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (!v->obj || v->paused) continue;
        if (v->kind == K_STREAM) mix_stream(v, out, frames, channels, fx ? sends : NULL);
        else if (v->playing) mix_buffer(v, out, frames, channels, fx ? sends : NULL);
    }
    { /* creating build/voices.now prints the five loudest voices every 250 ms for 10 s */
        static ULONGLONG until, next, check;
        static uint32_t mixed;
        ULONGLONG now = GetTickCount64();
        if (now >= check) {
            check = now + 1000;
            if (DeleteFileA("build/voices.now")) {
                until = now + 10000, next = now, mixed = 0;
                for (int i = 0; i < MAX_VOICES; i++) g_voices[i].energy = 0;
            }
        }
        mixed += frames;
        if (now < until && now >= next) {
            int top[5] = {-1, -1, -1, -1, -1};
            for (int i = 0; i < MAX_VOICES; i++)
                for (int k = 0; k < 5; k++)
                    if (g_voices[i].obj && g_voices[i].energy > 0 && (top[k] < 0 || g_voices[i].energy > g_voices[top[k]].energy)) {
                        memmove(top + k + 1, top + k, (4 - k) * sizeof top[0]);
                        top[k] = i;
                        break;
                    }
            fprintf(stderr, "[voices] t+%llu ms\n", 10000 - (until - now));
            for (int k = 0; k < 5 && top[k] >= 0; k++) {
                Voice *v = &g_voices[top[k]];
                float gl, gr;
                gain(v, &gl, &gr);
                fprintf(stderr, "[voices]   %08X rms %.3f %s tag %X ch %u rate %u freq %u play %d loop %d pos %.0f region %u+%u "
                                "loop %u+%u vol %.3f gain %.3f/%.3f bins %d 3d %d out %08X data %08X size %u\n",
                        v->obj, sqrtf(v->energy / mixed), v->kind == K_STREAM ? "stream" : "buffer", v->tag, v->channels,
                        v->rate, v->freq, v->playing, v->looping, v->pos, v->region_start, v->region_len, v->loop_start,
                        v->loop_len, v->volume, gl, gr, v->nbins, v->is3d, v->out, M32(v->obj + O_DATA), M32(v->obj + O_SIZE));
            }
            for (int i = 0; i < MAX_VOICES; i++) g_voices[i].energy = 0;
            next = now + 250, mixed = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (fx) fx_process(out, sends, frames, channels);
    /* soft limiter: linear up to 0.6, then bent smoothly (slope 1 at the knee) under full scale. A race summed past
     * 1.0 on 2% of samples; the APU's per-voice headroom, which would explain why, is not modelled */
    for (uint32_t i = 0; i < frames * (uint32_t)channels; i++) {
        float a = fabsf(out[i]);
        if (a > 0.6f) out[i] = copysignf(0.6f + 0.4f * tanhf((a - 0.6f) / 0.4f), out[i]);
    }
}

/* a voice as DirectSound starts one: full volume, 16-bit stereo at 48 kHz, DS3D's default distances and Doppler,
 * no cone, an unattenuated direct path */
static void voice_init(Voice *v, uint32_t obj, int kind) {
    memset(v, 0, sizeof *v);
    v->obj = obj;
    v->kind = kind;
    v->volume = v->direct = v->doppler = v->cone_outside = 1;
    v->room = -1;
    v->min_dist = 1, v->max_dist = 1e9f;
    v->cone_in = v->cone_out = 360;
    v->tag = 1, v->channels = 2, v->align = 4, v->bits = 16, v->rate = AUDIO_RATE;
}

static void mixer_selftest(void) {
    /* header sample 1000 at step index 0, then nibbles 0 add 7 >> 3 = 0; nibble 7 (the third) adds 0 + 7 + 3 + 1 and
     * moves to index 8 (step 16), where nibble 0 adds 2 */
    uint8_t block[36] = {0xE8, 0x03, 0, 0, 0x00, 0x07};
    int16_t pcm[ADPCM_FRAMES];
    adpcm_block(block, 1, pcm);
    if (pcm[0] != 1000 || pcm[1] != 1000 || pcm[2] != 1000 || pcm[3] != 1011 || pcm[4] != 1013) {
        fprintf(stderr, "runtime: ADPCM self-test failed (%d %d %d %d %d)\n", pcm[0], pcm[1], pcm[2], pcm[3], pcm[4]);
        abort();
    }
    /* 3D: twice the minimum distance straight ahead is half as loud on both sides; hard right is right only */
    static Voice v, w, b;
    voice_init(&v, 0, K_BUFFER);
    v.is3d = 1, v.max_dist = 100, v.pos3d[2] = 2;
    float l, r, l2, r2;
    gain(&v, &l, &r);
    v.pos3d[0] = 2, v.pos3d[2] = 0;
    gain(&v, &l2, &r2);
    /* a one-point curve fades from 1 at the minimum distance to its point at the maximum: halfway, half */
    voice_init(&w, 0, K_BUFFER);
    w.is3d = 1, w.max_dist = 3, w.pos3d[2] = 2, w.ncurve = 1, w.curve[0] = 0;
    float l3, r3;
    gain(&w, &l3, &r3);
    /* mix bins: a submix on the 3D bins and the centre (Forza's) stays at unity; front bins at -64 dB are muted */
    voice_init(&b, 0, K_BUFFER);
    b.nbins = 5, b.bin[0] = 6, b.bin[1] = 8, b.bin[2] = 7, b.bin[3] = 9, b.bin[4] = 2;
    for (int i = 0; i < 5; i++) b.bin_volume[i] = 1;
    float l4, r4, l5, r5;
    gain(&b, &l4, &r4);
    b.nbins = 2, b.bin[0] = 0, b.bin[1] = 1, b.bin_volume[0] = b.bin_volume[1] = powf(10, -6400 / 2000.0f);
    gain(&b, &l5, &r5);
    /* a cone facing away from the listener gives its outside volume (-3 dB), on top of distance (half) */
    v.pos3d[0] = 0, v.pos3d[2] = 2, v.cone_dir[2] = 1, v.cone_in = 110, v.cone_out = 360, v.cone_outside = powf(10, -300 / 2000.0f);
    float l6, r6;
    gain(&v, &l6, &r6);
    /* Doppler: receding at a tenth of the speed of sound lowers the pitch to 1 / 1.1 */
    w.pos3d[2] = 10, w.vel3d[2] = 34.33f;
    double dop = doppler(&w);
    /* the direct path's high shelf, run on a 20 kHz tone: -6 dB settles to half the amplitude; at 100 Hz, none */
    static Voice s;
    voice_init(&s, 0, K_BUFFER);
    float peak_hi = 0, peak_lo = 0, out[2];
    double g = hf_gain(-600);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 4800; i++) {
            float x = (float)sin(2 * 3.14159265358979 * (pass ? 100 : 20000) * i / AUDIO_RATE);
            out[0] = out[1] = 0;
            s.energy = 0;
            Gains unity = {2, {1, 1}, 0};
            emit(&s, out, x, x, &unity, (float)g, NULL);
            if (i > 2400) *(pass ? &peak_lo : &peak_hi) = fmaxf(pass ? peak_lo : peak_hi, fabsf(out[0]));
        }
    double h = peak_hi;
    if (fabsf(l - 0.5f) > 1e-3f || fabsf(r - 0.5f) > 1e-3f || l2 > 1e-3f || fabsf(r2 - 0.70710678f) > 1e-3f ||
        fabsf(l3 - 0.5f) > 1e-3f || fabsf(l4 - 1) > 1e-3f || fabsf(r4 - 1) > 1e-3f || l5 > 1e-3f || r5 > 1e-3f ||
        fabsf(l6 - 0.354f) > 1e-3f || fabs(dop - 1 / 1.1) > 1e-3 || fabs(h - 0.5012) > 0.02 || fabsf(peak_lo - 1) > 0.02f || hf_gain(0) != 1) {
        fprintf(stderr, "runtime: mixer self-test failed (%g %g %g %g %g %g %g %g %g cone %g doppler %g shelf %g)\n", l,
                r, l2, r2, l3, l4, r4, l5, r5, l6, dop, h);
        abort();
    }
}

static void init(void) {
    static int done;
    if (done++) return;
    fx_init();
    mixer_selftest();
    fx_enable(cfg_get("ENHAUDIO", "enhaudio", 1));
    InitializeCriticalSection(&g_lock);
    audio_start(mix);
}

static uint32_t new_object(int kind) {
    init();
    uint32_t o = guest_alloc(O_BYTES, 16);
    M32(o + O_MAGIC) = MAGIC;
    M32(o + O_KIND) = kind;
    M32(o + O_REFS) = 1;
    if (kind == K_DEVICE) return o;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i].obj) {
            voice_init(&g_voices[i], o, kind);
            M32(o + O_VOICE) = i + 1;
            break;
        }
    LeaveCriticalSection(&g_lock);
    if (!M32(o + O_VOICE)) fprintf(stderr, "[ds] out of voices: %08X stays silent\n", o);
    return o;
}

static void set_format(uint32_t o, uint32_t wfx) {
    /* WAVEFORMATEX: wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec, nBlockAlign, wBitsPerSample */
    Voice *v = voice(o);
    if (!wfx || !v) return;
    EnterCriticalSection(&g_lock);
    v->tag = M16(wfx), v->channels = M16(wfx + 2), v->rate = M32(wfx + 4), v->align = M16(wfx + 12), v->bits = M16(wfx + 14);
    v->cache_addr[0] = v->cache_addr[1] = 0;
    if (v->tag != 1 && v->tag != WAVE_FORMAT_XBOX_ADPCM) v->align = 0; /* unknown: silent */
    LeaveCriticalSection(&g_lock);
    if (dslog())
        fprintf(stderr, "[ds] %08X format tag %04X ch %u rate %u bps %u align %u bits %u\n", o, M16(wfx), M16(wfx + 2),
                M32(wfx + 4), M32(wfx + 8), M16(wfx + 12), M16(wfx + 14));
}

static void log_args(X86Ctx *c, const char *name, int n, int ptr_words);

/* ---------------- device ---------------- */

static uint32_t g_device;

void hle_DirectSoundCreate(X86Ctx *c) { /* (pGuid, ppDirectSound, pUnkOuter) */
    if (!g_device) g_device = new_object(K_DEVICE);
    M32(ARG(1)) = g_device;
    ret(c, S_OK, 3);
}
void hle_IDirectSound_Release(X86Ctx *c) { ret(c, 1, 1); } /* the device lives for the whole run */
void hle_IDirectSound_SynchPlayback(X86Ctx *c) { ret(c, S_OK, 1); }
void hle_IDirectSound_CommitDeferredSettings(X86Ctx *c) { ret(c, S_OK, 1); }
void hle_IDirectSound_SetDopplerFactor(X86Ctx *c) { log_args(c, "IDirectSound_SetDopplerFactor", 3, 0); ret(c, S_OK, 3); }
void hle_IDirectSound_SetI3DL2Listener(X86Ctx *c) { log_args(c, "IDirectSound_SetI3DL2Listener", 3, 12); ret(c, S_OK, 3); } /* DSI3DL2LISTENER */
void hle_IDirectSound_SetMixBinHeadroom(X86Ctx *c) { ret(c, S_OK, 3); }
void hle_IDirectSound_SetEffectData(X86Ctx *c) { ret(c, S_OK, 6); }
void hle_IDirectSound_GetEffectData(X86Ctx *c) { /* (pDS, dwEffectIndex, dwOffset, lpvData, dwDataSize) */
    if (ARG(3)) memset(g_mem + ARG(3), 0, ARG(4));
    ret(c, S_OK, 5);
}
void hle_IDirectSound_GetOutputLevels(X86Ctx *c) { /* (pDS, LPDSOUTPUTLEVELS, bResetPeak) */
    if (ARG(1)) memset(g_mem + ARG(1), 0, 0x90);
    ret(c, S_OK, 3);
}
void hle_IDirectSound_DownloadEffectsImage(X86Ctx *c) {
    /* (pDS, lpvImageBuffer, dwImageSize, pImageLoc, ppImageDesc): an image with no effects */
    static uint32_t desc;
    if (!desc) desc = guest_alloc(0x1000, 16); /* DSEFFECTIMAGEDESC: dwEffectCount 0, dwTotalScratchSize 0 */
    if (ARG(4)) M32(ARG(4)) = desc;
    ret(c, S_OK, 5);
}
void hle_XAudioSetEffectData(X86Ctx *c) { ret(c, S_OK, 3); }
void hle_DirectSoundUseFullHRTF(X86Ctx *c) { ret(c, 0, 0); }

/* ---------------- buffers ---------------- */

static uint32_t create_buffer(uint32_t desc) {
    /* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin */
    uint32_t o = new_object(K_BUFFER), bytes = desc ? M32(desc + 8) : 0;
    if (bytes) {
        M32(o + O_DATA) = guest_alloc(bytes, 16);
        M32(o + O_SIZE) = bytes;
    }
    if (dslog() && desc)
        fprintf(stderr, "[ds] buffer %08X flags %08X bytes %u mixbins %08X\n", o, M32(desc + 4), bytes, M32(desc + 0x10));
    if (desc) set_format(o, M32(desc + 0xC));
    EnterCriticalSection(&g_lock);
    if (desc) set_mixbins(voice(o), M32(desc + 0x10), 1);
    LeaveCriticalSection(&g_lock);
    return o;
}
void hle_IDirectSound_CreateSoundBuffer(X86Ctx *c) { /* (pDS, pdsbd, ppBuffer, pUnkOuter) */
    M32(ARG(2)) = create_buffer(ARG(1));
    ret(c, S_OK, 4);
}
void hle_DirectSoundCreateBuffer(X86Ctx *c) { /* (pdsbd, ppBuffer) */
    M32(ARG(1)) = create_buffer(ARG(0));
    ret(c, S_OK, 2);
}

static void play(uint32_t o, uint32_t flags) {
    Voice *v = voice(o);
    count("Play");
    if (dslog() > 1) fprintf(stderr, "[ds] play %08X flags %X data %08X size %u\n", o, flags, M32(o + O_DATA), M32(o + O_SIZE));
    if (!v) return;
    EnterCriticalSection(&g_lock);
    v->playing = 1; /* from where it stopped: a finished one-shot is back at its start */
    v->looping = flags & DSBPLAY_LOOPING;
    LeaveCriticalSection(&g_lock);
}
static void stop(uint32_t o, uint32_t flags) {
    Voice *v = voice(o);
    if (!v) return;
    EnterCriticalSection(&g_lock);
    if (flags & 2) v->looping = 0; /* DSBSTOPEX_RELEASEWAVEFORM: play on to the end */
    else v->playing = 0;
    LeaveCriticalSection(&g_lock);
}

void hle_IDirectSoundBuffer_Play(X86Ctx *c) { play(ARG(0), ARG(3)); ret(c, S_OK, 4); } /* (buf, r1, r2, dwFlags) */
void hle_IDirectSoundBuffer_PlayEx(X86Ctx *c) { play(ARG(0), ARG(3)); ret(c, S_OK, 4); } /* (buf, REFERENCE_TIME, dwFlags) */
void hle_IDirectSoundBuffer_Stop(X86Ctx *c) { stop(ARG(0), 0); ret(c, S_OK, 1); }
void hle_IDirectSoundBuffer_StopEx(X86Ctx *c) { stop(ARG(0), ARG(3)); ret(c, S_OK, 4); } /* (buf, REFERENCE_TIME, dwFlags) */
void hle_IDirectSoundBuffer_GetStatus(X86Ctx *c) { /* (buf, pdwStatus) */
    Voice *v = voice(ARG(0));
    EnterCriticalSection(&g_lock);
    M32(ARG(1)) = v && v->playing ? DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0) : 0;
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 2);
}
void hle_IDirectSoundBuffer_GetCurrentPosition(X86Ctx *c) { /* (buf, pdwPlay, pdwWrite) */
    Voice *v = voice(ARG(0));
    EnterCriticalSection(&g_lock);
    uint32_t pos = v ? to_bytes(v, (uint32_t)v->pos) : 0;
    LeaveCriticalSection(&g_lock);
    if (ARG(1)) M32(ARG(1)) = pos;
    if (ARG(2)) M32(ARG(2)) = pos;
    ret(c, S_OK, 3);
}
void hle_IDirectSoundBuffer_SetCurrentPosition(X86Ctx *c) { /* (buf, dwPlayCursor) */
    Voice *v = voice(ARG(0));
    EnterCriticalSection(&g_lock);
    if (v) v->pos = to_frames(v, ARG(1));
    LeaveCriticalSection(&g_lock);
    if (dslog() > 1) fprintf(stderr, "[ds] SetCurrentPosition %08X %u\n", ARG(0), ARG(1));
    ret(c, S_OK, 2);
}
void hle_IDirectSoundBuffer_SetBufferData(X86Ctx *c) { /* (buf, pvBufferData, dwBufferBytes) */
    count("SetBufferData");
    if (dslog() > 1) fprintf(stderr, "[ds] SetBufferData %08X %08X %u\n", ARG(0), ARG(1), ARG(2));
    Voice *v = voice(ARG(0));
    EnterCriticalSection(&g_lock);
    M32(ARG(0) + O_DATA) = ARG(1);
    M32(ARG(0) + O_SIZE) = ARG(2);
    if (v) v->pos = 0, v->cache_addr[0] = v->cache_addr[1] = 0;
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
void hle_IDirectSoundBuffer_SetFormat(X86Ctx *c) { set_format(ARG(0), ARG(1)); ret(c, S_OK, 2); }
void hle_IDirectSoundBuffer_Lock(X86Ctx *c) {
    /* (buf, dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags) */
    uint32_t o = ARG(0), size = M32(o + O_SIZE), off = size ? ARG(1) % size : 0, want = ARG(2);
    if (ARG(7) & 2 /* DSBLOCK_ENTIREBUFFER */) off = 0, want = size;
    uint32_t first = want <= size - off ? want : size - off;
    M32(ARG(3)) = M32(o + O_DATA) + off;
    M32(ARG(4)) = first;
    if (ARG(5)) M32(ARG(5)) = first < want ? M32(o + O_DATA) : 0;
    if (ARG(6)) M32(ARG(6)) = want - first;
    ret(c, S_OK, 8);
}
void hle_IDirectSoundBuffer_Unlock(X86Ctx *c) { ret(c, S_OK, 5); }
void hle_IDirectSoundBuffer_Release(X86Ctx *c) {
    uint32_t o = ARG(0), refs = M32(o + O_REFS) ? --M32(o + O_REFS) : 0; /* shortcut: guest memory is not reclaimed */
    Voice *v = voice(o);
    if (!refs && v) {
        EnterCriticalSection(&g_lock);
        v->obj = 0;
        LeaveCriticalSection(&g_lock);
    }
    ret(c, refs, 1);
}
void hle_IDirectSoundBuffer_GetVoiceProperties(X86Ctx *c) {
    if (ARG(1)) memset(g_mem + ARG(1), 0, 0x40);
    ret(c, S_OK, 2);
}

/* (obj, value) setters of mixer state, for buffers and streams alike */
#define SETTER(name, field_update)                     \
    void hle_##name(X86Ctx *c) {                       \
        Voice *v = voice(ARG(0));                      \
        uint32_t a = ARG(1);                           \
        count(#name);                                  \
        if (dslog() > 1) fprintf(stderr, "[ds] %s %08X %d\n", #name, ARG(0), (int32_t)a); \
        if (v) {                                       \
            EnterCriticalSection(&g_lock);             \
            field_update;                              \
            LeaveCriticalSection(&g_lock);             \
        }                                              \
        ret(c, S_OK, 2);                               \
    }
/* volume in 1/100 dB; pitch in 1/4096 octave from 48 kHz; frequency in Hz (0: the format's) */
SETTER(IDirectSoundBuffer_SetVolume, v->volume = powf(10, (int32_t)a / 2000.0f))
SETTER(IDirectSoundStream_SetVolume, v->volume = powf(10, (int32_t)a / 2000.0f))
SETTER(IDirectSoundBuffer_SetPitch, v->freq = (uint32_t)(AUDIO_RATE * pow(2, (int32_t)a / 4096.0)))
SETTER(IDirectSoundStream_SetPitch, v->freq = (uint32_t)(AUDIO_RATE * pow(2, (int32_t)a / 4096.0)))
SETTER(IDirectSoundBuffer_SetFrequency, v->freq = a)
SETTER(IDirectSoundBuffer_SetOutputBuffer, v->out = a)
SETTER(IDirectSoundStream_SetOutputBuffer, v->out = a)
SETTER(IDirectSoundStream_Pause, v->paused = a & 1) /* DSSTREAMPAUSE_PAUSE */

void hle_IDirectSoundBuffer_SetLoopRegion(X86Ctx *c) { /* (buf, dwLoopStart, dwLoopLength) */
    Voice *v = voice(ARG(0));
    count("SetLoopRegion");
    if (dslog() > 1) fprintf(stderr, "[ds] SetLoopRegion %08X %u+%u\n", ARG(0), ARG(1), ARG(2));
    EnterCriticalSection(&g_lock);
    if (v) v->loop_start = ARG(1), v->loop_len = ARG(2);
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
void hle_IDirectSoundBuffer_SetPlayRegion(X86Ctx *c) { /* (buf, dwPlayStart, dwPlayLength) */
    Voice *v = voice(ARG(0));
    count("SetPlayRegion");
    if (dslog() > 1) fprintf(stderr, "[ds] SetPlayRegion %08X %u+%u\n", ARG(0), ARG(1), ARG(2));
    EnterCriticalSection(&g_lock);
    if (v) v->region_start = ARG(1), v->region_len = ARG(2), v->pos = 0; /* positions are within the region */
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
void hle_IDirectSoundStream_SetFormat(X86Ctx *c) { set_format(ARG(0), ARG(1)); ret(c, S_OK, 2); }

/* 3D source state, applied at once (DS3D_DEFERRED is treated as immediate); DSLOG shows the first calls' values */
static float argf(X86Ctx *c, int i) {
    float f;
    uint32_t bits = ARG(i);
    memcpy(&f, &bits, 4);
    return f;
}
static int log3d(const char *name) {
    static struct { const char *name; int n; } seen[32];
    int i;
    for (i = 0; i < 32 && seen[i].name && seen[i].name != name; i++) {}
    if (i == 32) return 0;
    seen[i].name = name;
    count(name);
    return dslog() && seen[i].n++ < 200;
}
/* setters not modelled yet: DSLOG shows their first calls' arguments, or the struct ARG(1) points to (ptr_words) */
static void log_args(X86Ctx *c, const char *name, int n, int ptr_words) {
    if (!log3d(name)) return;
    fprintf(stderr, "[ds] %s %08X:", name, ARG(0));
    for (int i = 0; ptr_words && i < ptr_words; i++) fprintf(stderr, " %08X", ARG(1) ? M32(ARG(1) + 4 * i) : 0);
    for (int i = 1; !ptr_words && i < n; i++) fprintf(stderr, " %08X", ARG(i));
    fprintf(stderr, "\n");
}
#define LOGGED(name, n, ptr_words)              \
    void hle_##name(X86Ctx *c) {                \
        log_args(c, #name, n, ptr_words);       \
        ret(c, S_OK, n);                        \
    }
void hle_IDirectSoundBuffer_SetPosition(X86Ctx *c) { /* (buf, x, y, z, dwApply) */
    Voice *v = voice(ARG(0));
    if (log3d("SetPosition")) fprintf(stderr, "[ds] SetPosition %08X %g %g %g\n", ARG(0), argf(c, 1), argf(c, 2), argf(c, 3));
    EnterCriticalSection(&g_lock);
    if (v) v->is3d = 1, v->pos3d[0] = argf(c, 1), v->pos3d[1] = argf(c, 2), v->pos3d[2] = argf(c, 3);
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 5);
}
void hle_IDirectSoundBuffer_SetMode(X86Ctx *c) { /* (buf, dwMode, dwApply) */
    Voice *v = voice(ARG(0));
    if (log3d("SetMode")) fprintf(stderr, "[ds] SetMode %08X %u\n", ARG(0), ARG(1));
    EnterCriticalSection(&g_lock);
    if (v) v->mode3d = ARG(1);
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
void hle_IDirectSoundBuffer_SetMinDistance(X86Ctx *c) { /* (buf, flMinDistance, dwApply) */
    Voice *v = voice(ARG(0));
    if (log3d("SetMinDistance")) fprintf(stderr, "[ds] SetMinDistance %08X %g\n", ARG(0), argf(c, 1));
    EnterCriticalSection(&g_lock);
    if (v) v->min_dist = argf(c, 1);
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
void hle_IDirectSoundBuffer_SetMaxDistance(X86Ctx *c) { /* (buf, flMaxDistance, dwApply) */
    Voice *v = voice(ARG(0));
    if (log3d("SetMaxDistance")) fprintf(stderr, "[ds] SetMaxDistance %08X %g\n", ARG(0), argf(c, 1));
    EnterCriticalSection(&g_lock);
    if (v) v->max_dist = argf(c, 1);
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
void hle_IDirectSoundBuffer_SetRolloffCurve(X86Ctx *c) { /* (buf, pflPoints, dwPointCount, dwApply) */
    Voice *v = voice(ARG(0));
    uint32_t n = ARG(2) < 32 ? ARG(2) : 32;
    if (log3d("SetRolloffCurve") && n)
        fprintf(stderr, "[ds] SetRolloffCurve %08X %u points %g .. %g\n", ARG(0), ARG(2), MF32(ARG(1)), MF32(ARG(1) + 4 * (n - 1)));
    EnterCriticalSection(&g_lock);
    if (v) {
        v->ncurve = ARG(1) ? n : 0;
        for (int i = 0; i < v->ncurve; i++) v->curve[i] = MF32(ARG(1) + 4 * i);
    }
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 4);
}

/* setters that only shape sound the mixer does not model (see the shortcut above) */
#define SILENT(name, n)        \
    void hle_##name(X86Ctx *c) { \
        count(#name);            \
        ret(c, S_OK, n);         \
    }
/* (obj, LPCDSMIXBINS), buffers and streams alike */
#define MIXBINS(name, replace)                       \
    void hle_##name(X86Ctx *c) {                     \
        count(#name);                                \
        EnterCriticalSection(&g_lock);               \
        set_mixbins(voice(ARG(0)), ARG(1), replace); \
        LeaveCriticalSection(&g_lock);               \
        ret(c, S_OK, 2);                             \
    }
MIXBINS(IDirectSoundBuffer_SetMixBins, 1)
MIXBINS(IDirectSoundBuffer_SetMixBinVolumes_8, 0)
MIXBINS(IDirectSoundStream_SetMixBins, 1)
MIXBINS(IDirectSoundStream_SetMixBinVolumes_8, 0)
SILENT(IDirectSoundBuffer_Set3DVoiceData, 2) /* all-zero data in Forza's menus */
SILENT(IDirectSound3DCalculator_Calculate3D, 2)
LOGGED(IDirectSoundBuffer_SetFilter, 2, 6) /* DSFILTERDESC */
LOGGED(IDirectSoundBuffer_SetEG, 2, 10) /* DSENVELOPEDESC */
LOGGED(IDirectSoundBuffer_SetLFO, 2, 6) /* DSLFODESC */
LOGGED(IDirectSoundBuffer_SetHeadroom, 2, 0)
/* 3D effect setters: (buf, values..., dwApply), logged like the unmodelled ones, applied at once */
#define EFFECT(name, n, ptr_words, update)          \
    void hle_##name(X86Ctx *c) {                    \
        Voice *v = voice(ARG(0));                   \
        log_args(c, #name, n, ptr_words);           \
        EnterCriticalSection(&g_lock);              \
        if (v) { update; }                          \
        LeaveCriticalSection(&g_lock);              \
        ret(c, S_OK, n);                            \
    }
EFFECT(IDirectSoundBuffer_SetVelocity, 5, 0, for (int i = 0; i < 3; i++) v->vel3d[i] = argf(c, 1 + i))
EFFECT(IDirectSoundBuffer_SetDopplerFactor, 3, 0, v->doppler = argf(c, 1))
EFFECT(IDirectSoundBuffer_SetConeOrientation, 5, 0, for (int i = 0; i < 3; i++) v->cone_dir[i] = argf(c, 1 + i))
EFFECT(IDirectSoundBuffer_SetConeAngles, 4, 0, v->cone_in = (float)ARG(1); v->cone_out = (float)ARG(2))
EFFECT(IDirectSoundBuffer_SetConeOutsideVolume, 3, 0, v->cone_outside = powf(10, (int32_t)ARG(1) / 2000.0f))
/* DSI3DL2BUFFER: lDirect, lDirectHF, lRoom, lRoomHF, flRoomRolloffFactor, Obstruction {lHFLevel, flLFRatio},
 * Occlusion {lHFLevel, flLFRatio} (1/100 dB). Obstruction and occlusion take lHFLevel at high frequencies and
 * lHFLevel * flLFRatio at low ones; the room (reverb) sends are dropped, as no listener room is ever set */
void hle_IDirectSoundBuffer_SetI3DL2Source(X86Ctx *c) { /* (buf, pds3db, dwApply) */
    Voice *v = voice(ARG(0));
    uint32_t p = ARG(1);
    log_args(c, "IDirectSoundBuffer_SetI3DL2Source", 3, 9);
    if (!v || !p) {
        ret(c, S_OK, 3);
        return;
    }
    float obs = (float)(int32_t)M32(p + 20), obs_lf = MF32(p + 24), occ = (float)(int32_t)M32(p + 28);
    float occ_lf = MF32(p + 32), lf = (float)(int32_t)M32(p) + obs * obs_lf + occ * occ_lf;
    EnterCriticalSection(&g_lock);
    v->direct = powf(10, lf / 2000);
    float room = (float)(int32_t)M32(p + 8) + occ * occ_lf; /* the room path is occluded too, not obstructed */
    v->room = room <= -10000 ? 0 : powf(10, room / 2000);
    v->direct_hf = (float)(int32_t)M32(p + 4) + obs * (1 - obs_lf) + occ * (1 - occ_lf);
    LeaveCriticalSection(&g_lock);
    ret(c, S_OK, 3);
}
SILENT(IDirectSoundBuffer_Use3DVoiceData, 2)
LOGGED(IDirectSoundStream_SetFilter, 2, 6)
LOGGED(IDirectSoundStream_SetEG, 2, 10)
LOGGED(IDirectSoundStream_SetLFO, 2, 6)
LOGGED(IDirectSoundStream_SetHeadroom, 2, 0)
SILENT(IDirectSoundStream_Set3DVoiceData, 2)
SILENT(IDirectSoundStream_Use3DVoiceData, 2)

void hle_IDirectSoundStream_GetVoiceProperties(X86Ctx *c) {
    if (ARG(1)) memset(g_mem + ARG(1), 0, 0x40);
    ret(c, S_OK, 2);
}
void hle_IDirectSound3DCalculator_GetVoiceData(X86Ctx *c) {
    for (int i = 1; i < 5; i++)
        if (ARG(i) > 0x10000) M32(ARG(i)) = 0;
    ret(c, S_OK, 5);
}

/* ---------------- streams ---------------- */

static void complete(X86Ctx *c, uint32_t stream, const uint32_t *pkt, uint32_t status) {
    if (pkt[P_COMPLETED]) M32(pkt[P_COMPLETED]) = status ? 0 : pkt[P_SIZE];
    if (pkt[P_STATUS]) M32(pkt[P_STATUS]) = status;
    if (pkt[P_EVENT]) M32(pkt[P_EVENT] + 4) = 1; /* signal the completion event (handle = KEVENT) */
    if (M32(stream + O_DATA)) { /* LPFNXMOCALLBACK(pStreamContext, pPacketContext, dwStatus) */
        uint32_t args[3] = {M32(stream + O_SIZE), pkt[P_CONTEXT], status};
        guest_call(c, M32(stream + O_DATA), 3, args);
    }
}

/* completes a stream's packets (stream 0: every stream's), all of them or (played) those the mixer has finished;
 * one at a time, with the lock released, since a callback may queue the next packet */
static void complete_stream(X86Ctx *c, uint32_t stream, uint32_t status, int played) {
    for (;;) {
        uint32_t s = 0, p[7];
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_npending; i++)
            if ((!stream || g_pending[i].stream == stream) && (!played || g_pending[i].played)) {
                s = g_pending[i].stream;
                memcpy(p, g_pending[i].pkt, sizeof p);
                memmove(&g_pending[i], &g_pending[i + 1], (--g_npending - i) * sizeof g_pending[0]);
                Voice *v = voice(s);
                if (!played && v) v->pos = 0; /* flushed: the next packet plays from its start */
                break;
            }
        LeaveCriticalSection(&g_lock);
        if (!s) return;
        complete(c, s, p, status);
    }
}

void hle_DirectSoundDoWork(X86Ctx *c) {
    complete_stream(c, 0, 0, 1); /* XMEDIAPACKET_STATUS_SUCCESS */
    static uint32_t last;
    if (dslog() && now_ms() - last > 10000) {
        last = now_ms();
        fprintf(stderr, "[ds] calls:");
        for (int i = 0; i < 64 && g_calls[i].name; i++) fprintf(stderr, " %s %u", g_calls[i].name, g_calls[i].calls);
        fprintf(stderr, "\n");
    }
    ret(c, 0, 0);
}

/* XMediaObject vtable entries (stdcall, `this` first) */
static void xmo_AddRef(X86Ctx *c) { ret(c, ++M32(ARG(0) + O_REFS), 1); }
static void xmo_Release(X86Ctx *c) { /* the last release flushes: the caller frees its packets next */
    uint32_t s = ARG(0), refs = M32(s + O_REFS) ? --M32(s + O_REFS) : 0;
    if (!refs) {
        complete_stream(c, s, 0x8000000Bu, 0); /* XMEDIAPACKET_STATUS_FLUSHED */
        Voice *v = voice(s);
        EnterCriticalSection(&g_lock);
        if (v) v->obj = 0;
        LeaveCriticalSection(&g_lock);
    }
    ret(c, refs, 1);
}
static void xmo_GetInfo(X86Ctx *c) { /* XMEDIAINFO: dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead */
    memset(g_mem + ARG(1), 0, 16);
    M32(ARG(1) + 4) = 0x800;
    ret(c, S_OK, 2);
}
static void xmo_GetStatus(X86Ctx *c) {
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_npending; i++) n += g_pending[i].stream == ARG(0);
    LeaveCriticalSection(&g_lock);
    M32(ARG(1)) = n < (int)M32(ARG(0) + O_MAXPKT) ? 1 /* XMO_STATUSF_ACCEPT_INPUT_DATA */ : 0;
    ret(c, S_OK, 2);
}
static void xmo_Process(X86Ctx *c) { /* (this, pInputPacket, pOutputPacket) */
    uint32_t pkt = ARG(1);
    if (pkt) {
        if (M32(pkt + 0xC)) M32(M32(pkt + 0xC)) = E_PENDING;
        if (M32(pkt + 8)) M32(M32(pkt + 8)) = 0;
        if (g_npending == MAX_PENDING) complete_stream(c, 0, 0, 0);
        EnterCriticalSection(&g_lock);
        g_pending[g_npending].stream = ARG(0);
        g_pending[g_npending].played = 0;
        memcpy(g_pending[g_npending++].pkt, g_mem + pkt, 28);
        LeaveCriticalSection(&g_lock);
        if (dslog() >= 3) { /* DSLOG=3: the stream's data as submitted, to build/stream_<object>.raw */
            char path[64];
            snprintf(path, sizeof path, "build/stream_%08X.raw", ARG(0));
            FILE *f = fopen(path, "ab");
            if (f) fwrite(g_mem + M32(pkt), 1, M32(pkt + 4), f), fclose(f);
            if (log3d("Process")) fprintf(stderr, "[ds] Process %08X data %08X bytes %u\n", ARG(0), M32(pkt), M32(pkt + 4));
        }
    }
    ret(c, S_OK, 3);
}
static void xmo_Discontinuity(X86Ctx *c) { ret(c, S_OK, 1); }
static void xmo_Flush(X86Ctx *c) {
    complete_stream(c, ARG(0), 0x8000000Bu, 0); /* XMEDIAPACKET_STATUS_FLUSHED */
    ret(c, S_OK, 1);
}

static uint32_t create_stream(uint32_t desc) {
    /* DSSTREAMDESC: dwFlags, dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins */
    static uint32_t vtable;
    if (!vtable) {
        void (*fns[7])(X86Ctx *) = {xmo_AddRef, xmo_Release, xmo_GetInfo, xmo_GetStatus, xmo_Process,
                                    xmo_Discontinuity, xmo_Flush};
        vtable = guest_alloc(7 * 4, 16);
        for (int i = 0; i < 7; i++) M32(vtable + 4 * i) = hle_thunk(fns[i]);
    }
    uint32_t o = new_object(K_STREAM);
    if (dslog()) fprintf(stderr, "[ds] stream %08X flags %08X packets %u\n", o, M32(desc), M32(desc + 4));
    M32(o + O_VTABLE) = vtable;
    M32(o + O_MAXPKT) = M32(desc + 4) ? M32(desc + 4) : 1;
    set_format(o, M32(desc + 8));
    EnterCriticalSection(&g_lock);
    set_mixbins(voice(o), M32(desc + 0x14), 1);
    LeaveCriticalSection(&g_lock);
    M32(o + O_DATA) = M32(desc + 0xC);
    M32(o + O_SIZE) = M32(desc + 0x10);
    return o;
}
void hle_IDirectSound_CreateSoundStream(X86Ctx *c) { /* (pDS, pdssd, ppStream, pUnkOuter) */
    M32(ARG(2)) = create_stream(ARG(1));
    ret(c, S_OK, 4);
}
void hle_DirectSoundCreateStream(X86Ctx *c) { /* (pdssd, ppStream) */
    M32(ARG(1)) = create_stream(ARG(0));
    ret(c, S_OK, 2);
}
void hle_IDirectSoundStream_FlushEx(X86Ctx *c) { /* (stream, REFERENCE_TIME, dwFlags) */
    complete_stream(c, ARG(0), 0x8000000Bu, 0);
    ret(c, S_OK, 4);
}
