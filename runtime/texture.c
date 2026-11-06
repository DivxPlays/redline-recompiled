/* NV2A texture formats -> host upload data (BGRA8, or BC1-3 for DXT, which the NV2A stores unswizzled).
 * NV097_SET_TEXTURE_FORMAT: [1:0] DMA, [2] cube, [7:4] dimensions, [15:8] colour format, [19:16] mip levels,
 * [23:20] log2 width, [27:24] log2 height, [31:28] log2 depth of a volume. Linear ("rect") formats take their size
 * from IMAGE_RECT and their pitch from CONTROL1, have one level, and are sampled with unnormalised coordinates. A cube
 * map is six faces, each with its mip chain and starting 128-byte aligned. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pgraph.h"

enum { K_ARGB8, K_XRGB8, K_565, K_1555, K_X555, K_4444, K_L8, K_AY8, K_A8, K_A8L8, K_DXT1, K_DXT3, K_DXT5, K_P8 };
typedef struct {
    int kind, swizzled, bpp; /* bpp in bytes; 0 for DXT */
} FormatInfo;

static int format_info(uint32_t color, FormatInfo *f) {
    switch (color) {
    case 0x00: *f = (FormatInfo){K_L8, 1, 1}; return 1;      /* L8 */
    case 0x02: *f = (FormatInfo){K_1555, 1, 2}; return 1;    /* A1R5G5B5 */
    case 0x03: *f = (FormatInfo){K_X555, 1, 2}; return 1;    /* X1R5G5B5 */
    case 0x04: *f = (FormatInfo){K_4444, 1, 2}; return 1;    /* A4R4G4B4 */
    case 0x05: *f = (FormatInfo){K_565, 1, 2}; return 1;     /* R5G6B5 */
    case 0x06: *f = (FormatInfo){K_ARGB8, 1, 4}; return 1;   /* A8R8G8B8 */
    case 0x07: *f = (FormatInfo){K_XRGB8, 1, 4}; return 1;   /* X8R8G8B8 */
    case 0x0B: *f = (FormatInfo){K_P8, 1, 1}; return 1;      /* I8 into an A8R8G8B8 palette */
    case 0x0C: *f = (FormatInfo){K_DXT1, 0, 0}; return 1;
    case 0x0E: *f = (FormatInfo){K_DXT3, 0, 0}; return 1;
    case 0x0F: *f = (FormatInfo){K_DXT5, 0, 0}; return 1;
    case 0x10: *f = (FormatInfo){K_1555, 0, 2}; return 1;    /* linear */
    case 0x11: *f = (FormatInfo){K_565, 0, 2}; return 1;
    case 0x12: *f = (FormatInfo){K_ARGB8, 0, 4}; return 1;
    case 0x13: *f = (FormatInfo){K_L8, 0, 1}; return 1;      /* Y8 */
    case 0x19: *f = (FormatInfo){K_A8, 1, 1}; return 1;
    case 0x1A: *f = (FormatInfo){K_A8L8, 1, 2}; return 1;
    case 0x1B: *f = (FormatInfo){K_AY8, 0, 1}; return 1;     /* AY8, linear (the livery editor's vinyl shapes) */
    case 0x1C: *f = (FormatInfo){K_X555, 0, 2}; return 1;
    case 0x1D: *f = (FormatInfo){K_4444, 0, 2}; return 1;
    case 0x1E: *f = (FormatInfo){K_XRGB8, 0, 4}; return 1;
    case 0x1F: *f = (FormatInfo){K_A8, 0, 1}; return 1;      /* A8, linear */
    default: return 0;
    }
}

/* NV2A swizzle: interleave x, y and z bits, in that order, while each still has bits */
uint32_t swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d) {
    uint32_t off = 0, bit = 1;
    for (uint32_t m = 1; m < w || m < h || m < d; m <<= 1) {
        if (m < w) { if (x & m) off |= bit; bit <<= 1; }
        if (m < h) { if (y & m) off |= bit; bit <<= 1; }
        if (m < d) { if (z & m) off |= bit; bit <<= 1; }
    }
    return off;
}

static uint32_t expand(uint32_t v, int bits) { return bits == 1 ? (v ? 255 : 0) : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)); }

static uint32_t to_bgra(int kind, const uint8_t *p) {
    uint32_t v, a = 255, r, g, b;
    switch (kind) {
    case K_ARGB8: memcpy(&v, p, 4); return v;
    case K_XRGB8: memcpy(&v, p, 4); return v | 0xFF000000u;
    case K_565: v = p[0] | p[1] << 8; r = expand(v >> 11, 5); g = expand((v >> 5) & 63, 6); b = expand(v & 31, 5); break;
    case K_1555: v = p[0] | p[1] << 8; a = v >> 15 ? 255 : 0; /* fall through */
    case K_X555:
        v = p[0] | p[1] << 8; r = expand((v >> 10) & 31, 5); g = expand((v >> 5) & 31, 5); b = expand(v & 31, 5);
        if (kind == K_X555) a = 255;
        break;
    case K_4444: v = p[0] | p[1] << 8; a = (v >> 12) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; break;
    case K_L8: r = g = b = p[0]; break;
    case K_AY8: r = g = b = a = p[0]; break;
    case K_A8: r = g = b = 255; a = p[0]; break; /* white, as xemu has it: the livery editor's vinyls are A8 masks
                                                  * tinted by the colour chosen (black ones whatever the colour) */
    case K_A8L8: r = g = b = p[0]; a = p[1]; break;
    default: return 0xFFFF00FF;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

/* level-0 size and level count; *d is a volume's depth, else 1 */
static uint32_t levels_of(const TexDesc *t, const FormatInfo *f, uint32_t *w, uint32_t *h, uint32_t *d) {
    *d = tex_kind(t->format) == TK_3D ? 1u << ((t->format >> 28) & 15) : 1;
    if (!f->swizzled && f->bpp) { /* linear: size from IMAGE_RECT */
        *w = t->rect >> 16;
        *h = t->rect & 0xFFFF;
        return 1;
    }
    *w = 1u << ((t->format >> 20) & 15);
    *h = 1u << ((t->format >> 24) & 15);
    uint32_t n = (t->format >> 16) & 15;
    return n ? n : 1;
}

static uint32_t max1(uint32_t v) { return v ? v : 1; }

/* One DXT block (BC1 8 bytes; BC2/BC3 16, alpha first) to 4x4 BGRA pixels. For textures D3D11 cannot take
 * compressed: a block-compressed top level must be a multiple of 4 each way, and Forza has a 32x2 DXT5. */
static void dxt_block(int kind, const uint8_t *p, uint32_t out[16]) {
    uint8_t alpha[16];
    if (kind == K_DXT3)
        for (int i = 0; i < 16; i++) alpha[i] = ((p[i / 2] >> (4 * (i & 1))) & 15) * 17;
    if (kind == K_DXT5) {
        uint8_t a[8] = {p[0], p[1]};
        for (int i = 2; i < 8; i++)
            a[i] = p[0] > p[1] ? (uint8_t)(((8 - i) * p[0] + (i - 1) * p[1]) / 7)
                   : i < 6     ? (uint8_t)(((6 - i) * p[0] + (i - 1) * p[1]) / 5)
                               : (i == 6 ? 0 : 255);
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)p[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
    }
    if (kind != K_DXT1) p += 8;
    uint32_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8, idx = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
    uint32_t col[4];
    int r[4], g[4], b[4];
    for (int k = 0; k < 2; k++) {
        uint32_t c = k ? c1 : c0;
        r[k] = expand(c >> 11, 5), g[k] = expand((c >> 5) & 63, 6), b[k] = expand(c & 31, 5);
    }
    int four = kind != K_DXT1 || c0 > c1; /* BC1 with c0 <= c1: three colours and transparent black */
    for (int k = 2; k < 4; k++) {
        int w0 = four ? (k == 2 ? 2 : 1) : 1, w1 = four ? (k == 2 ? 1 : 2) : 1, d = four ? 3 : 2;
        r[k] = (w0 * r[0] + w1 * r[1]) / d, g[k] = (w0 * g[0] + w1 * g[1]) / d, b[k] = (w0 * b[0] + w1 * b[1]) / d;
    }
    for (int k = 0; k < 4; k++) col[k] = 0xFF000000u | r[k] << 16 | g[k] << 8 | b[k];
    if (!four) col[3] = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t c = col[(idx >> (2 * i)) & 3];
        out[i] = kind == K_DXT1 ? c : (c & 0xFFFFFF) | (uint32_t)alpha[i] << 24;
    }
}

void texture_selftest(void) {
    /* BC1 red/blue endpoints: index 0 red, 1 blue, 2 two-thirds red; BC3 alpha endpoints 255 and 0, pixels 0 and 1 */
    uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0x24, 0, 0, 0}, bc3[16] = {255, 0, 0x08};
    uint32_t px[16], px3[16];
    dxt_block(K_DXT1, bc1, px);
    memcpy(bc3 + 8, bc1, 8);
    dxt_block(K_DXT5, bc3, px3);
    if (px[0] != 0xFFFF0000u || px[1] != 0xFF0000FFu || px[2] != 0xFFAA0055u || px3[0] >> 24 != 255 || px3[1] >> 24 != 0) {
        fprintf(stderr, "runtime: DXT self-test failed (%08X %08X %08X %08X %08X)\n", px[0], px[1], px[2], px3[0], px3[1]);
        abort();
    }
}

/* bytes of one mip level (all slices of a volume) */
static uint32_t level_size(const FormatInfo *f, uint32_t w, uint32_t h, uint32_t d) {
    return f->bpp ? w * h * d * f->bpp : ((w + 3) / 4) * ((h + 3) / 4) * d * (f->kind == K_DXT1 ? 8 : 16);
}

uint32_t texture_guest_size(const TexDesc *t) {
    FormatInfo f;
    if (!format_info((t->format >> 8) & 0xFF, &f)) return 0;
    uint32_t w, h, d, n = levels_of(t, &f, &w, &h, &d), face = 0;
    if (!f.swizzled && f.bpp) return (t->ctrl1 >> 16) * h;
    for (uint32_t l = 0; l < n; l++) face += level_size(&f, max1(w >> l), max1(h >> l), max1(d >> l));
    return tex_kind(t->format) == TK_CUBE ? 6 * ((face + 127) & ~127u) : face; /* cube faces start 128-byte aligned */
}

int texture_decode(const TexDesc *t, TexImage *img) {
    FormatInfo f;
    memset(img, 0, sizeof *img);
    if (!format_info((t->format >> 8) & 0xFF, &f)) return 0;
    uint32_t w, h, d, n = levels_of(t, &f, &w, &h, &d);
    if (!w || !h || w > 4096 || h > 4096 || d > 512) return 0;
    img->width = w;
    img->height = h;
    img->depth = d;
    img->levels = n;
    img->faces = tex_kind(t->format) == TK_CUBE ? 6 : 1;
    img->rect = !f.swizzled && f.bpp;
    int soft = !f.bpp && ((w | h) & 3); /* DXT D3D11 cannot take: decompressed here */
    img->format = soft ? TEX_BGRA8 : f.kind == K_DXT1 ? TEX_BC1 : f.kind == K_DXT3 ? TEX_BC2 : f.kind == K_DXT5 ? TEX_BC3 : TEX_BGRA8;

    uint32_t face_size = texture_guest_size(t) / img->faces;
    for (int fc = 0; fc < img->faces; fc++) {
        const uint8_t *src = g_mem + t->addr + fc * face_size;
        for (uint32_t l = 0; l < n; l++) {
            uint32_t lw = max1(w >> l), lh = max1(h >> l), ld = max1(d >> l), s = fc * n + l;
            if (!f.bpp) { /* DXT: blocks as-is, but a volume keeps the blocks of up to 4 slices together per position */
                uint32_t bw = (lw + 3) / 4, bh = (lh + 3) / 4, bs = f.kind == K_DXT1 ? 8 : 16, group = ld < 4 ? ld : 4;
                img->pitch[s] = soft ? lw * 4 : bw * bs;
                img->slice[s] = soft ? lw * lh * 4 : bw * bh * bs;
                img->level[s] = malloc(img->slice[s] * ld);
                for (uint32_t z = 0; z < ld; z++)
                    for (uint32_t b = 0; b < bw * bh; b++) {
                        const uint8_t *blk = src + (((z / group) * bw * bh + b) * group + z % group) * bs;
                        if (!soft) {
                            memcpy(img->level[s] + z * img->slice[s] + b * bs, blk, bs);
                            continue;
                        }
                        uint32_t px[16], *dst = (uint32_t *)(img->level[s] + z * img->slice[s]);
                        dxt_block(f.kind, blk, px);
                        for (uint32_t y = 0; y < 4 && (b / bw) * 4 + y < lh; y++)
                            for (uint32_t x = 0; x < 4 && (b % bw) * 4 + x < lw; x++)
                                dst[((b / bw) * 4 + y) * lw + (b % bw) * 4 + x] = px[y * 4 + x];
                    }
                src += bw * bh * bs * ld;
                continue;
            }
            uint32_t *dst = malloc(lw * lh * ld * 4);
            img->level[s] = (uint8_t *)dst;
            img->pitch[s] = lw * 4;
            img->slice[s] = lw * lh * 4;
            if (f.kind == K_P8) {
                const uint32_t *pal = (const uint32_t *)(g_mem + t->palette);
                for (uint32_t z = 0; z < ld; z++)
                    for (uint32_t y = 0; y < lh; y++)
                        for (uint32_t x = 0; x < lw; x++) {
                            uint8_t i = src[swizzle(x, y, z, lw, lh, ld)];
                            dst[(z * lh + y) * lw + x] = i < t->palette_len ? pal[i] : 0;
                        }
            } else if (f.swizzled) {
                for (uint32_t z = 0; z < ld; z++)
                    for (uint32_t y = 0; y < lh; y++)
                        for (uint32_t x = 0; x < lw; x++)
                            dst[(z * lh + y) * lw + x] = to_bgra(f.kind, src + swizzle(x, y, z, lw, lh, ld) * f.bpp);
            } else {
                uint32_t pitch = t->ctrl1 >> 16;
                for (uint32_t y = 0; y < lh; y++)
                    for (uint32_t x = 0; x < lw; x++) dst[y * lw + x] = to_bgra(f.kind, src + y * pitch + x * f.bpp);
            }
            src += lw * lh * ld * f.bpp;
        }
    }
    return 1;
}

/* level 0 of a decoded texture as BGRA (out: width * height) */
void texture_bgra(const TexImage *img, uint32_t *out) {
    uint32_t w = img->width, h = img->height, bw = (w + 3) / 4;
    if (img->format == TEX_BGRA8) {
        for (uint32_t y = 0; y < h; y++) memcpy(out + y * w, img->level[0] + y * img->pitch[0], w * 4);
        return;
    }
    int kind = img->format == TEX_BC1 ? K_DXT1 : img->format == TEX_BC2 ? K_DXT3 : K_DXT5, bs = kind == K_DXT1 ? 8 : 16;
    for (uint32_t b = 0; b < bw * ((h + 3) / 4); b++) {
        uint32_t px[16];
        dxt_block(kind, img->level[0] + b * bs, px);
        for (uint32_t y = 0; y < 4 && (b / bw) * 4 + y < h; y++)
            for (uint32_t x = 0; x < 4 && (b % bw) * 4 + x < w; x++) out[((b / bw) * 4 + y) * w + (b % bw) * 4 + x] = px[y * 4 + x];
    }
}

void texture_free(TexImage *img) {
    for (int s = 0; s < img->faces * img->levels; s++) free(img->level[s]);
    memset(img, 0, sizeof *img);
}
