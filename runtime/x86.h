/* Guest CPU state and helpers used by recompiled code.
 *
 * Guest memory is one flat 4 GiB reservation (g_mem), so every 32-bit guest
 * address is directly addressable. Flags live in a function-local `Flags f`
 * so the C compiler drops flag computations nobody reads.
 */
#pragma once
#include <emmintrin.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef union {
    float f[4];
    uint32_t u[4];
    uint64_t q[2];
} Xmm;

typedef struct X86Ctx {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df, fs_base, mxcsr;
    uint32_t last_fn; /* guest VA of the most recently entered function, for crash reports */
    uint32_t eflags_kept; /* system EFLAGS bits (see fl_pack) */
    /* shortcut: x87 is modelled with doubles, not 80-bit; revisit if physics diverges */
    double st[8];
    uint32_t top;
    uint16_t fsw, fcw;
    Xmm xmm[8];
    uint64_t mm[8]; /* MMX; shortcut: not aliased onto the x87 registers (code must EMMS between them anyway) */
} X86Ctx;

typedef struct {
    uint8_t cf, pf, zf, sf, of;
} Flags;

typedef void (*GuestFn)(X86Ctx *c);
typedef struct {
    uint32_t va;
    GuestFn fn;
} FuncEntry;

extern uint8_t *g_mem;

void x86_call_indirect(X86Ctx *c, uint32_t target);
void x86_unimpl(X86Ctx *c, uint32_t addr, const char *text);
void trace_enter(X86Ctx *c, uint32_t va); /* entry of a function lifted with --trace */
void x86_fault(X86Ctx *c, uint32_t addr, const char *why);
double x86_ld80(uint32_t addr);
void x86_st80(uint32_t addr, double v);
uint64_t x86_rdtsc(void);
void x86_out(X86Ctx *c, uint32_t port, uint32_t value, int bits);
uint32_t x86_in(X86Ctx *c, uint32_t port, int bits);
void x86_cpuid(X86Ctx *c);

/* ---- memory ---- */
#define M8(a) (*(uint8_t *)(g_mem + (uint32_t)(a)))
#define M16(a) (*(uint16_t *)(g_mem + (uint32_t)(a)))
#define M32(a) (*(uint32_t *)(g_mem + (uint32_t)(a)))
#define M64(a) (*(uint64_t *)(g_mem + (uint32_t)(a)))
#define MF32(a) (*(float *)(g_mem + (uint32_t)(a)))
#define MF64(a) (*(double *)(g_mem + (uint32_t)(a)))
#define MX(a) (*(Xmm *)(g_mem + (uint32_t)(a)))

/* Driver code (lifted with MMIO_SECTIONS) uses these: device-register addresses go to the runtime so registers
 * can have side effects (write-1-to-clear, FIFO kick-off); everything else is a plain access. */
uint32_t mmio_read(uint32_t addr, int bits);
void mmio_write(uint32_t addr, uint32_t value, int bits);
#define IS_MMIO(a) ((uint32_t)(a) - 0xFD000000u < 0x01000000u)
#define MR8(a) (IS_MMIO(a) ? (uint8_t)mmio_read((a), 8) : M8(a))
#define MR16(a) (IS_MMIO(a) ? (uint16_t)mmio_read((a), 16) : M16(a))
#define MR32(a) (IS_MMIO(a) ? mmio_read((a), 32) : M32(a))
#define MR64(a) M64(a)
#define MW_(bits, a, v)                                                     \
    do {                                                                    \
        uint32_t _a = (a), _v = (uint32_t)(v);                              \
        if (IS_MMIO(_a)) mmio_write(_a, _v, bits);                          \
        else M##bits(_a) = (uint##bits##_t)_v;                              \
    } while (0)
#define MW8(a, v) MW_(8, a, v)
#define MW16(a, v) MW_(16, a, v)
#define MW32(a, v) MW_(32, a, v)
#define MW64(a, v) (M64(a) = (v))

/* Emitted on backward branches: if another thread wants the guest lock, hand it over (thread.c). Keeps tight
 * guest spin loops from starving the thread they are waiting for. */
extern volatile long g_gil_waiters;
void gil_yield(void);
#define SAFEPOINT()                     \
    do {                                \
        if (g_gil_waiters) gil_yield(); \
    } while (0)

#define PUSH32(v)               \
    do {                        \
        uint32_t _v = (v);      \
        c->esp -= 4;            \
        M32(c->esp) = _v;       \
    } while (0)

/* ---- flags ---- */
#define MSK(n) ((n) == 32 ? 0xFFFFFFFFu : ((1u << (n)) - 1))
#define SGN(x, n) ((uint8_t)(((x) >> ((n) - 1)) & 1))
#define PF8(r) ((uint8_t)(((0x6996u >> (((r) ^ ((r) >> 4)) & 0xF)) & 1) ^ 1))

static inline void fl_res(Flags *f, uint32_t r, int n) {
    r &= MSK(n);
    f->zf = r == 0;
    f->sf = SGN(r, n);
    f->pf = PF8(r & 0xFF);
}
static inline void fl_logic(Flags *f, uint32_t r, int n) {
    fl_res(f, r, n);
    f->cf = f->of = 0;
}
static inline void fl_add(Flags *f, uint32_t a, uint32_t b, uint32_t cin, uint32_t r, int n) {
    fl_res(f, r, n);
    f->cf = (uint64_t)(a & MSK(n)) + (b & MSK(n)) + cin > MSK(n);
    f->of = SGN((a ^ r) & (b ^ r), n);
}
static inline void fl_sub(Flags *f, uint32_t a, uint32_t b, uint32_t bin, uint32_t r, int n) {
    fl_res(f, r, n);
    f->cf = (uint64_t)(a & MSK(n)) < (uint64_t)(b & MSK(n)) + bin;
    f->of = SGN((a ^ b) & (a ^ r), n);
}
static inline void fl_inc(Flags *f, uint32_t a, uint32_t r, int n) { /* CF untouched */
    fl_res(f, r, n);
    f->of = SGN(~a & r, n);
}
static inline void fl_dec(Flags *f, uint32_t a, uint32_t r, int n) {
    fl_res(f, r, n);
    f->of = SGN(a & ~r, n);
}

/* EFLAGS bits 18-21 (AC, VIF, VIP, ID) are kept as written: CPUID detection toggles ID */
#define EFLAGS_KEPT 0x003C0000u
static inline uint32_t fl_pack(const Flags *f, const X86Ctx *c) {
    return f->cf | 2u | (f->pf << 2) | (f->zf << 6) | (f->sf << 7) | (c->df << 10) | (f->of << 11) |
           (c->eflags_kept & EFLAGS_KEPT);
}
static inline void fl_unpack(Flags *f, X86Ctx *c, uint32_t v) {
    f->cf = v & 1; f->pf = (v >> 2) & 1; f->zf = (v >> 6) & 1;
    f->sf = (v >> 7) & 1; c->df = (v >> 10) & 1; f->of = (v >> 11) & 1;
    c->eflags_kept = v & EFLAGS_KEPT;
}

/* ---- shifts ---- */
static inline uint32_t op_shl(Flags *f, uint32_t a, uint32_t cnt, int n) {
    uint32_t r = a << cnt;
    fl_res(f, r, n);
    f->cf = cnt <= (uint32_t)n ? (uint8_t)((a >> (n - cnt)) & 1) : 0;
    f->of = SGN(r, n) ^ f->cf;
    return r;
}
static inline uint32_t op_shr(Flags *f, uint32_t a, uint32_t cnt, int n) {
    a &= MSK(n);
    uint32_t r = a >> cnt;
    fl_res(f, r, n);
    f->cf = (a >> (cnt - 1)) & 1;
    f->of = SGN(a, n);
    return r;
}
static inline uint32_t op_sar(Flags *f, uint32_t a, uint32_t cnt, int n) {
    int32_t s = (int32_t)(a << (32 - n)) >> (32 - n);
    uint32_t r = (uint32_t)(s >> cnt);
    fl_res(f, r, n);
    f->cf = (s >> (cnt - 1)) & 1;
    f->of = 0;
    return r;
}
static inline uint32_t op_rol(Flags *f, uint32_t a, uint32_t cnt, int n) {
    a &= MSK(n); cnt %= n;
    uint32_t r = cnt ? ((a << cnt) | (a >> (n - cnt))) & MSK(n) : a;
    f->cf = r & 1;
    f->of = SGN(r, n) ^ f->cf;
    return r;
}
static inline uint32_t op_ror(Flags *f, uint32_t a, uint32_t cnt, int n) {
    a &= MSK(n); cnt %= n;
    uint32_t r = cnt ? ((a >> cnt) | (a << (n - cnt))) & MSK(n) : a;
    f->cf = SGN(r, n);
    f->of = SGN(r, n) ^ SGN(r << 1, n);
    return r;
}
/* rotate through carry: an (n+1)-bit rotate of CF:a; cnt is already masked to 5 bits */
static inline uint32_t op_rcl(Flags *f, uint32_t a, uint32_t cnt, int n) {
    a &= MSK(n);
    for (cnt %= n + 1; cnt; cnt--) {
        uint8_t out = SGN(a, n);
        a = ((a << 1) | f->cf) & MSK(n);
        f->cf = out;
    }
    f->of = SGN(a, n) ^ f->cf;
    return a;
}
static inline uint32_t op_rcr(Flags *f, uint32_t a, uint32_t cnt, int n) {
    a &= MSK(n);
    for (cnt %= n + 1; cnt; cnt--) {
        uint8_t out = a & 1;
        a = (a >> 1) | ((uint32_t)f->cf << (n - 1));
        f->cf = out;
    }
    f->of = SGN(a, n) ^ SGN(a << 1, n);
    return a;
}
static inline uint32_t op_shld(Flags *f, uint32_t d, uint32_t s, uint32_t cnt) {
    uint32_t r = (d << cnt) | (s >> (32 - cnt));
    fl_res(f, r, 32);
    f->cf = (d >> (32 - cnt)) & 1;
    return r;
}
static inline uint32_t op_shrd(Flags *f, uint32_t d, uint32_t s, uint32_t cnt) {
    uint32_t r = (d >> cnt) | (s << (32 - cnt));
    fl_res(f, r, 32);
    f->cf = (d >> (cnt - 1)) & 1;
    return r;
}

/* ---- multiply / divide ---- */
static inline void op_mul32(X86Ctx *c, Flags *f, uint32_t s) {
    uint64_t r = (uint64_t)c->eax * s;
    c->eax = (uint32_t)r; c->edx = (uint32_t)(r >> 32);
    f->cf = f->of = c->edx != 0;
}
static inline void op_imul32(X86Ctx *c, Flags *f, uint32_t s) {
    int64_t r = (int64_t)(int32_t)c->eax * (int32_t)s;
    c->eax = (uint32_t)r; c->edx = (uint32_t)((uint64_t)r >> 32);
    f->cf = f->of = r != (int32_t)r;
}
static inline uint32_t op_imul2(Flags *f, uint32_t a, uint32_t b, int n) {
    int32_t sa = (int32_t)(a << (32 - n)) >> (32 - n), sb = (int32_t)(b << (32 - n)) >> (32 - n);
    int64_t r = (int64_t)sa * sb;
    uint32_t t = (uint32_t)r & MSK(n);
    fl_res(f, t, n);
    f->cf = f->of = r != (int64_t)((int32_t)(t << (32 - n)) >> (32 - n));
    return t;
}
static inline void op_div32(X86Ctx *c, uint32_t s, uint32_t at) {
    uint64_t n = ((uint64_t)c->edx << 32) | c->eax;
    if (!s || n / s > 0xFFFFFFFFu) x86_fault(c, at, "divide error");
    c->eax = (uint32_t)(n / s); c->edx = (uint32_t)(n % s);
}
static inline void op_idiv32(X86Ctx *c, uint32_t s, uint32_t at) {
    int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax);
    if (!s || (n == INT64_MIN && (int32_t)s == -1)) x86_fault(c, at, "divide error");
    int64_t q = n / (int32_t)s;
    if (q != (int32_t)q) x86_fault(c, at, "divide overflow");
    c->eax = (uint32_t)q; c->edx = (uint32_t)(n % (int32_t)s);
}
static inline void op_div8(X86Ctx *c, uint8_t s, uint32_t at) {
    uint16_t n = (uint16_t)c->eax;
    if (!s || n / s > 0xFF) x86_fault(c, at, "divide error");
    c->eax = (c->eax & 0xFFFF0000u) | ((uint32_t)(n % s) << 8) | (n / s);
}

/* ---- string ops (element by element, so overlapping copies behave like hardware) ---- */
#define STR_STEP(sz) (c->df ? -(int32_t)(sz) : (int32_t)(sz))
static inline void str_movs(X86Ctx *c, int sz, int rep) {
    do {
        if (rep && !c->ecx) break;
        memcpy(g_mem + c->edi, g_mem + c->esi, sz);
        c->esi += STR_STEP(sz); c->edi += STR_STEP(sz);
        if (rep) c->ecx--;
    } while (rep);
}
static inline void str_stos(X86Ctx *c, int sz, int rep) {
    do {
        if (rep && !c->ecx) break;
        memcpy(g_mem + c->edi, &c->eax, sz);
        c->edi += STR_STEP(sz);
        if (rep) c->ecx--;
    } while (rep);
}
static inline void str_lods(X86Ctx *c, int sz) {
    uint32_t v = 0;
    memcpy(&v, g_mem + c->esi, sz);
    c->eax = sz == 4 ? v : (c->eax & ~MSK(sz * 8)) | v;
    c->esi += STR_STEP(sz);
}
/* rep: 0 none, 1 repe, 2 repne */
static inline void str_cmps(X86Ctx *c, Flags *f, int sz, int rep, int scas) {
    do {
        if (rep && !c->ecx) break;
        uint32_t a = 0, b = 0;
        if (scas) a = c->eax & MSK(sz * 8); else memcpy(&a, g_mem + c->esi, sz);
        memcpy(&b, g_mem + c->edi, sz);
        fl_sub(f, a, b, 0, a - b, sz * 8);
        if (!scas) c->esi += STR_STEP(sz);
        c->edi += STR_STEP(sz);
        if (rep) c->ecx--;
    } while ((rep == 1 && f->zf) || (rep == 2 && !f->zf));
}

/* ---- x87 ---- */
#define ST(i) c->st[(c->top + (i)) & 7]
static inline void fpush(X86Ctx *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline void fpop(X86Ctx *c) { c->top = (c->top + 1) & 7; }
static inline void fcom(X86Ctx *c, double a, double b) {
    c->fsw &= ~0x4500;
    if (a != a || b != b) c->fsw |= 0x4500;
    else if (a < b) c->fsw |= 0x0100;
    else if (a == b) c->fsw |= 0x4000;
}
/* FXAM: classify ST(0) into C3 C2 C0 (zero 100, normal 010, infinity 011, NaN 001, denormal 110), sign into C1.
 * shortcut: registers are never reported empty (101); x87 is modelled without tags */
static inline void fxam(X86Ctx *c, double v) {
    uint16_t cc;
    if (v != v) cc = 0x0100;
    else if (v == 0) cc = 0x4000;
    else if (isinf(v)) cc = 0x0500;
    else if (fabs(v) < 2.2250738585072014e-308) cc = 0x4400;
    else cc = 0x0400;
    c->fsw = (uint16_t)((c->fsw & ~0x4700) | cc | (signbit(v) ? 0x0200 : 0));
}

static inline void fcomi(Flags *f, double a, double b) {
    f->of = f->sf = 0;
    if (a != a || b != b) f->zf = f->pf = f->cf = 1;
    else { f->pf = 0; f->zf = a == b; f->cf = a < b; }
}
static inline uint16_t fstsw(X86Ctx *c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->top & 7) << 11)); }
static inline double fround(X86Ctx *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

/* ---- SSE ---- */
static inline void sse_comi(Flags *f, float a, float b) {
    f->of = f->sf = 0;
    if (a != a || b != b) f->zf = f->pf = f->cf = 1;
    else { f->pf = 0; f->zf = a == b; f->cf = a < b; }
}
static inline int32_t sse_cvtt(float v) {
    return (v != v || v >= 2147483648.0f || v < -2147483648.0f) ? INT32_MIN : (int32_t)v;
}
static inline int32_t sse_cvt(float v) {
    return (v != v || v >= 2147483648.0f || v < -2147483648.0f) ? INT32_MIN : (int32_t)nearbyintf(v);
}
static inline Xmm sse_shufps(Xmm d, Xmm s, int imm) {
    Xmm r;
    r.u[0] = d.u[imm & 3]; r.u[1] = d.u[(imm >> 2) & 3];
    r.u[2] = s.u[(imm >> 4) & 3]; r.u[3] = s.u[(imm >> 6) & 3];
    return r;
}

/* ---- MMX: one helper; the op is a constant at every call site, so the switch folds away ---- */
typedef union {
    uint64_t q;
    uint32_t d[2];
    int32_t sd[2];
    uint16_t w[4];
    int16_t sw[4];
    uint8_t b[8];
    int8_t sb[8];
} Mmx;
enum {
    MX_PADDB, MX_PADDW, MX_PADDD, MX_PADDQ, MX_PSUBB, MX_PSUBW, MX_PSUBD, MX_PSUBQ,
    MX_PADDSB, MX_PADDSW, MX_PADDUSB, MX_PADDUSW, MX_PSUBSB, MX_PSUBSW, MX_PSUBUSB, MX_PSUBUSW,
    MX_PMULLW, MX_PMULHW, MX_PMULHUW, MX_PMADDWD, MX_PAND, MX_PANDN, MX_POR, MX_PXOR,
    MX_PCMPEQB, MX_PCMPEQW, MX_PCMPEQD, MX_PCMPGTB, MX_PCMPGTW, MX_PCMPGTD,
    MX_PUNPCKLBW, MX_PUNPCKLWD, MX_PUNPCKLDQ, MX_PUNPCKHBW, MX_PUNPCKHWD, MX_PUNPCKHDQ,
    MX_PACKSSWB, MX_PACKSSDW, MX_PACKUSWB, MX_PAVGB, MX_PAVGW, MX_PMINUB, MX_PMAXUB, MX_PMINSW, MX_PMAXSW,
    MX_PSADBW, MX_PSLLW, MX_PSLLD, MX_PSLLQ, MX_PSRLW, MX_PSRLD, MX_PSRLQ, MX_PSRAW, MX_PSRAD,
};
/* each op as its SSE2 instruction on the low quadword (the scalar lanes this replaced are runtime.c's mmx_ref, which
 * mmx_selftest compares every op with); SSE2's shifts take a 64-bit count and clear (or fill) past the lane as MMX's */
static inline uint64_t mmx(int op, uint64_t qa, uint64_t qb) {
    __m128i a = _mm_cvtsi64_si128((long long)qa), b = _mm_cvtsi64_si128((long long)qb), r;
    switch (op) {
    case MX_PADDB: r = _mm_add_epi8(a, b); break;
    case MX_PADDW: r = _mm_add_epi16(a, b); break;
    case MX_PADDD: r = _mm_add_epi32(a, b); break;
    case MX_PADDQ: r = _mm_add_epi64(a, b); break;
    case MX_PSUBB: r = _mm_sub_epi8(a, b); break;
    case MX_PSUBW: r = _mm_sub_epi16(a, b); break;
    case MX_PSUBD: r = _mm_sub_epi32(a, b); break;
    case MX_PSUBQ: r = _mm_sub_epi64(a, b); break;
    case MX_PADDSB: r = _mm_adds_epi8(a, b); break;
    case MX_PADDSW: r = _mm_adds_epi16(a, b); break;
    case MX_PADDUSB: r = _mm_adds_epu8(a, b); break;
    case MX_PADDUSW: r = _mm_adds_epu16(a, b); break;
    case MX_PSUBSB: r = _mm_subs_epi8(a, b); break;
    case MX_PSUBSW: r = _mm_subs_epi16(a, b); break;
    case MX_PSUBUSB: r = _mm_subs_epu8(a, b); break;
    case MX_PSUBUSW: r = _mm_subs_epu16(a, b); break;
    case MX_PMULLW: r = _mm_mullo_epi16(a, b); break;
    case MX_PMULHW: r = _mm_mulhi_epi16(a, b); break;
    case MX_PMULHUW: r = _mm_mulhi_epu16(a, b); break;
    case MX_PMADDWD: r = _mm_madd_epi16(a, b); break;
    case MX_PAND: r = _mm_and_si128(a, b); break;
    case MX_PANDN: r = _mm_andnot_si128(a, b); break;
    case MX_POR: r = _mm_or_si128(a, b); break;
    case MX_PXOR: r = _mm_xor_si128(a, b); break;
    case MX_PCMPEQB: r = _mm_cmpeq_epi8(a, b); break;
    case MX_PCMPEQW: r = _mm_cmpeq_epi16(a, b); break;
    case MX_PCMPEQD: r = _mm_cmpeq_epi32(a, b); break;
    case MX_PCMPGTB: r = _mm_cmpgt_epi8(a, b); break;
    case MX_PCMPGTW: r = _mm_cmpgt_epi16(a, b); break;
    case MX_PCMPGTD: r = _mm_cmpgt_epi32(a, b); break;
    case MX_PUNPCKLBW: r = _mm_unpacklo_epi8(a, b); break;
    case MX_PUNPCKLWD: r = _mm_unpacklo_epi16(a, b); break;
    case MX_PUNPCKLDQ: r = _mm_unpacklo_epi32(a, b); break;
    case MX_PUNPCKHBW: r = _mm_srli_si128(_mm_unpacklo_epi8(a, b), 8); break; /* MMX's high half */
    case MX_PUNPCKHWD: r = _mm_srli_si128(_mm_unpacklo_epi16(a, b), 8); break;
    case MX_PUNPCKHDQ: r = _mm_srli_si128(_mm_unpacklo_epi32(a, b), 8); break;
    case MX_PACKSSWB: r = _mm_packs_epi16(_mm_unpacklo_epi64(a, b), a); break; /* a's lanes then b's */
    case MX_PACKSSDW: r = _mm_packs_epi32(_mm_unpacklo_epi64(a, b), a); break;
    case MX_PACKUSWB: r = _mm_packus_epi16(_mm_unpacklo_epi64(a, b), a); break;
    case MX_PAVGB: r = _mm_avg_epu8(a, b); break;
    case MX_PAVGW: r = _mm_avg_epu16(a, b); break;
    case MX_PMINUB: r = _mm_min_epu8(a, b); break;
    case MX_PMAXUB: r = _mm_max_epu8(a, b); break;
    case MX_PMINSW: r = _mm_min_epi16(a, b); break;
    case MX_PMAXSW: r = _mm_max_epi16(a, b); break;
    case MX_PSADBW: r = _mm_sad_epu8(a, b); break;
    case MX_PSLLW: r = _mm_sll_epi16(a, b); break;
    case MX_PSLLD: r = _mm_sll_epi32(a, b); break;
    case MX_PSLLQ: r = _mm_sll_epi64(a, b); break;
    case MX_PSRLW: r = _mm_srl_epi16(a, b); break;
    case MX_PSRLD: r = _mm_srl_epi32(a, b); break;
    case MX_PSRLQ: r = _mm_srl_epi64(a, b); break;
    case MX_PSRAW: r = _mm_sra_epi16(a, b); break;
    case MX_PSRAD: r = _mm_sra_epi32(a, b); break;
    default: r = _mm_setzero_si128(); break;
    }
    return (uint64_t)_mm_cvtsi128_si64(r);
}
static inline uint64_t mmx_pshufw(uint64_t qa, int imm) {
    Mmx a = {qa}, r;
    for (int i = 0; i < 4; i++) r.w[i] = a.w[(imm >> (2 * i)) & 3];
    return r.q;
}
static inline uint32_t mmx_pmovmskb(uint64_t qa) {
    Mmx a = {qa};
    uint32_t m = 0;
    for (int i = 0; i < 8; i++) m |= (uint32_t)(a.b[i] >> 7) << i;
    return m;
}

/* ---- conditions ---- */
#define CC_O (f.of)
#define CC_NO (!f.of)
#define CC_B (f.cf)
#define CC_AE (!f.cf)
#define CC_E (f.zf)
#define CC_NE (!f.zf)
#define CC_BE (f.cf || f.zf)
#define CC_A (!f.cf && !f.zf)
#define CC_S (f.sf)
#define CC_NS (!f.sf)
#define CC_P (f.pf)
#define CC_NP (!f.pf)
#define CC_L (f.sf != f.of)
#define CC_GE (f.sf == f.of)
#define CC_LE (f.zf || f.sf != f.of)
#define CC_G (!f.zf && f.sf == f.of)
