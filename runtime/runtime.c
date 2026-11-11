/* Host-side runtime for recompiled code: indirect dispatch and fault reporting. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox.h"

uint8_t *g_mem;

/* the lifted game code, build/forza_recomp.dll beside the exe (x86.h): its table, sorted by va, and the originals
 * (o_) the hle_*.c wrappers call */
static const FuncEntry *g_func_table;
static uint32_t g_func_count;
GuestFn o_000340C0, o_000597E0, o_00098230, o_000B0A50, o_0035B040, o_003A99B0, o_003ACF90, o_003BFCA0, o_003C2A52;

/* after g_mem is reserved: the DLL gets it, the runtime its table and originals */
void game_load(void) {
    static const struct { const char *name; GuestFn *fn; } origs[] = {
        {"o_000340C0", &o_000340C0}, {"o_000597E0", &o_000597E0}, {"o_00098230", &o_00098230},
        {"o_000B0A50", &o_000B0A50}, {"o_0035B040", &o_0035B040}, {"o_003A99B0", &o_003A99B0},
        {"o_003ACF90", &o_003ACF90}, {"o_003BFCA0", &o_003BFCA0}, {"o_003C2A52", &o_003C2A52}};
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n ? strrchr(path, '\\') : NULL;
    snprintf(slash ? slash + 1 : path, MAX_PATH - (slash ? slash + 1 - path : 0), "forza_recomp.dll");
    HMODULE dll = LoadLibraryA(path);
    if (!dll) {
        fprintf(stderr, "cannot load %s (error %lu): build the game in the launcher first\n", path, GetLastError());
        fatal_exit(1);
    }
    const uint32_t *count = (const uint32_t *)GetProcAddress(dll, "g_func_count");
    uint8_t **mem = (uint8_t **)GetProcAddress(dll, "g_mem");
    g_func_table = (const FuncEntry *)GetProcAddress(dll, "g_func_table");
    int ok = count && mem && g_func_table;
    for (size_t i = 0; ok && i < sizeof origs / sizeof origs[0]; i++) ok = (*origs[i].fn = (GuestFn)GetProcAddress(dll, origs[i].name)) != NULL;
    if (!ok) {
        fprintf(stderr, "%s is from another version of the runtime: build the game again in the launcher\n", path);
        fatal_exit(1);
    }
    g_func_count = *count;
    *mem = g_mem;
}

/* Host functions given guest addresses, for vtables the runtime builds (e.g. DirectSound streams). */
static void (*g_thunks[256])(X86Ctx *c);
static uint32_t g_nthunks;

uint32_t hle_thunk(void (*fn)(X86Ctx *c)) { /* the same address for the same function */
    for (uint32_t i = 0; i < g_nthunks; i++)
        if (g_thunks[i] == fn) return HLE_THUNK_BASE + 4 * i;
    if (g_nthunks == 256) {
        fprintf(stderr, "too many HLE thunks\n");
        abort();
    }
    g_thunks[g_nthunks] = fn;
    return HLE_THUNK_BASE + 4 * g_nthunks++;
}

void x86_call_indirect(X86Ctx *c, uint32_t target) {
    uint32_t lo = 0, hi = g_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_func_table[mid].va < target) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_func_count && g_func_table[lo].va == target) {
        g_func_table[lo].fn(c);
        return;
    }
    if (target >= KTHUNK_BASE) {
        kernel_call(c, target - KTHUNK_BASE);
        return;
    }
    if (target >= HLE_THUNK_BASE && (target - HLE_THUNK_BASE) / 4 < g_nthunks) {
        g_thunks[(target - HLE_THUNK_BASE) / 4](c);
        return;
    }
    x86_fault(c, target, "indirect branch to unknown address");
}

void x86_unimpl(X86Ctx *c, uint32_t addr, const char *text) {
    (void)c;
    fprintf(stderr, "unimplemented instruction at %08X: %s\n", addr, text);
    abort();
}

/* functions listed in runtime/trace.txt (lift.py --trace) report each call: caller, first stack arguments, ecx;
 * after 200 calls, only every 1000th, with the running count (per-frame draw calls would flood the log) */
void trace_enter(X86Ctx *c, uint32_t va) {
    static struct { uint32_t va, n; } seen[64];
    int i = 0;
    while (i < 63 && seen[i].va && seen[i].va != va) i++;
    seen[i].va = va;
    if (++seen[i].n > 200 && seen[i].n % 1000) return;
    fprintf(stderr, "[trace] %08X from %08X args %08X %08X %08X ecx %08X #%u", va, M32(c->esp), M32(c->esp + 4),
            M32(c->esp + 8), M32(c->esp + 12), c->ecx, seen[i].n);
    for (int i = 1; i <= 3; i++) { /* arguments that point at text */
        uint32_t p = M32(c->esp + 4 * i), n = 0;
        if (p < 0x10000 || p >= GUEST_RAM_END) continue;
        while (n < 80 && M8(p + n) >= 0x20 && M8(p + n) < 0x7F) n++;
        if (n >= 3 && !M8(p + n)) fprintf(stderr, " \"%.*s\"", (int)n, (const char *)(g_mem + p));
    }
    fprintf(stderr, "\n");
}

void x86_fault(X86Ctx *c, uint32_t addr, const char *why) {
    fprintf(stderr, "guest fault at %08X: %s (in function %08X, esp=%08X)\n", addr, why, c->last_fn, c->esp);
    abort();
}

double x86_ld80(uint32_t addr) {
    uint64_t mant = M64(addr);
    uint16_t se = M16(addr + 8);
    int exp = se & 0x7FFF;
    double v = exp == 0 && mant == 0 ? 0.0 : ldexp((double)mant, exp - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}

void x86_st80(uint32_t addr, double v) {
    uint16_t sign = v < 0 ? 0x8000 : 0;
    v = fabs(v);
    if (v == 0) {
        M64(addr) = 0;
        M16(addr + 8) = sign;
        return;
    }
    int e;
    double m = frexp(v, &e); /* v = m * 2^e, m in [0.5, 1) */
    M64(addr) = (uint64_t)ldexp(m, 64);
    M16(addr + 8) = sign | (uint16_t)(e - 1 + 16383);
}

/* the Xbox's 733 MHz time stamp counter: games budget work in its cycles (Forza's race loader slices are 335000) */
uint64_t x86_rdtsc(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart / f.QuadPart * 733333333u + (uint64_t)t.QuadPart % f.QuadPart * 733333333u / f.QuadPart;
}

/* MMX lane by lane, as written from the manuals: the reference for x86.h's SSE2 mmx */
static int sat(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint64_t mmx_ref(int op, uint64_t qa, uint64_t qb) {
    Mmx a = {qa}, b = {qb}, r = {0};
    uint64_t n = qb; /* shift count */
    int i;
    switch (op) {
    case MX_PADDB: for (i = 0; i < 8; i++) r.b[i] = a.b[i] + b.b[i]; break;
    case MX_PADDW: for (i = 0; i < 4; i++) r.w[i] = a.w[i] + b.w[i]; break;
    case MX_PADDD: for (i = 0; i < 2; i++) r.d[i] = a.d[i] + b.d[i]; break;
    case MX_PADDQ: r.q = a.q + b.q; break;
    case MX_PSUBB: for (i = 0; i < 8; i++) r.b[i] = a.b[i] - b.b[i]; break;
    case MX_PSUBW: for (i = 0; i < 4; i++) r.w[i] = a.w[i] - b.w[i]; break;
    case MX_PSUBD: for (i = 0; i < 2; i++) r.d[i] = a.d[i] - b.d[i]; break;
    case MX_PSUBQ: r.q = a.q - b.q; break;
    case MX_PADDSB: for (i = 0; i < 8; i++) r.sb[i] = (int8_t)sat(a.sb[i] + b.sb[i], -128, 127); break;
    case MX_PADDSW: for (i = 0; i < 4; i++) r.sw[i] = (int16_t)sat(a.sw[i] + b.sw[i], -32768, 32767); break;
    case MX_PADDUSB: for (i = 0; i < 8; i++) r.b[i] = (uint8_t)sat(a.b[i] + b.b[i], 0, 255); break;
    case MX_PADDUSW: for (i = 0; i < 4; i++) r.w[i] = (uint16_t)sat(a.w[i] + b.w[i], 0, 65535); break;
    case MX_PSUBSB: for (i = 0; i < 8; i++) r.sb[i] = (int8_t)sat(a.sb[i] - b.sb[i], -128, 127); break;
    case MX_PSUBSW: for (i = 0; i < 4; i++) r.sw[i] = (int16_t)sat(a.sw[i] - b.sw[i], -32768, 32767); break;
    case MX_PSUBUSB: for (i = 0; i < 8; i++) r.b[i] = (uint8_t)sat(a.b[i] - b.b[i], 0, 255); break;
    case MX_PSUBUSW: for (i = 0; i < 4; i++) r.w[i] = (uint16_t)sat(a.w[i] - b.w[i], 0, 65535); break;
    case MX_PMULLW: for (i = 0; i < 4; i++) r.w[i] = (uint16_t)(a.sw[i] * b.sw[i]); break;
    case MX_PMULHW: for (i = 0; i < 4; i++) r.w[i] = (uint16_t)((a.sw[i] * b.sw[i]) >> 16); break;
    case MX_PMULHUW: for (i = 0; i < 4; i++) r.w[i] = (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16); break;
    case MX_PMADDWD: for (i = 0; i < 2; i++) r.sd[i] = a.sw[2 * i] * b.sw[2 * i] + a.sw[2 * i + 1] * b.sw[2 * i + 1]; break;
    case MX_PAND: r.q = a.q & b.q; break;
    case MX_PANDN: r.q = ~a.q & b.q; break;
    case MX_POR: r.q = a.q | b.q; break;
    case MX_PXOR: r.q = a.q ^ b.q; break;
    case MX_PCMPEQB: for (i = 0; i < 8; i++) r.b[i] = a.b[i] == b.b[i] ? 0xFF : 0; break;
    case MX_PCMPEQW: for (i = 0; i < 4; i++) r.w[i] = a.w[i] == b.w[i] ? 0xFFFF : 0; break;
    case MX_PCMPEQD: for (i = 0; i < 2; i++) r.d[i] = a.d[i] == b.d[i] ? 0xFFFFFFFFu : 0; break;
    case MX_PCMPGTB: for (i = 0; i < 8; i++) r.b[i] = a.sb[i] > b.sb[i] ? 0xFF : 0; break;
    case MX_PCMPGTW: for (i = 0; i < 4; i++) r.w[i] = a.sw[i] > b.sw[i] ? 0xFFFF : 0; break;
    case MX_PCMPGTD: for (i = 0; i < 2; i++) r.d[i] = a.sd[i] > b.sd[i] ? 0xFFFFFFFFu : 0; break;
    case MX_PUNPCKLBW: for (i = 0; i < 4; i++) r.b[2 * i] = a.b[i], r.b[2 * i + 1] = b.b[i]; break;
    case MX_PUNPCKHBW: for (i = 0; i < 4; i++) r.b[2 * i] = a.b[i + 4], r.b[2 * i + 1] = b.b[i + 4]; break;
    case MX_PUNPCKLWD: for (i = 0; i < 2; i++) r.w[2 * i] = a.w[i], r.w[2 * i + 1] = b.w[i]; break;
    case MX_PUNPCKHWD: for (i = 0; i < 2; i++) r.w[2 * i] = a.w[i + 2], r.w[2 * i + 1] = b.w[i + 2]; break;
    case MX_PUNPCKLDQ: r.d[0] = a.d[0]; r.d[1] = b.d[0]; break;
    case MX_PUNPCKHDQ: r.d[0] = a.d[1]; r.d[1] = b.d[1]; break;
    case MX_PACKSSWB: for (i = 0; i < 4; i++) r.sb[i] = (int8_t)sat(a.sw[i], -128, 127), r.sb[i + 4] = (int8_t)sat(b.sw[i], -128, 127); break;
    case MX_PACKSSDW: for (i = 0; i < 2; i++) r.sw[i] = (int16_t)sat(a.sd[i], -32768, 32767), r.sw[i + 2] = (int16_t)sat(b.sd[i], -32768, 32767); break;
    case MX_PACKUSWB: for (i = 0; i < 4; i++) r.b[i] = (uint8_t)sat(a.sw[i], 0, 255), r.b[i + 4] = (uint8_t)sat(b.sw[i], 0, 255); break;
    case MX_PAVGB: for (i = 0; i < 8; i++) r.b[i] = (uint8_t)((a.b[i] + b.b[i] + 1) >> 1); break;
    case MX_PAVGW: for (i = 0; i < 4; i++) r.w[i] = (uint16_t)((a.w[i] + b.w[i] + 1) >> 1); break;
    case MX_PMINUB: for (i = 0; i < 8; i++) r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; break;
    case MX_PMAXUB: for (i = 0; i < 8; i++) r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; break;
    case MX_PMINSW: for (i = 0; i < 4; i++) r.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case MX_PMAXSW: for (i = 0; i < 4; i++) r.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case MX_PSADBW: { uint32_t s = 0; for (i = 0; i < 8; i++) s += a.b[i] > b.b[i] ? a.b[i] - b.b[i] : b.b[i] - a.b[i]; r.q = s; break; }
    case MX_PSLLW: for (i = 0; i < 4; i++) r.w[i] = n > 15 ? 0 : (uint16_t)(a.w[i] << n); break;
    case MX_PSLLD: for (i = 0; i < 2; i++) r.d[i] = n > 31 ? 0 : a.d[i] << n; break;
    case MX_PSLLQ: r.q = n > 63 ? 0 : a.q << n; break;
    case MX_PSRLW: for (i = 0; i < 4; i++) r.w[i] = n > 15 ? 0 : (uint16_t)(a.w[i] >> n); break;
    case MX_PSRLD: for (i = 0; i < 2; i++) r.d[i] = n > 31 ? 0 : a.d[i] >> n; break;
    case MX_PSRLQ: r.q = n > 63 ? 0 : a.q >> n; break;
    case MX_PSRAW: for (i = 0; i < 4; i++) r.sw[i] = (int16_t)(a.sw[i] >> (n > 15 ? 15 : n)); break;
    case MX_PSRAD: for (i = 0; i < 2; i++) r.sd[i] = a.sd[i] >> (n > 31 ? 31 : n); break;
    }
    return r.q;
}

/* known answers for the MMX helper (x86.h), and every op against the lanes above on edge and random values (shift
 * counts 0-70), run once at startup */
void mmx_selftest(void) {
    int ok = mmx(MX_PADDUSB, 0x00FF00F0FF000000ull, 0x0002000F01000000ull) == 0x00FF00FFFF000000ull &&
             mmx(MX_PUNPCKLBW, 0x0000000044332211ull, 0x00000000DDCCBBAAull) == 0xDD44CC33BB22AA11ull &&
             mmx(MX_PSRAW, 0x8000400000020001ull, 1) == 0xC000200000010000ull &&
             mmx(MX_PMADDWD, 0x0002000300040005ull, 0x0006000700080009ull) == ((uint64_t)(2 * 6 + 3 * 7) << 32 | (4 * 8 + 5 * 9)) &&
             mmx(MX_PACKUSWB, 0x0100FFFF00800010ull, 0) == 0x00000000FF008010ull;
    static const uint64_t edge[] = {0, ~0ull, 0x8000800080008000ull, 0x7FFF7FFF7FFF7FFFull, 0x80808080807F7F7Full,
                                    0x00FF00FF00FF00FFull, 0x8000000080000000ull, 0x0123456789ABCDEFull};
    uint64_t rnd = 0x9E3779B97F4A7C15ull;
    for (int op = MX_PADDB; op <= MX_PSRAD && ok; op++)
        for (int i = 0; i < 600 && ok; i++) {
            uint64_t a, b;
            if (i < 64) a = edge[i & 7], b = edge[i >> 3];
            else rnd ^= rnd << 13, rnd ^= rnd >> 7, rnd ^= rnd << 17, a = rnd, rnd ^= rnd << 13, rnd ^= rnd >> 7, rnd ^= rnd << 17, b = rnd;
            if (op >= MX_PSLLW) b = (uint64_t)(i % 71); /* shift counts */
            if (mmx(op, a, b) != mmx_ref(op, a, b)) {
                fprintf(stderr, "runtime: MMX op %d: %016llX, %016llX gives %016llX, not %016llX\n", op,
                        (unsigned long long)a, (unsigned long long)b, (unsigned long long)mmx(op, a, b),
                        (unsigned long long)mmx_ref(op, a, b));
                ok = 0;
            }
        }
    /* and rotate-through-carry (x86.h) */
    Flags f = {0};
    ok = ok && op_rcr(&f, 3, 1, 32) == 1 && f.cf == 1 && op_rcr(&f, 0, 1, 32) == 0x80000000u && f.cf == 0;
    f.cf = 0;
    ok = ok && op_rcl(&f, 0x80, 1, 8) == 0 && f.cf == 1 && op_rcl(&f, 0x5A, 9, 8) == 0x5A && f.cf == 1;
    /* and x87 classification: zero, normal, -infinity, NaN */
    static X86Ctx x;
    fxam(&x, 0.0), ok = ok && (x.fsw & 0x4700) == 0x4000;
    fxam(&x, 1.5), ok = ok && (x.fsw & 0x4700) == 0x0400;
    fxam(&x, -INFINITY), ok = ok && (x.fsw & 0x4700) == 0x0700;
    fxam(&x, NAN), ok = ok && (x.fsw & 0x4500) == 0x0100;
    if (!ok) {
        fprintf(stderr, "runtime: MMX self-test failed\n");
        abort();
    }
}

/* Port I/O (MCPX power/GPIO, SMBus, PCI config): writes are dropped, reads see an idle device. */
void x86_out(X86Ctx *c, uint32_t port, uint32_t value, int bits) {
    if (ktrace()) fprintf(stderr, "[io] out %04X <- %08X (%d-bit) in %08X\n", port, value, bits, c->last_fn);
}

uint32_t x86_in(X86Ctx *c, uint32_t port, int bits) {
    if (ktrace()) fprintf(stderr, "[io] in %04X (%d-bit) in %08X\n", port, bits, c->last_fn);
    return 0;
}

void x86_cpuid(X86Ctx *c) {
    /* report the Xbox's Pentium III (family 6, model 8) */
    switch (c->eax) {
    case 0: c->eax = 2; c->ebx = 0x756E6547; c->edx = 0x49656E69; c->ecx = 0x6C65746E; break;
    case 1: c->eax = 0x0683; c->ebx = 0; c->ecx = 0; c->edx = 0x0383F9FF; break;
    default: c->eax = c->ebx = c->ecx = c->edx = 0; break;
    }
}
