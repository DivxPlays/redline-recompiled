/* NV2A 3D (Kelvin) state shared by the push buffer interpreter, shader/texture translators and the renderer. */
#pragma once
#include <string.h>

#include "xbox.h"

/* Change detection over guest data, 8 bytes a step in four lanes. Each step is a bijection of the lane, so a
 * changed word always changes its lane. */
static inline uint64_t hash_bytes(const void *p, size_t n, uint64_t seed) {
    const uint8_t *b = p;
    uint64_t h[4] = {seed, seed ^ 1, seed ^ 2, seed ^ 3}, w;
    for (; n >= 32; n -= 32, b += 32)
        for (int i = 0; i < 4; i++) memcpy(&w, b + 8 * i, 8), h[i] = (h[i] ^ w) * 0x9E3779B97F4A7C15ull, h[i] ^= h[i] >> 32;
    uint64_t r = 0xCBF29CE484222325ull;
    for (int i = 0; i < 4; i++) r = (r ^ h[i]) * 0x100000001B3ull, r ^= r >> 29;
    for (; n; n--) r = (r ^ *b++) * 0x100000001B3ull;
    return r;
}

#define KM(m) ((m) >> 2)  /* method -> index into g_k */
#define KELVIN_SIZE 0x800 /* methods 0x0000..0x1FFC */
extern uint32_t g_k[KELVIN_SIZE]; /* last value written to each Kelvin method (subchannel 0) */

typedef struct {
    float a[16][4]; /* the 16 vertex attributes */
} Vertex;

/* one colour/depth surface pair as the NV2A sees it */
typedef struct {
    uint32_t color_addr, zeta_addr; /* guest addresses; 0 = none */
    uint32_t color_format, zeta_format;
    uint32_t width, height;         /* surface size in guest pixels, anti-aliasing included */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t aa_x, aa_y;
    uint32_t ui_w, ui_h;            /* nonzero: drawn in this front-end space, stretched over the surface */
} Surface;

/* VIDEOMODE 720/1080: the HD back buffer's width (0 at 480 lines); hd_part: v is all, half or a quarter of the HD
 * size `full`, as Forza's HD-aware post-processing sizes its viewports and targets */
uint32_t pgraph_hd_width(void);
static inline int hd_part(uint32_t v, uint32_t full) { return v == full || v == full / 2 || v == full / 4; }

typedef struct {
    int enabled;
    uint32_t addr, format, ctrl0, ctrl1, filter, address, rect, border;
    uint32_t palette; /* guest address of the 8-bit formats' ARGB palette */
    uint32_t palette_len;
} TexDesc;

typedef struct {
    uint32_t fmt, addr; /* NV097_SET_VERTEX_DATA_ARRAY_FORMAT (size 0: not an array) and the guest address */
} VArray;

typedef struct {
    Surface surf;
    const Vertex *verts; /* decoded vertices, which the indices name; NULL: the indices are vertex numbers in arr */
    uint32_t nverts;     /* decoded vertices, or vmax - vmin + 1 */
    VArray arr[16];      /* without verts: each attribute's array, else konst's value */
    float konst[16][4];
    uint32_t vmin, vmax; /* the vertex numbers the indices span */
    const uint32_t *indices;
    uint32_t nindices;
    int topology;  /* 0 points, 1 lines, 2 triangles */
    const uint32_t (*program)[4]; /* NULL: fixed-function transform */
    int program_start;
    const float (*constants)[4]; /* 192 vertex shader constants */
    TexDesc tex[4];
} Draw;

void pgraph_method(uint32_t m, uint32_t arg);
void pgraph_array_elements(const uint32_t *words, uint32_t n, int wide); /* a burst of ARRAY_ELEMENT16 (wide: 32) */
void gpu_enter(void); /* nv2a.c: the GPU lock, for the renderer and the pusher's state; nests */
void gpu_leave(void);
void pgraph_surface(Surface *s);
void pgraph_attr(const Draw *d, uint32_t vertex, int a, float out[4]); /* attribute a of vertex (as indices name it) */
uint32_t nv2a_dma_addr(uint32_t ctx, uint32_t offset);

int vsh_generate(const uint32_t (*prog)[4], int start, char *dst, size_t cap, int mv); /* mv: run it on last frame's constants too */
void vsh_position(const uint32_t (*prog)[4], int start, const float (*c)[4], const float (*v)[4], float pos[4]); /* on the CPU */
void vsh_selftest(void);
void vsh_fixed(char *dst, size_t cap, int mv);
void psh_generate(const uint32_t *k, char *dst, size_t cap, int mv); /* mv: a second target, the motion */
uint64_t vsh_key(const uint32_t (*prog)[4], int start); /* hash of exactly what the generators read */
uint64_t psh_key(const uint32_t *k);

/* how a texture unit is sampled, from its NV097_SET_TEXTURE_FORMAT: a depth format is a shadow map (compared, not
 * filtered), [2] makes a cube map, dimensionality 3 a volume */
enum { TK_2D, TK_3D, TK_CUBE, TK_SHADOW };
static inline int tex_kind(uint32_t format) {
    uint32_t color = (format >> 8) & 0xFF;
    if (color >= 0x2A && color <= 0x31) return TK_SHADOW;
    if (format & 4) return TK_CUBE;
    return ((format >> 4) & 15) == 3 ? TK_3D : TK_2D;
}

/* texture.c: NV2A texture -> host upload data */
enum { TEX_BGRA8, TEX_BC1, TEX_BC2, TEX_BC3 };
typedef struct {
    int format, levels, rect; /* rect: sampled with unnormalised coordinates */
    int faces;                /* 6 for a cube map */
    uint32_t width, height, depth;
    uint8_t *level[6 * 13];   /* D3D11 subresource order: face * levels + level */
    uint32_t pitch[6 * 13], slice[6 * 13];
} TexImage;
int texture_decode(const TexDesc *t, TexImage *img); /* 0 if the format is not supported yet */
uint32_t swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d); /* texel's index */
uint32_t texture_guest_size(const TexDesc *t);       /* bytes of guest memory the texture spans */
void texture_free(TexImage *img);
void texture_bgra(const TexImage *img, uint32_t *out); /* level 0 as BGRA, width * height */
void texture_selftest(void);

/* render.c */
void render_draw(const Draw *d);
void render_clear(const Surface *s, uint32_t flags, uint32_t argb, uint32_t zstencil, int x0, int y0, int x1, int y1);
