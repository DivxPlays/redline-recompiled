/* ENHANCED AUDIO (enhaudio): what the mixer adds when it is on. Off, the mix is the one the port always made.
 * - Resampling: an 8-tap Kaiser-windowed sinc in place of linear interpolation. Most of the game's sounds are 8-22
 *   kHz, and linear interpolation both dulls them (its response falls to -4 dB at half their rate) and mirrors them
 *   into the highs; when a sound is pitched up (engines), the cutoff follows the pitch so the highs it pushes past
 *   the output's band are filtered, not folded back.
 * - Reverb: the game sends every 3D source to I3DL2's room (lRoom, -12 dB) but never sets a listener environment,
 *   so the Xbox played those sends into nothing. Here they feed a Freeverb-style stereo reverb (8 combs, 4 allpasses
 *   a side) tuned as an open-air venue: a short tail that gives the stands, walls and tunnels some space.
 * - Tone: a low shelf (+3 dB at 100 Hz), a high shelf (+1.5 dB at 10 kHz), the stereo image widened a quarter, and
 *   with surround speakers the bass of all channels also on the LFE.
 */
#include <math.h>
#include <string.h>

#include "xbox.h"

static volatile int g_on = 1;
void fx_enable(int on) { g_on = on; }
int fx_enabled(void) { return g_on; }

/* ---------------- resampling kernels ---------------- */

#define PHASES 256
static const float g_cut[FX_CUTS] = {0.95f, 0.8f, 0.65f, 0.5f, 0.38f, 0.28f};
static float g_kern[FX_CUTS][PHASES + 1][FX_TAPS];

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; k++) term *= (x / (2 * k)) * (x / (2 * k)), sum += term;
    return sum;
}

static void kernels(void) {
    const double beta = 6, norm = bessel_i0(beta);
    for (int c = 0; c < FX_CUTS; c++)
        for (int p = 0; p <= PHASES; p++) {
            double f = (double)p / PHASES, sum = 0, h[FX_TAPS];
            for (int t = 0; t < FX_TAPS; t++) {
                double x = (t - (FX_TAPS / 2 - 1)) - f, y = g_cut[c] * x, r = x / (FX_TAPS / 2);
                double s = fabs(y) < 1e-9 ? 1 : sin(3.14159265358979 * y) / (3.14159265358979 * y);
                double w = fabs(r) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - r * r)) / norm;
                h[t] = g_cut[c] * s * w, sum += h[t];
            }
            for (int t = 0; t < FX_TAPS; t++) g_kern[c][p][t] = (float)(h[t] / sum); /* unity at DC */
        }
}

int fx_cut(double step) { /* the widest cutoff not past the output's band for a sound read `step` frames a frame */
    double want = step > 1 ? 0.95 / step : 0.95;
    int c = 0;
    while (c < FX_CUTS - 1 && g_cut[c] > want + 1e-6) c++;
    return c;
}

const float *fx_taps(int cut, double frac) { return g_kern[cut][(int)(frac * PHASES + 0.5)]; }

/* ---------------- reverb ---------------- */

static const int g_comb_len[8] = {1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760}; /* Freeverb's, at 48 kHz */
static const int g_ap_len[4] = {605, 480, 371, 245};
#define SPREAD 25
static float g_comb_buf[2][8][1760 + SPREAD], g_comb_store[2][8], g_ap_buf[2][4][605 + SPREAD];
static int g_comb_pos[2][8], g_ap_pos[2][4];
#define FEEDBACK 0.80f /* tail of about 1.3 s */
#define DAMP 0.35f

static float reverb_side(int s, float in) {
    float out = 0;
    for (int i = 0; i < 8; i++) {
        int len = g_comb_len[i] + s * SPREAD, *p = &g_comb_pos[s][i];
        float y = g_comb_buf[s][i][*p];
        g_comb_store[s][i] = y * (1 - DAMP) + g_comb_store[s][i] * DAMP;
        g_comb_buf[s][i][*p] = in + g_comb_store[s][i] * FEEDBACK;
        if (++*p >= len) *p = 0;
        out += y;
    }
    for (int i = 0; i < 4; i++) {
        int len = g_ap_len[i] + s * SPREAD, *p = &g_ap_pos[s][i];
        float b = g_ap_buf[s][i][*p];
        g_ap_buf[s][i][*p] = out + b * 0.5f;
        if (++*p >= len) *p = 0;
        out = b - out;
    }
    return out;
}

/* ---------------- tone ---------------- */

typedef struct {
    float b0, b1, b2, a1, a2;
} Biquad;
static Biquad g_low, g_high, g_sub;
static float g_z[8][3][2]; /* per channel and filter: the transposed direct form II state */

static Biquad shelf(int high, double hz, double db) { /* RBJ cookbook shelves, slope 1 */
    double A = pow(10, db / 40), w = 2 * 3.14159265358979 * hz / AUDIO_RATE, c = cos(w), al = sin(w) / 2 * sqrt(2);
    double sa = 2 * sqrt(A) * al, b0, b1, b2, a0, a1, a2;
    if (high) {
        b0 = A * ((A + 1) + (A - 1) * c + sa), b1 = -2 * A * ((A - 1) + (A + 1) * c), b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa, a1 = 2 * ((A - 1) - (A + 1) * c), a2 = (A + 1) - (A - 1) * c - sa;
    } else {
        b0 = A * ((A + 1) - (A - 1) * c + sa), b1 = 2 * A * ((A - 1) - (A + 1) * c), b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa, a1 = -2 * ((A - 1) + (A + 1) * c), a2 = (A + 1) + (A - 1) * c - sa;
    }
    return (Biquad){(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0), (float)(a1 / a0), (float)(a2 / a0)};
}

static Biquad lowpass(double hz) { /* RBJ low-pass, Q 0.707 */
    double w = 2 * 3.14159265358979 * hz / AUDIO_RATE, c = cos(w), al = sin(w) / (2 * 0.7071), a0 = 1 + al;
    return (Biquad){(float)((1 - c) / 2 / a0), (float)((1 - c) / a0), (float)((1 - c) / 2 / a0), (float)(-2 * c / a0),
                    (float)((1 - al) / a0)};
}

static float run(const Biquad *f, float *z, float x) {
    float y = f->b0 * x + z[0];
    z[0] = f->b1 * x - f->a1 * y + z[1];
    z[1] = f->b2 * x - f->a2 * y;
    return y;
}

void fx_init(void) {
    kernels();
    g_low = shelf(0, 100, 3), g_high = shelf(1, 10000, 1.5), g_sub = lowpass(120);
}

/* the mix (channels interleaved: 2, or 6 as FL FR C LFE BL BR) and its reverb sends (stereo) -> the enhanced mix */
void fx_process(float *out, const float *send, uint32_t frames, int channels) {
    if (!g_on) return;
    for (uint32_t i = 0; i < frames; i++) {
        float *o = out + (size_t)i * channels;
        float in = (send[2 * i] + send[2 * i + 1]) * 0.015f, wl = reverb_side(0, in), wr = reverb_side(1, in);
        o[0] += wl, o[1] += wr;
        if (channels >= 6) o[4] += wl * 0.7f, o[5] += wr * 0.7f; /* the tail from all around */
        float m = (o[0] + o[1]) * 0.5f, s = (o[0] - o[1]) * 0.5f * 1.25f; /* a quarter wider */
        o[0] = m + s, o[1] = m - s;
        float bass = 0;
        for (int k = 0; k < channels && k < 8; k++) {
            if (channels >= 6 && k == 3) continue; /* the LFE has its own: below */
            o[k] = run(&g_high, g_z[k][1], run(&g_low, g_z[k][0], o[k]));
            bass += o[k];
        }
        if (channels >= 6) o[3] += run(&g_sub, g_z[3][2], bass) * 0.3f; /* bass management for a subwoofer */
    }
}
