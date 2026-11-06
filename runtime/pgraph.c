/* Kelvin (NV097) 3D object: state, vertex program/constant upload, vertex assembly and primitives.
 * Every method lands in g_k; the ones with side effects are handled here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pgraph.h"

uint32_t g_k[KELVIN_SIZE];

static uint32_t g_program[136][4], g_program_load;
static float g_constants[192][4];
static uint32_t g_constant_load; /* in dwords */

static uint32_t g_prim; /* BEGIN_END argument while a primitive is open, else 0 */
static Vertex *g_verts;
static uint32_t g_nverts, g_cap_verts;
static uint32_t *g_ref; /* the primitive's elements in order, each a vertex in g_verts (an indexed one is shared) */
static uint32_t g_nref, g_cap_ref;
static float g_attr[16][4]; /* current immediate attribute values */

static uint32_t *g_inline; /* INLINE_ARRAY words of the open primitive */
static uint32_t g_ninline, g_cap_inline;

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return p;
    *cap = need * 2 > 1024 ? need * 2 : 1024;
    p = realloc(p, *cap * elem);
    if (!p) {
        fprintf(stderr, "[pgraph] out of memory\n");
        exit(6);
    }
    return p;
}

static void ref(uint32_t slot) {
    g_ref = grow(g_ref, &g_cap_ref, g_nref + 1, 4);
    g_ref[g_nref++] = slot;
}

static Vertex *new_vertex(void) {
    g_verts = grow(g_verts, &g_cap_verts, g_nverts + 1, sizeof(Vertex));
    ref(g_nverts);
    return &g_verts[g_nverts++];
}

static int g_raw; /* the open primitive's elements are vertex numbers in the arrays, which the GPU reads itself */
static uint32_t g_vmin, g_vmax; /* the lowest and highest of them */
static void decode_refs(void);

static void emit(const Vertex *v) {
    if (g_raw) decode_refs(); /* immediate or inline vertices too: all decoded */
    *new_vertex() = *v;
}

/* ---------------- vertex attribute decoding ---------------- */

/* NV097_SET_VERTEX_DATA_ARRAY_FORMAT: [3:0] type, [7:4] size, [31:8] stride */
enum { T_UB_D3D = 0, T_S1 = 1, T_F = 2, T_UB_OGL = 4, T_S32K = 5, T_CMP = 6 };

/* decode one attribute from host memory `p` into out[4]; returns bytes consumed */
static uint32_t decode_attr(uint32_t fmt, const uint8_t *p, float out[4]) {
    uint32_t type = fmt & 15, size = (fmt >> 4) & 15;
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    switch (type) {
    case T_F:
        memcpy(out, p, 4 * size);
        return 4 * size;
    case T_UB_D3D: /* D3DCOLOR: B, G, R, A in memory */
        if (size == 4) {
            out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        } else {
            for (uint32_t i = 0; i < size; i++) out[i] = p[i] / 255.0f;
        }
        return size;
    case T_UB_OGL:
        for (uint32_t i = 0; i < size; i++) out[i] = p[i] / 255.0f;
        return size;
    case T_S1:
        for (uint32_t i = 0; i < size; i++) out[i] = ((const int16_t *)p)[i] / 32767.0f;
        return 2 * size;
    case T_S32K:
        for (uint32_t i = 0; i < size; i++) out[i] = (float)((const int16_t *)p)[i];
        return 2 * size;
    case T_CMP: { /* 11:11:10 signed normalised */
        uint32_t v;
        memcpy(&v, p, 4);
        out[0] = ((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = ((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = ((int32_t)v >> 22) / 511.0f;
        return 4;
    }
    default: {
        static int warned;
        if (!warned++) fprintf(stderr, "[pgraph] vertex attribute type %u not supported\n", type);
        return size * 4;
    }
    }
}

/* The vertex arrays as the open primitive reads them, set up at its first array vertex and again if they or the
 * immediate attributes change: each enabled attribute's format and vertex 0's address, the rest from g_attr. An
 * index already seen this setup reuses its vertex: a mesh's indices name each vertex about six times. */
static struct {
    uint32_t a, fmt, addr;
} g_arr[16];
static int g_narr = -1; /* -1: to set up */
static int g_arr_n;     /* the arrays of the last setup, which decode_refs still decodes by once g_narr is -1 */
static int g_arr_raw;   /* every enabled array's type is one the vertex shader reads (shaders.c vfetch) */
static Vertex g_tmpl;
#define SEEN 16384 /* by index, direct-mapped; a collision only decodes again */
typedef struct {
    uint32_t stamp, index, slot;
} Seen;
static Seen g_seen[SEEN];
static uint32_t g_stamp;

static void arrays_setup(void) {
    g_narr = 0;
    g_arr_raw = 1;
    memcpy(g_tmpl.a, g_attr, sizeof g_tmpl.a);
    for (uint32_t a = 0; a < 16; a++) {
        uint32_t fmt = g_k[KM(0x1760) + a], off = g_k[KM(0x1720) + a];
        if (!((fmt >> 4) & 15)) continue;
        uint32_t ctx = off & 0x80000000u ? g_k[KM(0x01A0)] : g_k[KM(0x019C)]; /* VERTEX_B / VERTEX_A */
        g_arr[g_narr].a = a, g_arr[g_narr].fmt = fmt, g_arr[g_narr++].addr = nv2a_dma_addr(ctx, off & 0x7FFFFFFF);
        uint32_t type = fmt & 15;
        if (type == 3 || type > 6) g_arr_raw = 0;
    }
    g_arr_n = g_narr;
    if (!++g_stamp) memset(g_seen, 0, sizeof g_seen), g_stamp = 1;
}

/* vertex `index` of the vertex arrays (NV097_SET_VERTEX_DATA_ARRAY_OFFSET/FORMAT). A primitive whose vertices all
 * come from one set of arrays is drawn from the arrays' own bytes (only its elements are kept here); one that mixes
 * in immediate or inline vertices, or changes its arrays midway, has its vertices decoded. */
static void decoded_vertex(uint32_t index);
static void array_vertex(uint32_t index) {
    if (g_narr < 0) {
        if (g_raw && g_nref) decode_refs(); /* by the arrays they came from */
        arrays_setup();
        if (!g_arr_raw) g_raw = 0;
    }
    if (g_raw) {
        ref(index);
        g_vmin = index < g_vmin ? index : g_vmin, g_vmax = index > g_vmax ? index : g_vmax;
        return;
    }
    decoded_vertex(index);
}

/* A burst of ARRAY_ELEMENT16 (two vertex numbers a word, low first) or ARRAY_ELEMENT32 words at once, as nv2a.c's
 * run hands them over: per word and per index through method() and array_vertex they took ~40% of the GPU
 * thread's time in a race. Arrays still to set up, or a primitive being decoded, go the way of array_vertex. */
void pgraph_array_elements(const uint32_t *w, uint32_t n, int wide) {
    uint32_t i = 0;
    for (; i < n && !(g_raw && g_narr >= 0 && g_prim); i++) /* arrays to set up, or vertices decoded: one by one */
        if (wide) array_vertex(w[i]);
        else array_vertex(w[i] & 0xFFFF), array_vertex(w[i] >> 16);
    if (i == n) return;
    uint32_t count = (n - i) * (wide ? 1 : 2), lo = g_vmin, hi = g_vmax;
    g_ref = grow(g_ref, &g_cap_ref, g_nref + count, 4);
    uint32_t *r = g_ref + g_nref;
    for (; i < n; i++) {
        uint32_t a = wide ? w[i] : w[i] & 0xFFFF;
        *r++ = a, lo = a < lo ? a : lo, hi = a > hi ? a : hi;
        if (!wide) {
            uint32_t b = w[i] >> 16;
            *r++ = b, lo = b < lo ? b : lo, hi = b > hi ? b : hi;
        }
    }
    g_nref += count, g_vmin = lo, g_vmax = hi;
    g_k[KM(wide ? 0x1808 : 0x1800)] = w[n - 1];
}

/* the open primitive's elements so far, vertex numbers, become decoded vertices */
static void decode_refs(void) {
    uint32_t n = g_nref;
    g_raw = 0, g_nref = 0;
    if (!++g_stamp) memset(g_seen, 0, sizeof g_seen), g_stamp = 1;
    for (uint32_t i = 0; i < n; i++) decoded_vertex(g_ref[i]); /* writes element i after reading it */
}

static void decoded_vertex(uint32_t index) {
    Seen *s = &g_seen[index % SEEN];
    if (s->stamp == g_stamp && s->index == index) {
        ref(s->slot);
        return;
    }
    s->stamp = g_stamp, s->index = index, s->slot = g_nverts;
    Vertex *v = new_vertex();
    *v = g_tmpl;
    for (int i = 0; i < g_arr_n; i++)
        decode_attr(g_arr[i].fmt, g_mem + g_arr[i].addr + index * (g_arr[i].fmt >> 8), v->a[g_arr[i].a]);
}

/* INLINE_ARRAY: whole vertices, enabled attributes packed in order, each padded to a dword */
static void inline_vertices(void) {
    const uint8_t *p = (const uint8_t *)g_inline, *end = p + g_ninline * 4;
    while (p < end) {
        Vertex v;
        for (int a = 0; a < 16; a++) {
            uint32_t fmt = g_k[KM(0x1760) + a];
            if (!((fmt >> 4) & 15)) {
                memcpy(v.a[a], g_attr[a], sizeof v.a[a]);
                continue;
            }
            p += (decode_attr(fmt, p, v.a[a]) + 3) & ~3u;
        }
        if (p > end) break; /* partial vertex */
        emit(&v);
    }
}

/* immediate attribute write; writing the last component of attribute 0 emits a vertex */
static void attr_write(int a, int comp, float value, int last) {
    g_attr[a][comp] = value;
    g_narr = -1;
    if (a == 0 && last && g_prim) {
        Vertex v;
        memcpy(v.a, g_attr, sizeof v.a);
        emit(&v);
    }
}

/* ---------------- primitives ---------------- */

static uint32_t *g_idx;
static uint32_t g_nidx, g_cap_idx;

/* elements a, b, c of the primitive -> their vertices */
static void idx3(uint32_t a, uint32_t b, uint32_t c) { /* room made by assemble */
    g_idx[g_nidx++] = g_ref[a]; g_idx[g_nidx++] = g_ref[b]; g_idx[g_nidx++] = g_ref[c];
}
static void idx2(uint32_t a, uint32_t b) {
    g_idx[g_nidx++] = g_ref[a]; g_idx[g_nidx++] = g_ref[b];
}

/* NV097_SET_BEGIN_END primitive of n elements -> index list; returns topology (0 points, 1 lines, 2 triangles) */
static int assemble(uint32_t prim, uint32_t n) {
    g_nidx = 0;
    g_idx = grow(g_idx, &g_cap_idx, 3 * n + 3, 4); /* the most any primitive makes of n elements */
    switch (prim) {
    case 1: memcpy(g_idx, g_ref, n * 4), g_nidx = n; return 0;
    case 2: for (uint32_t i = 0; i + 1 < n; i += 2) idx2(i, i + 1); return 1;
    case 3: for (uint32_t i = 0; i < n; i++) idx2(i, (i + 1) % n); return 1;
    case 4: for (uint32_t i = 0; i + 1 < n; i++) idx2(i, i + 1); return 1;
    case 5: g_nidx = n - n % 3, memcpy(g_idx, g_ref, g_nidx * 4); return 2;
    case 6: for (uint32_t i = 0; i + 2 < n; i++) i & 1 ? idx3(i + 1, i, i + 2) : idx3(i, i + 1, i + 2); return 2;
    case 7: case 10: for (uint32_t i = 1; i + 1 < n; i++) idx3(0, i, i + 1); return 2;
    case 8: for (uint32_t i = 0; i + 3 < n; i += 4) { idx3(i, i + 1, i + 2); idx3(i, i + 2, i + 3); } return 2;
    case 9: for (uint32_t i = 0; i + 3 < n; i += 2) { idx3(i, i + 1, i + 3); idx3(i, i + 3, i + 2); } return 2;
    default: return -1;
    }
}

void pgraph_surface(Surface *s) {
    uint32_t fmt = g_k[KM(0x0208)], aa = (fmt >> 12) & 15;
    s->aa_x = aa == 1 || aa == 2 ? 2 : 1; /* CENTER_CORNER_2, SQUARE_OFFSET_4 */
    s->aa_y = aa == 2 ? 2 : 1;
    s->color_format = fmt & 15;
    s->zeta_format = (fmt >> 4) & 15;
    s->clip_x = g_k[KM(0x0200)] & 0xFFFF;
    s->clip_w = g_k[KM(0x0200)] >> 16;
    s->clip_y = g_k[KM(0x0204)] & 0xFFFF;
    s->clip_h = g_k[KM(0x0204)] >> 16;
    s->width = (s->clip_x + s->clip_w) * s->aa_x;
    s->height = (s->clip_y + s->clip_h) * s->aa_y;
    s->color_addr = s->color_format ? nv2a_dma_addr(g_k[KM(0x0194)], g_k[KM(0x0210)]) : 0;
    s->zeta_addr = s->zeta_format && g_k[KM(0x0198)] ? nv2a_dma_addr(g_k[KM(0x0198)], g_k[KM(0x0214)]) : 0;
    /* VIDEOMODE 720/1080: only Forza's post-processing is HD-aware (viewports of 1/2 and 1/4 the HD size). Menus,
     * HUD and the race itself set viewports, which D3D makes the surface clip, inside 640x480 on the HD back buffers:
     * linear surfaces that wide by their pitch, which D3D may round up (1920 x 2 AA x 4 = 15360 bytes, pitch 16384).
     * Such draws are stretched over the surface, as the 480-line widescreen mode has the TV stretch them; ui_w x ui_h
     * is the extent they were drawn in.
     * shortcut: an HD viewport other than those fractions inside the top-left 640x480 is stretched too; none seen. */
    uint32_t hd_w = pgraph_hd_width(), hd_h = hd_w * 9 / 16, w0 = s->clip_x + s->clip_w, h0 = s->clip_y + s->clip_h;
    uint32_t pitch = g_k[KM(0x020C)] & 0xFFFF, row = hd_w * s->aa_x * 4;
    s->ui_w = s->ui_h = 0;
    if (hd_w && ((fmt >> 8) & 15) == 1 && pitch >= row && pitch < row + row / 4 && w0 <= 640 && h0 <= 480 &&
        !(hd_part(s->clip_w, hd_w) && hd_part(s->clip_h, hd_h))) {
        s->ui_w = w0, s->ui_h = h0;
        s->clip_x = s->clip_x * hd_w / 640, s->clip_w = s->clip_w * hd_w / 640;
        s->clip_y = s->clip_y * hd_h / 480, s->clip_h = s->clip_h * hd_h / 480;
        s->width = w0 * hd_w / 640 * s->aa_x, s->height = h0 * hd_h / 480 * s->aa_y;
    }
}

uint32_t pgraph_hd_width(void) {
    static uint32_t w = ~0u;
    if (w == ~0u) w = cfg_video_lines() >= 1080 ? 1920 : cfg_video_lines() >= 720 ? 1280 : 0;
    return w;
}

static float f32(uint32_t v);

void pgraph_attr(const Draw *d, uint32_t vertex, int a, float out[4]) {
    if (d->verts) memcpy(out, d->verts[vertex].a[a], 16);
    else if ((d->arr[a].fmt >> 4) & 15) decode_attr(d->arr[a].fmt, g_mem + d->arr[a].addr + vertex * (d->arr[a].fmt >> 8), out);
    else memcpy(out, d->konst[a], 16);
}

extern FILE *g_nv2a_dump; /* nv2a.c: a frame being dumped (NV2A_DUMP) */
static void draw(void) {
    if (g_ninline) inline_vertices();
    if (g_raw && g_vmax - g_vmin + 1 > 4 * g_nref + 4096) decode_refs(); /* sparse: the vertices between would come too */
    if (!g_nref) return;
    int topo = assemble(g_prim, g_nref);
    if (topo < 0 || !g_nidx) {
        if (topo < 0) fprintf(stderr, "[pgraph] primitive %u not supported\n", g_prim);
        return;
    }
    Draw d = {0};
    if (g_raw) { /* the arrays these vertices were named in */
        for (int i = 0; i < g_arr_n; i++) d.arr[g_arr[i].a].fmt = g_arr[i].fmt, d.arr[g_arr[i].a].addr = g_arr[i].addr;
        memcpy(d.konst, g_tmpl.a, sizeof d.konst);
        d.vmin = g_vmin, d.vmax = g_vmax, d.nverts = g_vmax - g_vmin + 1;
    } else d.verts = g_verts, d.nverts = g_nverts;
    if (g_nv2a_dump) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, p[4];
        for (uint32_t i = 0; i < g_nidx; i++) {
            pgraph_attr(&d, g_idx[i], 0, p);
            x0 = p[0] < x0 ? p[0] : x0, x1 = p[0] > x1 ? p[0] : x1, y0 = p[1] < y0 ? p[1] : y0, y1 = p[1] > y1 ? p[1] : y1;
        }
        fprintf(g_nv2a_dump, "# draw prim %u verts %u x %g..%g y %g..%g\n", g_prim, d.nverts, x0, x1, y0, y1);
    }
    pgraph_surface(&d.surf);
    if (!d.surf.color_addr) return;
    d.indices = g_idx;
    d.nindices = g_nidx;
    d.topology = topo;
    if ((g_k[KM(0x1E94)] & 3) == 2) { /* NV097_SET_TRANSFORM_EXECUTION_MODE: program */
        d.program = (const uint32_t(*)[4])g_program;
        d.program_start = g_k[KM(0x1EA0)];
        d.constants = (const float(*)[4])g_constants;
    } else { /* fixed function: the shader takes NV097_SET_COMPOSITE_MATRIX's rows as c[0..3] */
        static float ff[192][4];
        for (int i = 0; i < 16; i++) ff[i / 4][i % 4] = f32(g_k[KM(0x0680) + i]);
        d.constants = (const float(*)[4])ff;
    }
    for (int t = 0; t < 4; t++) {
        TexDesc *x = &d.tex[t];
        uint32_t b = KM(0x1B00) + t * 16; /* 0x40 bytes per texture unit */
        x->ctrl0 = g_k[b + KM(0x0C)];
        x->enabled = (x->ctrl0 >> 30) & 1;
        x->format = g_k[b + KM(0x04)];
        x->addr = nv2a_dma_addr(x->format & 2 ? g_k[KM(0x0188)] : g_k[KM(0x0184)], g_k[b]); /* DMA B / A */
        x->address = g_k[b + KM(0x08)];
        x->ctrl1 = g_k[b + KM(0x10)];
        x->filter = g_k[b + KM(0x14)];
        x->rect = g_k[b + KM(0x1C)];
        x->border = g_k[b + KM(0x24)];
        /* NV097_SET_TEXTURE_PALETTE: [0] DMA (0 A, 1 B), [3:2] length (256 >> n entries), [31:6] offset */
        uint32_t pal = g_k[b + KM(0x20)];
        x->palette = nv2a_dma_addr(pal & 1 ? g_k[KM(0x0188)] : g_k[KM(0x0184)], pal & ~0x3Fu);
        x->palette_len = 256 >> ((pal >> 2) & 3);
    }
    render_draw(&d);
}

/* ---------------- methods ---------------- */

static float f32(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

void pgraph_method(uint32_t m, uint32_t arg) {
    if (m >= 0x2000) return;
    g_k[KM(m)] = arg;
    if ((m >= 0x1720 && m < 0x17A0) || m == 0x019C || m == 0x01A0) g_narr = -1; /* vertex arrays or their DMA */

    if (m >= 0x0B00 && m < 0x0B80) { /* NV097_SET_TRANSFORM_PROGRAM: 4 dwords per instruction */
        if (g_program_load < 136) g_program[g_program_load][(m >> 2) & 3] = arg;
        if ((m & 0xC) == 0xC) g_program_load++;
        return;
    }
    if (m >= 0x0B80 && m < 0x0C00) { /* NV097_SET_TRANSFORM_CONSTANT */
        if (g_constant_load < 192 * 4) g_constants[g_constant_load / 4][g_constant_load & 3] = f32(arg);
        g_constant_load++;
        return;
    }
    /* the viewport lives in transform constant memory (XF context VPSCL 0x3A / VPOFF 0x3B), where D3D's vertex
     * programs read it as c[-38]/c[-37]; nothing uploads those slots as ordinary constants */
    if (m >= 0x0AF0 && m < 0x0B00) { /* NV097_SET_VIEWPORT_SCALE */
        g_constants[58][(m - 0x0AF0) / 4] = f32(arg);
        return;
    }
    if (m >= 0x0A20 && m < 0x0A30) { /* NV097_SET_VIEWPORT_OFFSET */
        g_constants[59][(m - 0x0A20) / 4] = f32(arg);
        return;
    }
    if (m == 0x1818) { /* NV097_INLINE_ARRAY (sent non-increasing) */
        g_inline = grow(g_inline, &g_cap_inline, g_ninline + 1, 4);
        g_inline[g_ninline++] = arg;
        return;
    }
    if (m >= 0x1500 && m < 0x150C) { /* SET_VERTEX3F: position x y z (w = 1); z emits the vertex */
        int comp = (m - 0x1500) / 4;
        if (comp == 0) g_attr[0][3] = 1;
        attr_write(0, comp, f32(arg), comp == 2);
        return;
    }
    if (m >= 0x1518 && m < 0x1528) { /* SET_VERTEX4F: position x y z w; w emits the vertex */
        int comp = (m - 0x1518) / 4;
        attr_write(0, comp, f32(arg), comp == 3);
        return;
    }
    if (m >= 0x1880 && m < 0x1900) { /* VERTEX_DATA2F_M: attr * 8 + component * 4 */
        int a = (m - 0x1880) / 8, comp = (m >> 2) & 1;
        if (comp == 0) g_attr[a][2] = 0, g_attr[a][3] = 1;
        attr_write(a, comp, f32(arg), comp == 1);
        return;
    }
    if (m >= 0x1A00 && m < 0x1B00) { /* VERTEX_DATA4F_M: attr * 16 + component * 4 */
        int a = (m - 0x1A00) / 16, comp = (m >> 2) & 3;
        attr_write(a, comp, f32(arg), comp == 3);
        return;
    }
    if (m >= 0x1940 && m < 0x1980) { /* VERTEX_DATA4UB: one dword per attribute */
        int a = (m - 0x1940) / 4;
        for (int c = 0; c < 4; c++) attr_write(a, c, ((arg >> (8 * c)) & 0xFF) / 255.0f, c == 3);
        return;
    }
    if (m >= 0x1900 && m < 0x1940) { /* VERTEX_DATA2S */
        int a = (m - 0x1900) / 4;
        g_attr[a][2] = 0, g_attr[a][3] = 1;
        attr_write(a, 0, (float)(int16_t)arg, 0);
        attr_write(a, 1, (float)(int16_t)(arg >> 16), 1);
        return;
    }
    if (m >= 0x1980 && m < 0x1A00) { /* VERTEX_DATA4S_M: attr * 8 */
        int a = (m - 0x1980) / 8, hi = (m >> 2) & 1;
        attr_write(a, hi * 2, (float)(int16_t)arg, 0);
        attr_write(a, hi * 2 + 1, (float)(int16_t)(arg >> 16), hi);
        return;
    }

    switch (m) {
    case 0x1E9C: g_program_load = arg; break;      /* SET_TRANSFORM_PROGRAM_LOAD */
    case 0x1EA4: g_constant_load = arg * 4; break; /* SET_TRANSFORM_CONSTANT_LOAD */
    case 0x17FC:                                   /* SET_BEGIN_END */
        if (arg) {
            g_prim = arg;
            g_nverts = g_nref = g_ninline = 0;
            g_narr = -1;
            g_raw = 1, g_vmin = ~0u, g_vmax = 0;
        } else if (g_prim) {
            draw();
            g_prim = 0;
            g_nverts = g_nref = g_ninline = 0;
        }
        break;
    case 0x1810: { /* DRAW_ARRAYS: start [23:0], count-1 [31:24] */
        uint32_t start = arg & 0xFFFFFF, count = (arg >> 24) + 1;
        for (uint32_t i = 0; i < count; i++) array_vertex(start + i);
        break;
    }
    case 0x1800: /* ARRAY_ELEMENT16: two indices per dword */
        array_vertex(arg & 0xFFFF);
        array_vertex(arg >> 16);
        break;
    case 0x1808: array_vertex(arg); break; /* ARRAY_ELEMENT32 */
    case 0x1D94: {                         /* CLEAR_SURFACE */
        Surface s;
        pgraph_surface(&s);
        render_clear(&s, arg, g_k[KM(0x1D90)], g_k[KM(0x1D8C)], g_k[KM(0x1D98)] & 0xFFFF, g_k[KM(0x1D9C)] & 0xFFFF,
                     g_k[KM(0x1D98)] >> 16, g_k[KM(0x1D9C)] >> 16);
        break;
    }
    }
}
