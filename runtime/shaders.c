/* HLSL generation for the NV2A programmable stages.
 *
 * Vertex programs: NV2A transform microcode (136 x 128-bit instructions) -> an HLSL vertex shader. The program
 * leaves oPos in screen space (D3D appends the viewport transform), so the epilogue maps it to clip space.
 * Pixels: the texture shader stages and register combiners (Kelvin state) -> an HLSL pixel shader.
 * Field layouts follow the NV2A documentation as reconstructed by the xemu project.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "pgraph.h"

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void out(Str *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) s->len += (size_t)n < s->cap - s->len ? (size_t)n : s->cap - s->len - 1;
}

/* ---------------- shared declarations ---------------- */

static const char *g_common =
    "cbuffer K : register(b0) {\n"
    "  float4 c[192];\n"
    "  float4 surf;          /* surface width, height (guest pixels), z scale, w-buffered (1) */\n"
    "  float4 texscale[4];   /* xy: 1/size for rect (unnormalised) textures, else 1; z: the render scale */\n"
    "  float4 factor0[8], factor1[8];\n"
    "  float4 fogcolor, alpharef, final0, final1; /* alpharef: x the reference; w-buffered: y, z the polygon offset's\n"
    "                                              slope factor (per host pixel) and bias (depth units) */\n"
    "  float4 ui;            /* ULTRAWIDE's 2D: screen-space positions (w 1; z > 0: all) are squeezed across by x about y */\n"
    "  uint4 va[16];         /* vertex attributes: x NV2A type | size << 4 (size 0: vk's value), y stride, z vertex 0's\n"
    "                          byte in vb (signed) */\n"
    "  float4 vk[16];\n"
    "};\n"
    "struct VOut {\n"
    "#ifndef POS_MODE\n"
    "#define POS_MODE\n"
    "#endif\n"
    "  POS_MODE float4 pos : SV_Position;\n"
    "  float4 d0 : COLOR0; float4 d1 : COLOR1; float4 b0 : COLOR2; float4 b1 : COLOR3;\n"
    "  float4 fog : FOG;\n"
    "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "  float wd : TEXCOORD5; /* the depth, w-buffered: interpolated perspective-correct, as the NV2A does */\n"
    "#ifdef MV\n"
    "  float4 mv0 : TEXCOORD6; float4 mv1 : TEXCOORD7; /* clip x, y, w now and a frame ago, unjittered */\n"
    "#endif\n"
    "};\n"
    "#ifdef MV\n"
    "cbuffer P : register(b1) {\n"
    "  float4 cp[192];       /* the constants this draw had a frame ago */\n"
    "  float4 jitter;        /* xy: the camera's sub-pixel offset this frame, in clip units */\n"
    "};\n"
    "#endif\n";

/* ---------------- vertex programs ---------------- */

/* The vertex arrays' own bytes (render.c vertex_upload): each attribute decoded as pgraph.c decode_attr does, so the
 * CPU only passes the elements on. vld reads a dword at any byte. */
static const char *g_vsh_fetch =
    "ByteAddressBuffer vb : register(t0);\n"
    "uint vld(uint at) { uint s = (at & 3) * 8, w = vb.Load(at & ~3u);\n"
    "  return s ? (w >> s) | (vb.Load((at & ~3u) + 4) << (32 - s)) : w; }\n"
    "float4 vfetch(uint a, uint n) {\n"
    "  uint t = va[a].x & 15, size = (va[a].x >> 4) & 15, at = (uint)((int)va[a].z + (int)(n * va[a].y));\n"
    "  if (!size) return vk[a];\n"
    "  float4 r = float4(0, 0, 0, 1);\n"
    "  [branch] if (t == 2) { [unroll] for (uint i = 0; i < 4; i++) if (i < size) r[i] = asfloat(vld(at + 4 * i)); }\n"
    "  else if (t == 1 || t == 5) { [unroll] for (uint i = 0; i < 4; i++) if (i < size) {\n"
    "    float h = (float)((int)(vld(at + 2 * i) << 16) >> 16); r[i] = t == 1 ? h / 32767 : h; } }\n"
    "  else if (t == 6) { uint w = vld(at); r.xyz = float3(int3(w << 21, w << 10, w) >> int3(21, 21, 22)) / float3(1023, 1023, 511); }\n"
    "  else { uint w = vld(at); [unroll] for (uint i = 0; i < 4; i++) if (i < size) r[i] = ((w >> (8 * i)) & 255) / 255.0;\n"
    "    if (t == 0 && size == 4) r.xyz = r.zyx; } /* D3DCOLOR: B, G, R, A */\n"
    "  return r;\n}\n";

/* oPos is in screen space (post-divide x/y/z, w kept for perspective): map to clip space. Depth is z, or with
 * w-buffering (NV097_SET_CONTROL0 bit 16, the race scene's draws) w, which D3D scales so the far plane is the depth
 * buffer's maximum: linear in distance, as fine far away as near (z/w in 24 bits steps ~15 cm at 500 m, so decals
 * and shadows flickered on the road). With MV (the scene's draws while a temporal upscaler runs) the position a frame
 * ago comes from the program run on last frame's constants, both pass to the pixel shader unjittered, and the drawn
 * position gets the camera jitter */
static const char *g_vsh_epilogue =
    "  VOut vo;\n"
    "  float w = o[0].w; if (w == 0 || isinf(w)) w = 1;\n"
    "  if (ui.x != 1 && (w == 1 || ui.z > 0)) o[0].x = ui.y + (o[0].x - ui.y) * ui.x;\n"
    "  float2 ndc = float2(o[0].x / surf.x * 2 - 1, 1 - o[0].y / surf.y * 2);\n"
    "  float zd = (surf.w > 0 ? w : o[0].z) * surf.z;\n"
    "  vo.pos = float4(ndc * w, saturate(zd) * w, w); vo.wd = zd;\n"
    "  vo.d0 = saturate(o[3]); vo.d1 = saturate(o[4]); vo.b0 = saturate(o[7]); vo.b1 = saturate(o[8]);\n"
    "  vo.fog = o[5]; vo.t0 = o[9]; vo.t1 = o[10]; vo.t2 = o[11]; vo.t3 = o[12];\n"
    "#ifdef MV\n"
    "  { float4 q = prev_pos(v); float pw = q.w; if (pw == 0 || isinf(pw)) pw = 1;\n"
    "    vo.mv0 = float4(vo.pos.xy, w, 0);\n"
    "    vo.mv1 = float4(float2(q.x / surf.x * 2 - 1, 1 - q.y / surf.y * 2) * pw, pw, 0);\n"
    "    vo.pos.xy += jitter.xy * w; }\n"
    "#endif\n"
    "  return vo;\n}\n";

/* bit field (dword index, lowest bit, width) of a 4-dword NV2A vertex instruction */
static uint32_t fld(const uint32_t *in, int word, int lo, int width) { return (in[word] >> lo) & ((1u << width) - 1); }

enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };

static void operand(Str *s, const uint32_t *in, int which) {
    static const char swz[] = "xyzw";
    uint32_t mux, reg, neg, sx, sy, sz, sw;
    if (which == 0) { /* A */
        neg = fld(in, 1, 8, 1); sx = fld(in, 1, 6, 2); sy = fld(in, 1, 4, 2); sz = fld(in, 1, 2, 2); sw = fld(in, 1, 0, 2);
        reg = fld(in, 2, 28, 4); mux = fld(in, 2, 26, 2);
    } else if (which == 1) { /* B */
        neg = fld(in, 2, 25, 1); sx = fld(in, 2, 23, 2); sy = fld(in, 2, 21, 2); sz = fld(in, 2, 19, 2); sw = fld(in, 2, 17, 2);
        reg = fld(in, 2, 13, 4); mux = fld(in, 2, 11, 2);
    } else { /* C */
        neg = fld(in, 2, 10, 1); sx = fld(in, 2, 8, 2); sy = fld(in, 2, 6, 2); sz = fld(in, 2, 4, 2); sw = fld(in, 2, 2, 2);
        reg = fld(in, 2, 0, 2) << 2 | fld(in, 3, 30, 2); mux = fld(in, 3, 28, 2);
    }
    out(s, "%s", neg ? "-" : "");
    if (mux == MUX_R) out(s, reg == 12 ? "o[0]" : "r[%u]", reg); /* R12 reads back oPos */
    else if (mux == MUX_V) out(s, "v[%u]", fld(in, 1, 9, 4)); /* inputs use the instruction's one V index */
    else if (fld(in, 3, 1, 1)) out(s, "c[clamp(a0 + %u, 0, 191)]", fld(in, 1, 13, 8)); /* A0X: relative */
    else out(s, "c[%u]", fld(in, 1, 13, 8));
    out(s, ".%c%c%c%c", swz[sx], swz[sy], swz[sz], swz[sw]);
}

static void mask(Str *s, uint32_t m) {
    out(s, ".%s%s%s%s", m & 8 ? "x" : "", m & 4 ? "y" : "", m & 2 ? "z" : "", m & 1 ? "w" : "");
}

/* NV2A output register file: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0, 8 oB1, 9..12 oT0..oT3 */
int vsh_generate(const uint32_t (*prog)[4], int start, char *dst, size_t cap, int mv) {
    static char body_buf[49152];
    Str s = {body_buf, 0, sizeof body_buf};
    int n = 0;
    for (int pc = start; pc < 136; pc++, n++) {
        const uint32_t *in = prog[pc];
        uint32_t ilu = fld(in, 1, 25, 3), macop = fld(in, 1, 21, 4);
        uint32_t mac_mask = fld(in, 3, 24, 4), out_r = fld(in, 3, 20, 4), ilu_mask = fld(in, 3, 16, 4);
        uint32_t o_mask = fld(in, 3, 12, 4), orb = fld(in, 3, 11, 1), out_addr = fld(in, 3, 3, 8);
        uint32_t out_mux = fld(in, 3, 2, 1);

        out(&s, "  /* %03d */ ", pc);
        static const char *mac_fmt[] = {NULL, "%A", "%A * %B", "%A + %C", "%A * %B + %C", "dot((%A).xyz, (%B).xyz).xxxx",
                                        "(dot((%A).xyz, (%B).xyz) + (%B).w).xxxx", "dot(%A, %B).xxxx",
                                        "float4(1, (%A).y * (%B).y, (%A).z, (%B).w)", "min(%A, %B)", "max(%A, %B)",
                                        "float4((%A) < (%B))", "float4((%A) >= (%B))", "%A"};
        if (macop && macop < 14) {
            out(&s, "mac = ");
            for (const char *p = mac_fmt[macop]; *p; p++)
                if (*p == '%') {
                    p++;
                    out(&s, "(");
                    operand(&s, in, *p == 'A' ? 0 : *p == 'B' ? 1 : 2);
                    out(&s, ")");
                } else {
                    out(&s, "%c", *p);
                }
            out(&s, "; ");
            if (macop == 13) out(&s, "a0 = (int)floor(mac.x); "); /* ARL */
        }
        if (ilu) {
            out(&s, "{ float4 cv = (");
            operand(&s, in, 2);
            out(&s, "); float x = cv.x; ");
            /* MOV and LIT take the whole vector; the others read one scalar */
            static const char *ilu_expr[] = {
                NULL, "ilu = cv;", "ilu = (1.0 / x).xxxx;",
                "{ float y = 1.0 / x; ilu = (sign(y) * clamp(abs(y), 5.42101e-20, 1.84467e19)).xxxx; }",
                "ilu = (1.0 / sqrt(abs(x))).xxxx;",
                "ilu = float4(exp2(floor(x)), frac(x), exp2(x), 1);",
                "{ float l = log2(abs(x)); ilu = float4(floor(l), abs(x) / exp2(floor(l)), l, 1); }",
                "{ float d = max(cv.x, 0), p = clamp(cv.w, -128, 128);" /* LIT */
                " ilu = float4(1, d, cv.x > 0 ? pow(max(cv.y, 0), p) : 0, 1); }"};
            out(&s, "%s } ", ilu_expr[ilu]);
        }
        /* writes after all reads: temps, then outputs */
        if (macop && macop != 13 && mac_mask) {
            out(&s, "r[%u]", out_r);
            mask(&s, mac_mask);
            out(&s, " = mac");
            mask(&s, mac_mask);
            out(&s, "; ");
        }
        if (ilu && ilu_mask) {
            uint32_t rr = macop ? 1 : out_r; /* paired with any MAC op (even one writing only outputs), ILU goes to R1 */
            out(&s, "r[%u]", rr);
            mask(&s, ilu_mask);
            out(&s, " = ilu");
            mask(&s, ilu_mask);
            out(&s, "; ");
        }
        if (o_mask) {
            if (!orb) {
                out(&s, "/* write to c[%u] not supported */ ", out_addr);
            } else if (out_addr < 13) {
                out(&s, "o[%u]", out_addr);
                mask(&s, o_mask);
                out(&s, " = %s", out_mux ? "ilu" : "mac");
                mask(&s, o_mask);
                out(&s, "; ");
            }
        }
        out(&s, "\n");
        if (fld(in, 3, 0, 1)) break; /* FINAL */
    }

    static const char *locals = "  float4 r[12] = (float4[12])0;\n  float4 o[13] = (float4[13])0;\n"
                                "  o[3] = o[4] = o[7] = o[8] = float4(0, 0, 0, 1);\n  int a0 = 0;\n  float4 mac, ilu;\n";
    Str d = {dst, 0, cap};
    out(&d, "%s%s%s", mv ? "#define MV 1\n" : "", g_common, g_vsh_fetch);
    if (mv) { /* the same program on last frame's constants: only its position */
        out(&d, "float4 prev_pos(float4 v[16]) {\n%s", locals);
        for (const char *p = body_buf; *p; p++)
            if (p[0] == 'c' && p[1] == '[' && (p == body_buf || p[-1] == ' ' || p[-1] == '(' || p[-1] == '-'))
                out(&d, "cp"), p++, out(&d, "[");
            else out(&d, "%c", *p);
        out(&d, "  return o[0];\n}\n");
    }
    /* only the attributes the program reads are fetched: all sixteen inlined took a shader ~160 ms to compile */
    out(&d, "VOut main(uint vid : SV_VertexID) {\n  float4 v[16] = (float4[16])0;\n");
    int used[16] = {0};
    for (const char *p = body_buf; (p = strstr(p, "v[")) != NULL; p += 2)
        if (p == body_buf || !(isalnum((unsigned char)p[-1]) || p[-1] == '_')) used[atoi(p + 2) & 15] = 1;
    for (int i = 0; i < 16; i++)
        if (used[i]) out(&d, "  v[%d] = vfetch(%d, vid);\n", i, i);
    out(&d, "%s%s%s", locals, body_buf, g_vsh_epilogue);
    return n;
}

/* operand A, B or C of an instruction, as operand() writes it */
static void cpu_operand(const uint32_t *in, int which, float (*r)[4], float (*o)[4], const float (*v)[4],
                        const float (*c)[4], int a0, float x[4]) {
    uint32_t mux, reg, neg, sw[4];
    if (which == 0) {
        neg = fld(in, 1, 8, 1), sw[0] = fld(in, 1, 6, 2), sw[1] = fld(in, 1, 4, 2), sw[2] = fld(in, 1, 2, 2), sw[3] = fld(in, 1, 0, 2);
        reg = fld(in, 2, 28, 4), mux = fld(in, 2, 26, 2);
    } else if (which == 1) {
        neg = fld(in, 2, 25, 1), sw[0] = fld(in, 2, 23, 2), sw[1] = fld(in, 2, 21, 2), sw[2] = fld(in, 2, 19, 2), sw[3] = fld(in, 2, 17, 2);
        reg = fld(in, 2, 13, 4), mux = fld(in, 2, 11, 2);
    } else {
        neg = fld(in, 2, 10, 1), sw[0] = fld(in, 2, 8, 2), sw[1] = fld(in, 2, 6, 2), sw[2] = fld(in, 2, 4, 2), sw[3] = fld(in, 2, 2, 2);
        reg = fld(in, 2, 0, 2) << 2 | fld(in, 3, 30, 2), mux = fld(in, 3, 28, 2);
    }
    int ci = (int)fld(in, 1, 13, 8) + (fld(in, 3, 1, 1) ? a0 : 0);
    const float *src = mux == MUX_R ? (reg == 12 ? o[0] : r[reg % 12]) : mux == MUX_V ? v[fld(in, 1, 9, 4)]
                                                                       : c[ci < 0 ? 0 : ci > 191 ? 191 : ci];
    for (int i = 0; i < 4; i++) x[i] = neg ? -src[sw[i]] : src[sw[i]];
}

/* A vertex program's oPos for one vertex, run on the CPU as vsh_generate writes it in HLSL (render.c places the
 * ULTRAWIDE race HUD by where its draws land). v: the vertex's sixteen attributes; c: the 192 constants. */
void vsh_position(const uint32_t (*prog)[4], int start, const float (*c)[4], const float (*v)[4], float pos[4]) {
    float r[12][4] = {0}, o[13][4] = {0}, A[4], B[4], C[4], mac[4] = {0}, ilu[4] = {0};
    int a0 = 0;
    for (int pc = start; pc < 136; pc++) {
        const uint32_t *in = prog[pc];
        uint32_t iop = fld(in, 1, 25, 3), macop = fld(in, 1, 21, 4);
        uint32_t mac_mask = fld(in, 3, 24, 4), out_r = fld(in, 3, 20, 4) % 12, ilu_mask = fld(in, 3, 16, 4);
        uint32_t o_mask = fld(in, 3, 12, 4), orb = fld(in, 3, 11, 1), out_addr = fld(in, 3, 3, 8), out_mux = fld(in, 3, 2, 1);
        if (macop && macop < 14) {
            cpu_operand(in, 0, r, o, v, c, a0, A), cpu_operand(in, 1, r, o, v, c, a0, B), cpu_operand(in, 2, r, o, v, c, a0, C);
            float d3 = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
            for (int i = 0; i < 4; i++)
                switch (macop) {
                case 2: mac[i] = A[i] * B[i]; break;
                case 3: mac[i] = A[i] + C[i]; break;
                case 4: mac[i] = A[i] * B[i] + C[i]; break;
                case 5: mac[i] = d3; break;
                case 6: mac[i] = d3 + B[3]; break;
                case 7: mac[i] = d3 + A[3] * B[3]; break;
                case 8: mac[i] = i == 0 ? 1 : i == 1 ? A[1] * B[1] : i == 2 ? A[2] : B[3]; break;
                case 9: mac[i] = A[i] < B[i] ? A[i] : B[i]; break;
                case 10: mac[i] = A[i] > B[i] ? A[i] : B[i]; break;
                case 11: mac[i] = A[i] < B[i]; break;
                case 12: mac[i] = A[i] >= B[i]; break;
                default: mac[i] = A[i]; /* MOV, ARL */
                }
            if (macop == 13) a0 = (int)floorf(mac[0]);
        }
        if (iop) {
            cpu_operand(in, 2, r, o, v, c, a0, C);
            float x = C[0], y, l;
            switch (iop) {
            case 1: memcpy(ilu, C, sizeof ilu); break;
            case 2: ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1 / x; break;
            case 3:
                y = 1 / x, l = fabsf(y) < 5.42101e-20f ? 5.42101e-20f : fabsf(y) > 1.84467e19f ? 1.84467e19f : fabsf(y);
                ilu[0] = ilu[1] = ilu[2] = ilu[3] = y < 0 ? -l : y > 0 ? l : 0;
                break;
            case 4: ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1 / sqrtf(fabsf(x)); break;
            case 5: ilu[0] = exp2f(floorf(x)), ilu[1] = x - floorf(x), ilu[2] = exp2f(x), ilu[3] = 1; break;
            case 6: l = log2f(fabsf(x)), ilu[0] = floorf(l), ilu[1] = fabsf(x) / exp2f(floorf(l)), ilu[2] = l, ilu[3] = 1; break;
            case 7: {
                float p = C[3] < -128 ? -128 : C[3] > 128 ? 128 : C[3];
                ilu[0] = 1, ilu[1] = C[0] > 0 ? C[0] : 0, ilu[2] = C[0] > 0 ? powf(C[1] > 0 ? C[1] : 0, p) : 0, ilu[3] = 1;
            }
            }
        }
        for (int i = 0; i < 4; i++) { /* writes after all reads, as in HLSL; mask bit 8 is x */
            int bit = 8 >> i;
            if (macop && macop != 13 && (mac_mask & bit)) r[out_r][i] = mac[i];
            if (iop && (ilu_mask & bit)) r[macop ? 1 : out_r][i] = ilu[i];
            if ((o_mask & bit) && orb && out_addr < 13) o[out_addr][i] = out_mux ? ilu[i] : mac[i];
        }
        if (fld(in, 3, 0, 1)) break; /* FINAL */
    }
    memcpy(pos, o[0], sizeof o[0]);
}

void vsh_selftest(void) { /* MAD oPos, v0.yxzw, c96, -c96: swizzle, the three muxes, negation, the output write */
    static const uint32_t prog[1][4] = {{0, 4u << 21 | 96u << 13 | (1u << 6 | 0u << 4 | 2u << 2 | 3u), /* v0.yxzw */
                                         2u << 26 | (0u << 23 | 1u << 21 | 2u << 19 | 3u << 17) | 3u << 11 | 1u << 10 |
                                             (0u << 8 | 1u << 6 | 2u << 4 | 3u << 2), /* c96, -c96 */
                                         3u << 28 | 0xFu << 12 | 1u << 11 | 1u}};
    static float c[192][4], v[16][4] = {{1, 2, 3, 4}};
    c[96][0] = 2, c[96][1] = 3, c[96][2] = 4, c[96][3] = 5;
    float p[4];
    vsh_position(prog, 0, (const float(*)[4])c, (const float(*)[4])v, p);
    if (p[0] != 2 || p[1] != 0 || p[2] != 8 || p[3] != 15) /* (2,1,3,4) * (2,3,4,5) - (2,3,4,5) */
        fprintf(stderr, "runtime: vertex program self-test failed (%g %g %g %g)\n", p[0], p[1], p[2], p[3]);
}

uint64_t vsh_key(const uint32_t (*prog)[4], int start) {
    if (start >= 136) return 0;
    int end = start;
    while (end < 135 && !fld(prog[end], 3, 0, 1)) end++; /* FINAL, as vsh_generate stops */
    return hash_bytes(prog[start], (size_t)(end + 1 - start) * 16, (uint64_t)start + 1);
}

/* Fixed-function transform: position through the composite matrix (which includes the viewport), then the
 * perspective divide the hardware does in this mode; colours and texture coordinates pass through.
 * shortcut: no lighting, texgen, texture matrices, skinning or fog; add them when a 3D fixed-function draw needs
 * them. Not yet seen in a running frame, so unverified. The caller puts the composite rows in c[0..3]. */
void vsh_fixed(char *dst, size_t cap, int mv) {
    Str s = {dst, 0, cap};
    out(&s, "%s%s%s", mv ? "#define MV 1\n" : "", g_common, g_vsh_fetch);
    if (mv)
        out(&s, "float4 prev_pos(float4 v[16]) {\n"
                "  float4 p = float4(dot(v[0], cp[0]), dot(v[0], cp[1]), dot(v[0], cp[2]), dot(v[0], cp[3]));\n"
                "  if (p.w != 0 && !isinf(p.w)) p.xyz /= p.w;\n  return p;\n}\n");
    out(&s, "VOut main(uint vid : SV_VertexID) {\n  float4 o[13] = (float4[13])0;\n  float4 v[16] = (float4[16])0;\n"
            "  v[0] = vfetch(0, vid); v[3] = vfetch(3, vid); v[4] = vfetch(4, vid);\n"
            "  v[9] = vfetch(9, vid); v[10] = vfetch(10, vid); v[11] = vfetch(11, vid); v[12] = vfetch(12, vid);\n"
            "  float4 p = float4(dot(v[0], c[0]), dot(v[0], c[1]), dot(v[0], c[2]), dot(v[0], c[3]));\n"
            "  if (p.w != 0 && !isinf(p.w)) p.xyz /= p.w;\n"
            "  o[0] = p; o[3] = o[7] = v[3]; o[4] = o[8] = v[4];\n"
            "  o[9] = v[9]; o[10] = v[10]; o[11] = v[11]; o[12] = v[12];\n");
    out(&s, "%s", g_vsh_epilogue);
}

/* ---------------- register combiners ---------------- */

/* combiner register file names; 14/15 are only valid in the final combiner */
static const char *g_reg[16] = {"zero", "c0", "c1", "fog", "v0", "v1", "zero", "zero",
                                "t0", "t1", "t2", "t3", "r0", "r1", "sum", "prod"};

/* one 8-bit combiner input: [3:0] register, [4] alpha channel, [7:5] mapping */
static void cinput(Str *s, uint32_t in, int alpha_portion, int final) {
    uint32_t reg = in & 15, use_alpha = (in >> 4) & 1, map = (in >> 5) & 7;
    char v[48];
    if (alpha_portion) snprintf(v, sizeof v, "%s.%s", g_reg[reg], use_alpha ? "a" : "b"); /* alpha: A or blue */
    else if (use_alpha) snprintf(v, sizeof v, "%s.aaa", g_reg[reg]);
    else snprintf(v, sizeof v, "%s.rgb", g_reg[reg]);
    if (final) { /* final combiner inputs only know identity and invert */
        out(s, map & 1 ? "(1 - saturate(%s))" : "saturate(%s)", v);
        return;
    }
    static const char *maps[8] = {"saturate(%s)", "(1 - saturate(%s))", "(2 * max(%s, 0) - 1)", "(-2 * max(%s, 0) + 1)",
                                  "(max(%s, 0) - 0.5)", "(-max(%s, 0) + 0.5)", "(%s)", "(-(%s))"};
    out(s, maps[map], v);
}

/* one stage half (colour or alpha). icw: inputs A..D, ocw: outputs/operation. Reads the current registers
 * and writes the n_* copies, so every input sees the values from before the stage. */
static void stage(Str *s, uint32_t icw, uint32_t ocw, int alpha) {
    const char *t = alpha ? "float" : "float3", *sw = alpha ? ".a" : ".rgb";
    uint32_t cd_dst = ocw & 15, ab_dst = (ocw >> 4) & 15, sum_dst = (ocw >> 8) & 15;
    int cd_dot = !alpha && (ocw >> 12) & 1, ab_dot = !alpha && (ocw >> 13) & 1, mux = (ocw >> 14) & 1;
    uint32_t op = (ocw >> 15) & 7;
    out(s, "  { %s A = ", t); cinput(s, icw >> 24, alpha, 0);
    out(s, ", B = ");         cinput(s, (icw >> 16) & 0xFF, alpha, 0);
    out(s, ", C = ");         cinput(s, (icw >> 8) & 0xFF, alpha, 0);
    out(s, ", D = ");         cinput(s, icw & 0xFF, alpha, 0);
    out(s, ";\n    %s ab = %s, cd = %s;\n", t, ab_dot ? "dot(A, B).xxx" : "A * B", cd_dot ? "dot(C, D).xxx" : "C * D");
    out(s, "    %s sum = %s;\n", t, mux ? "(r0.a >= 0.5 ? cd : ab)" : "ab + cd");
    /* output scale/bias: NOSHIFT, NOSHIFT_BIAS, SHIFTLEFTBY1, SHIFTLEFTBY1_BIAS, SHIFTLEFTBY2, -, SHIFTRIGHTBY1 */
    static const char *ops[8] = {"%s", "(%s - 0.5)", "(%s * 2)", "((%s - 0.5) * 2)", "(%s * 4)", "%s", "(%s * 0.5)", "%s"};
    for (int k = 0; k < 3; k++) {
        uint32_t dst = k == 0 ? ab_dst : k == 1 ? cd_dst : sum_dst;
        if (!dst || dst == 6 || dst == 7 || dst > 13) continue; /* discard */
        char expr[64];
        snprintf(expr, sizeof expr, ops[op], k == 0 ? "ab" : k == 1 ? "cd" : "sum");
        out(s, "    n_%s%s = clamp(%s, -1, 1);\n", g_reg[dst], sw, expr);
        if (!alpha && k < 2 && (ocw >> (k == 0 ? 19 : 18)) & 1) /* blue to alpha */
            out(s, "    n_%s.a = n_%s.b;\n", g_reg[dst], g_reg[dst]);
    }
    out(s, "  }\n");
}

/* every Kelvin register psh_generate reads: keep the two in step */
/* NV097_SET_CONTROL0 bit 16 (Z_PERSPECTIVE_ENABLE): depth is w */
static uint32_t w_buffered(const uint32_t *k) { return (k[KM(0x0290)] >> 16) & 1; }

uint64_t psh_key(const uint32_t *k) {
    static const uint32_t one[] = {0x1E70, 0x1E60, 0x0288, 0x028C, 0x0300, 0x033C, 0x1E6C},
                          eight[] = {0x0AC0, 0x1E40, 0x0260, 0x0AA0};
    uint32_t v[7 + 4 * 8 + 4], n = 0;
    for (int i = 0; i < 7; i++) v[n++] = k[KM(one[i])];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) v[n++] = k[KM(eight[i]) + j];
    for (int t = 0; t < 4; t++) /* texture kind and colour format, and whether the unit is enabled */
        v[n++] = (k[KM(0x1B04) + 16 * t] & 0xFFFC) | (k[KM(0x1B0C) + 16 * t] & 0x40000000);
    v[n - 1] ^= w_buffered(k) << 1; /* bit 1 is never set there */
    return hash_bytes(v, sizeof v, 0);
}

/* the view texture unit t is bound as: render.c binds a black 2D texture to a disabled unit */
static int unit_kind(const uint32_t *k, int t) {
    return (k[KM(0x1B0C) + 16 * t] >> 30) & 1 ? tex_kind(k[KM(0x1B04) + 16 * t]) : TK_2D;
}

/* GL comparison order, as NV097_SET_ALPHA_FUNC and SET_SHADOW_DEPTH_FUNC use it (NEVER .. ALWAYS) */
static const char *g_cmp[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};

/* k: raw Kelvin state indexed by method/4 */
void psh_generate(const uint32_t *k, char *dst, size_t cap, int mv) {
    Str s = {dst, 0, cap};
    int wbuf = w_buffered(k);
    /* conservative depth wants the position as noperspective centroid (the same values without multisampling) */
    out(&s, "%s%s%s", mv ? "#define MV 1\n" : "", wbuf ? "#define POS_MODE noperspective centroid\n" : "", g_common);
    static const char *decl[4] = {"Texture2D", "Texture3D", "TextureCube", "Texture2D"}; /* by TK_* */
    for (int t = 0; t < 4; t++)
        out(&s, "%s tex%d : register(t%d); SamplerState smp%d : register(s%d);\n", decl[unit_kind(k, t)], t, t, t, t);
    /* w-buffered depth, per pixel: never more than the depth rasterized from the vertices (w is convex along any
     * screen line, so the chord is above it), which keeps early depth testing */
    if (mv || wbuf)
        out(&s, "struct POut { float4 color : SV_Target0;%s%s };\nPOut main(VOut i) {\n",
            mv ? " float4 motion : SV_Target1;" : "", wbuf ? " float depth : SV_DepthLessEqual;" : "");
    else out(&s, "float4 main(VOut i) : SV_Target {\n");
    out(&s, "  float4 zero = 0, c0 = 0, c1 = 0;\n");
    if (wbuf) out(&s, "  float wslope = max(abs(ddx(i.wd)), abs(ddy(i.wd)));\n"); /* before any discard */

    /* texture shader stages: NV097_SET_SHADER_STAGE_PROGRAM, 5 bits per stage */
    uint32_t prog = k[KM(0x1E70)];
    for (int t = 0; t < 4; t++) {
        uint32_t mode = (prog >> (5 * t)) & 31, kind = unit_kind(k, t);
        if ((mode == 1 || mode == 2) && kind == TK_SHADOW) {
            /* PROJECT with a depth texture: compare the stored depth (in depth-buffer units) with r/q */
            uint32_t func = k[KM(0x1E6C)] & 7, color = (k[KM(0x1B04) + 16 * t] >> 8) & 0xFF;
            const char *max = color == 0x2C || color == 0x2D || color >= 0x30 ? "65535.0" : "16777215.0";
            /* as the NV2A's shadow buffer: each of the four nearest texels compared, the results blended by where the
             * sample falls between them. On the game's own texel grid: the shadow map is rendered RENDER_SCALE times
             * finer, and a 2x2 blend of those texels left the low sun's long car shadows hard and stepped, where the
             * Xbox's spread a shadow's edge over a whole texel of its map. (One comparison of a filtered depth, before
             * that, gave hard edges everywhere.) */
            if (func == 0 || func == 7) out(&s, "  float4 t%d = %d;\n", t, func == 7);
            else
                out(&s, "  float4 t%d; { float w = i.t%d.w == 0 ? 1 : i.t%d.w; float2 uv = i.t%d.xy / w * texscale[%d].xy;\n"
                        "    float ref = clamp(i.t%d.z / w, 0, %s); uint sw, sh; tex%d.GetDimensions(sw, sh);\n"
                        "    float s = max(texscale[%d].z, 1); float2 size = float2(sw, sh), p = uv * size / s - 0.5, g = floor(p), f = p - g;\n"
                        "    int2 a = int2(clamp(g * s + s * 0.5, 0, size - 1)), b = int2(clamp((g + 1) * s + s * 0.5, 0, size - 1));\n"
                        "    float4 d = float4(tex%d.Load(int3(a.x, b.y, 0)).r, tex%d.Load(int3(b.x, b.y, 0)).r,\n"
                        "                      tex%d.Load(int3(b.x, a.y, 0)).r, tex%d.Load(int3(a.x, a.y, 0)).r) * %s;\n"
                        "    float4 c = d %s ref;\n" /* x (0,1) y (1,1) z (1,0) w (0,0) */
                        "    t%d = dot(c, float4((1 - f.x) * f.y, f.x * f.y, f.x * (1 - f.y), (1 - f.x) * (1 - f.y))); }\n",
                    t, t, t, t, t, t, max, t, t, t, t, t, t, max, g_cmp[func], t);
        } else if ((mode == 1 || mode == 2) && kind == TK_3D) /* PROJECT3D on a volume */
            out(&s, "  float4 t%d = tex%d.Sample(smp%d, float3(i.t%d.xy * texscale[%d].xy, i.t%d.z) / (i.t%d.w == 0 ? 1 : i.t%d.w));\n",
                t, t, t, t, t, t, t, t);
        else if (mode == 3 || (mode && kind == TK_CUBE)) /* CUBE_MAP: the coordinate is a direction */
            out(&s, "  float4 t%d = tex%d.Sample(smp%d, i.t%d.xyz);\n", t, t, t, t);
        else if (mode == 1 || mode == 2) /* PROJECT2D / PROJECT3D */
            out(&s, "  float4 t%d = tex%d.Sample(smp%d, i.t%d.xy / (i.t%d.w == 0 ? 1 : i.t%d.w) * texscale[%d].xy);\n",
                t, t, t, t, t, t, t);
        else if (mode == 4) /* PASS_THROUGH: the texture coordinate itself, clamped, as the colour */
            out(&s, "  float4 t%d = saturate(i.t%d);\n", t, t);
        else
            out(&s, "  float4 t%d = 0; /* stage mode %u */\n", t, mode);
    }
    out(&s, "  float4 v0 = i.d0, v1 = i.d1, fog = float4(fogcolor.rgb, i.fog.x);\n"
            "  float4 r0 = float4(0, 0, 0, t0.a), r1 = 0;\n");

    uint32_t control = k[KM(0x1E60)];
    int stages = control & 0xFF;
    for (int i = 0; i < stages && i < 8; i++) {
        out(&s, "  c0 = factor0[%d]; c1 = factor1[%d];\n", control & (1 << 12) ? i : 0, control & (1 << 16) ? i : 0);
        out(&s, "  {\n  float4 n_v0 = v0, n_v1 = v1, n_t0 = t0, n_t1 = t1, n_t2 = t2, n_t3 = t3, n_r0 = r0, n_r1 = r1;\n"
                "  float4 n_zero = 0, n_c0 = c0, n_c1 = c1, n_fog = fog;\n");
        stage(&s, k[KM(0x0AC0) + i], k[KM(0x1E40) + i], 0);
        stage(&s, k[KM(0x0260) + i], k[KM(0x0AA0) + i], 1);
        out(&s, "  v0 = n_v0; v1 = n_v1; t0 = n_t0; t1 = n_t1; t2 = n_t2; t3 = n_t3; r0 = n_r0; r1 = n_r1;\n  }\n");
    }

    /* final combiner: out.rgb = A*B + (1-A)*C + D, out.a = G; E*F ("prod") and V1+R0 ("sum") are extra inputs;
     * its constants are NV097_SET_SPECULAR_FOG_FACTOR 0/1 */
    uint32_t cw0 = k[KM(0x0288)], cw1 = k[KM(0x028C)];
    out(&s, "  c0 = final0; c1 = final1;\n");
    out(&s, "  float4 sum = float4(saturate(%s + %s), 0);\n", cw1 & 0x40 ? "(1 - v1.rgb)" : "v1.rgb",
        cw1 & 0x20 ? "(1 - r0.rgb)" : "r0.rgb");
    out(&s, "  float4 prod = 0; { float3 E = ");
    cinput(&s, cw1 >> 24, 0, 1);
    out(&s, ", F = ");
    cinput(&s, (cw1 >> 16) & 0xFF, 0, 1);
    out(&s, "; prod.rgb = E * F; }\n  float3 A = ");
    cinput(&s, cw0 >> 24, 0, 1);
    out(&s, ", B = "); cinput(&s, (cw0 >> 16) & 0xFF, 0, 1);
    out(&s, ", C = "); cinput(&s, (cw0 >> 8) & 0xFF, 0, 1);
    out(&s, ", D = "); cinput(&s, cw0 & 0xFF, 0, 1);
    out(&s, ";\n  float G = ");
    cinput(&s, (cw1 >> 8) & 0xFF, 1, 1);
    out(&s, ";\n  float4 result = float4(saturate(A * B + (1 - A) * C + D), G);\n");

    /* alpha test: NV097_SET_ALPHA_TEST_ENABLE / ALPHA_FUNC (GL enums) / ALPHA_REF */
    if (k[KM(0x0300)]) {
        uint32_t f = (k[KM(0x033C)] - 0x200) & 7;
        if (f == 0) out(&s, "  discard;\n");
        else if (f != 7) out(&s, "  if (!(round(result.a * 255) %s alpharef.x)) discard;\n", g_cmp[f]);
    }
    if (mv || wbuf) {
        out(&s, "  POut po; po.color = result;\n");
        if (mv) out(&s, "  po.motion = float4((i.mv1.xy / i.mv1.z - i.mv0.xy / i.mv0.z) * float2(0.5, -0.5), 0, 0);\n");
        if (wbuf) out(&s, "  po.depth = min(saturate(i.wd + alpharef.z + alpharef.y * wslope), i.pos.z);\n");
        static int wshow = -1; /* WSHOW=1: w-buffered draws show their depth (w / zmax): red x256, green x16, blue x1 */
        if (wshow < 0) wshow = getenv("WSHOW") != NULL;
        if (wbuf && wshow) out(&s, "  po.color = float4(saturate(i.wd * float3(256, 16, 1)), 1);\n");
        out(&s, "  return po;\n}\n");
    } else out(&s, "  return result;\n}\n");
}
