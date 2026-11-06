/* NV2A stand-in for the lifted XDK Direct3D library.
 *
 * Registers at 0xFD000000 are backed by memory holding the values the driver expects of an idle GPU. D3D code
 * accesses them through mmio_read/mmio_write (see MR/MW in x86.h), which add the behaviour that matters:
 * interrupt status is write-1-to-clear, flushes complete instantly, a DMA_PUT write hands the push buffer to the
 * GPU thread (below), and the CRTC start address tells us which framebuffer is on screen.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "pgraph.h"

#define REG(off) M32(NV2A_BASE + (off))

uint32_t g_display_start; /* framebuffer the CRTC scans out (physical) */
static uint32_t g_flips;  /* writes to the CRTC start address = frames shown */

void nv2a_init(void) {
    REG(0x000000) = 0x02A000A1;            /* NV_PMC_BOOT_0: NV2A A1 */
    REG(0x100200) = 0x03070103;            /* NV_PFB_CFG0 */
    REG(0x10020C) = GUEST_RAM_END;         /* NV_PFB_CSTATUS: RAM size */
    REG(0x002400) = 0x00000010;            /* NV_PFIFO_RUNOUT_STATUS: empty */
    REG(0x003214) = 0x00000010;            /* NV_PFIFO_CACHE1_STATUS: empty */
    REG(0x009200) = 1;                     /* NV_PTIMER_NUMERATOR */
    REG(0x009210) = 1;                     /* NV_PTIMER_DENOMINATOR */
    nv2a_tick();
}

/* ---------------- push buffer ---------------- */

/* Push buffer addresses are offsets in a DMA object based at physical 0, i.e. guest addresses here. */
static volatile uint32_t g_get; /* next word the "GPU" reads */
static uint32_t g_return_addr;  /* for the push buffer's call/return */
static uint32_t g_method_count[0x800], g_words;

/* DMA contexts by id, as CMiniport::CreateCtxDmaObject made them */
#define MAX_DMA 32
static struct {
    uint32_t cls, base, limit;
} g_dma[MAX_DMA];
static uint32_t g_semaphore_ctx, g_semaphore_offset;

/* ---------------- the GPU thread ---------------- */

/* The push buffer runs on a thread of its own, as the NV2A runs beside the CPU (GPUTHREAD=0: inline on the thread
 * that writes DMA_PUT, as before). A DMA_PUT write records PUT and wakes it; GET, the semaphores and the beta marker
 * advance as it goes, and the game thread meanwhile gets on with the next frame: before, the game thread spent
 * ~60% of a race at FRAME RATE 144 running the push buffer itself. The thread holds the GPU lock (gpu_enter) while it
 * runs, never the guest lock; everything else that uses the renderer or the pusher's state (presenting at the vblank,
 * readbacks, the overlay, settings, Bink's copies, DMA objects, GET) takes the GPU lock, which the thread lets go
 * of between commands when someone waits. A flip or software-method stall stops it until the driver's handler
 * resumes it; g_flip_lock makes the check for a stall and the resume one before the other. */
static int g_gpu_threaded = -1;
static HANDLE g_gpu_wake; /* auto-reset: PUT moved, or a stall resumed */
static SRWLOCK g_gpu_lock = SRWLOCK_INIT, g_flip_lock = SRWLOCK_INIT;
static volatile LONG g_gpu_wanted;
static __declspec(thread) int t_gpu_depth; /* gpu_enter nests */
static void run(void);
static LONGLONG g_push_ticks; /* QPC time spent running the push buffer, for FPSLOG */

void gpu_enter(void) {
    if (t_gpu_depth++) return;
    InterlockedIncrement(&g_gpu_wanted);
    AcquireSRWLockExclusive(&g_gpu_lock);
    InterlockedDecrement(&g_gpu_wanted);
}
void gpu_leave(void) {
    if (!--t_gpu_depth) ReleaseSRWLockExclusive(&g_gpu_lock);
}
/* between commands: a thread waiting for the GPU lock gets it now */
static void gpu_safepoint(void) {
    if (!g_gpu_wanted || t_gpu_depth != 1) return;
    ReleaseSRWLockExclusive(&g_gpu_lock);
    while (g_gpu_wanted) SwitchToThread();
    AcquireSRWLockExclusive(&g_gpu_lock);
}

static DWORD WINAPI gpu_proc(void *p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_gpu_wake, INFINITE);
        LARGE_INTEGER t0, t1;
        gpu_enter();
        QueryPerformanceCounter(&t0);
        run();
        QueryPerformanceCounter(&t1);
        g_push_ticks += t1.QuadPart - t0.QuadPart;
        gpu_leave();
    }
}

static int gpu_threaded(void) {
    if (g_gpu_threaded < 0) {
        g_gpu_threaded = cfg_get("GPUTHREAD", "gputhread", 1) != 0;
        if (g_gpu_threaded) {
            g_gpu_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
            HANDLE t = g_gpu_wake ? CreateThread(NULL, 1 << 20, gpu_proc, NULL, 0, NULL) : NULL;
            if (t) CloseHandle(t);
            else g_gpu_threaded = 0;
        }
        fprintf(stderr, "[nv2a] push buffer on %s\n", g_gpu_threaded ? "a GPU thread" : "the thread kicking it");
    }
    return g_gpu_threaded;
}

/* run the push buffer from GET to PUT: wake the GPU thread, or here */
static void kick(void) {
    if (gpu_threaded()) {
        SetEvent(g_gpu_wake);
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    gpu_enter();
    run();
    gpu_leave();
    QueryPerformanceCounter(&t1);
    g_push_ticks += t1.QuadPart - t0.QuadPart;
}

void nv2a_dma_object(uint32_t id, uint32_t cls, uint32_t base, uint32_t limit) {
    if (ktrace()) fprintf(stderr, "[nv2a] DMA object %u: class %02X base %08X limit %08X\n", id, cls, base, limit);
    if (id < MAX_DMA) {
        gpu_enter();
        g_dma[id].cls = cls;
        g_dma[id].base = base & 0x0FFFFFFF; /* physical */
        g_dma[id].limit = limit;
        gpu_leave();
    }
}

/* guest address of `offset` inside DMA context `id` */
uint32_t nv2a_dma_addr(uint32_t id, uint32_t offset) {
    if (id >= MAX_DMA || !g_dma[id].cls || offset > g_dma[id].limit) {
        fprintf(stderr, "[nv2a] bad DMA access: context %u offset %08X\n", id, offset);
        exit(6);
    }
    return g_dma[id].base + offset;
}

/* Flip queue (PGRAPH_SURFACE 3D fields). FLIP_STALL holds the push buffer while the buffer being drawn is still
 * on screen; the driver's vblank handler flips the CRTC and bumps `read` through PGRAPH_INCREMENT. This is what
 * paces the game to the display. */
static uint32_t g_flip_read, g_flip_write, g_flip_modulo = 1; /* under g_flip_lock */
static uint32_t g_read_incs, g_traps; /* for nv2a_report */
static volatile int g_stalled; /* STALL_FLIP or STALL_TRAP: the push buffer waits for the driver (set under g_flip_lock) */
static volatile uint32_t g_put; /* latest DMA_PUT */
int64_t nv2a_push_ticks(void) { return g_push_ticks; }
enum { STALL_FLIP = 1, STALL_TRAP = 2 };

/* Software methods: D3D sends NO_OPERATION with a nonzero argument; the GPU traps it into PGRAPH_TRAPPED_ADDR/DATA,
 * raises a PGRAPH interrupt and holds the FIFO until the driver's handler has run it and re-enabled FIFO access. */
#define PGRAPH_INTR 0x400100
#define PGRAPH_NSOURCE 0x400108
#define PGRAPH_TRAPPED_ADDR 0x400704
#define PGRAPH_TRAPPED_DATA 0x400708
#define PGRAPH_FIFO_ACCESS 0x400720
static volatile LONG g_pgraph_intr;

static void trap_software_method(uint32_t subchannel, uint32_t m, uint32_t arg) {
    REG(PGRAPH_TRAPPED_ADDR) = m | subchannel << 13;
    REG(PGRAPH_TRAPPED_DATA) = arg;
    REG(PGRAPH_NSOURCE) = 2; /* ILLEGAL_MTHD */
    g_stalled = STALL_TRAP;  /* before the interrupt, which the handler answers by resuming */
    MemoryBarrier();
    InterlockedOr(&g_pgraph_intr, 0x00100000); /* NV_PGRAPH_INTR_ERROR */
    dpc_kick();
    if (ktrace()) fprintf(stderr, "[nv2a] software method %X arg %08X at get %08X\n", m, arg, g_get);
}

FILE *g_nv2a_dump; /* NV2A_DUMP's open frame file; pgraph adds a "# draw" line per draw */

/* returns a STALL_* reason when the push buffer must stop after this method */
static int method(uint32_t subchannel, uint32_t m, uint32_t arg) {
    g_method_count[(m >> 2) & 0x7FF]++;
    { /* NV2A_DUMP=<s>: write every method of the first whole frame after s seconds to build/frame.txt
       * (a frame runs from one FLIP_INCREMENT_WRITE to the next); creating build/dump.now does the same, any time */
        static int64_t due = -2;
        FILE *dump = g_nv2a_dump;
        if (due == -2) due = getenv("NV2A_DUMP") ? (int64_t)GetTickCount64() + 1000ll * atoi(getenv("NV2A_DUMP")) : -1;
        int flip = subchannel == 0 && m == 0x012C;
        if (dump) fprintf(dump, "%u %04X %08X\n", subchannel, m, arg);
        if (flip && dump) {
            fclose(dump);
            dump = g_nv2a_dump = NULL;
            due = -1;
        } else if (flip && ((due >= 0 && (int64_t)GetTickCount64() >= due) || DeleteFileA("build/dump.now"))) {
            dump = g_nv2a_dump = fopen("build/frame.txt", "w");
        }
    }
    if (m == 0x0100 && arg) { /* NO_OPERATION with an argument: a software method for the driver */
        g_traps++;
        trap_software_method(subchannel, m, arg);
        return STALL_TRAP;
    }
    /* subchannel 5, method 0x310 lands in PGRAPH 0x400B10 (the beta register); D3D (re)uses it as a marker of how
     * far the GPU got: pushbuffer address << 5 | GPU-time low bits << 2 | lap, polled by D3D_BlockOnTime */
    if (subchannel == 5 && m == 0x0310) REG(0x400B10) = arg;
    if (subchannel != 0) return 0; /* only the Kelvin (3D) object on subchannel 0 matters so far */
    if (m >= 0x0120 && m <= 0x0130) { /* the flip queue, which the driver's vblank handler moves on its side */
        int stall = 0;
        AcquireSRWLockExclusive(&g_flip_lock);
        switch (m) {
        case 0x0120: g_flip_read = arg; break;                                 /* NV097_SET_FLIP_READ */
        case 0x0124: g_flip_write = arg; break;                                /* NV097_SET_FLIP_WRITE */
        case 0x0128: g_flip_modulo = arg ? arg : 1; break;                     /* NV097_SET_FLIP_MODULO */
        case 0x012C: g_flip_write = (g_flip_write + 1) % g_flip_modulo; break; /* NV097_FLIP_INCREMENT_WRITE */
        case 0x0130:                                                           /* NV097_FLIP_STALL */
            if (g_flip_read == g_flip_write) g_stalled = stall = STALL_FLIP;
            break;
        }
        ReleaseSRWLockExclusive(&g_flip_lock);
        if (stall) return stall;
    }
    switch (m) {
    case 0x01A4: /* NV097_SET_CONTEXT_DMA_SEMAPHORE */
        g_semaphore_ctx = arg;
        break;
    case 0x1D6C: /* NV097_SET_SEMAPHORE_OFFSET */
        g_semaphore_offset = arg;
        break;
    case 0x1D70: /* NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: D3D's fences ("GPU time") */
        M32(nv2a_dma_addr(g_semaphore_ctx, g_semaphore_offset)) = arg;
        break;
    }
    pgraph_method(m, arg); /* 3D state, vertices, draws, clears */
    return 0;
}

/* the driver (re)positions the FIFO's read pointer */
void nv2a_pushbuffer_set_get(uint32_t get) {
    gpu_enter();
    g_get = get;
    gpu_leave();
}

/* Execute commands from GET up to PUT, which may move meanwhile (or until a stall); GET is published as it goes.
 * With the GPU lock held. */
#define USER_DMA_GET 0x800044
static void run(void) {
    if (!g_get) {
        fprintf(stderr, "[nv2a] DMA_PUT %08X before any DMA_GET was set; starting there\n", g_put);
        g_get = g_put;
    }
    uint32_t guard = 0, start = g_get, words_before = g_words;
    static int kicks;
    while (g_get != g_put && !g_stalled) {
        if (++guard > 50000000) {
            fprintf(stderr, "[nv2a] push buffer runaway at %08X (put %08X)\n", g_get, g_put);
            exit(6);
        }
        uint32_t w = M32(g_get);
        g_words++;
        if ((w & 0xE0000003) == 0x20000000) { /* old jump */
            g_get = w & 0x1FFFFFFC;
        } else if ((w & 3) == 1) { /* jump */
            g_get = w & 0xFFFFFFFC;
        } else if ((w & 3) == 2) { /* call */
            g_return_addr = g_get + 4;
            g_get = w & 0xFFFFFFFC;
        } else if (w == 0x00020000) { /* return */
            g_get = g_return_addr;
        } else if ((w & 0xE0030003) == 0 || (w & 0xE0030003) == 0x40000000) { /* (non-)increasing methods */
            uint32_t m = w & 0x1FFC, sub = (w >> 13) & 7, n = (w >> 18) & 0x7FF;
            int incr = (w & 0xE0030003) == 0;
            if (sub == 0 && !incr && (m == 0x1800 || m == 0x1808) && !g_nv2a_dump) { /* a mesh's indices: at once */
                g_method_count[m >> 2] += n;
                pgraph_array_elements((const uint32_t *)(g_mem + g_get + 4), n, m == 0x1808);
            } else
            for (uint32_t i = 0; i < n; i++) {
                int stall = method(sub, incr ? m + 4 * i : m, M32(g_get + 4 + 4 * i)); /* sets g_stalled itself */
                if (stall && i != n - 1) /* shortcut: resumes after the burst; D3D sends these methods alone */
                    fprintf(stderr, "[nv2a] stall inside a method burst at %08X\n", g_get);
            }
            g_get += 4 + 4 * n;
        } else {
            fprintf(stderr, "[nv2a] bad push buffer word %08X at %08X\n", w, g_get);
            exit(6);
        }
        REG(USER_DMA_GET) = g_get;
        gpu_safepoint();
    }
    REG(USER_DMA_GET) = g_get; /* also when there was nothing to run (the first kick sets it from PUT) */
    if (ktrace() && ++kicks <= 40)
        fprintf(stderr, "[nv2a] kick %d: %08X -> %08X, %u words\n", kicks, start, g_get, g_words - words_before);
}

uint32_t nv2a_pushbuffer_run(uint32_t put) {
    g_put = put;
    kick();
    return g_get;
}

/* PGRAPH_SURFACE: WRITE_3D [22:20], READ_3D [26:24], MODULO_3D [30:28] (other fields kept as written) */
static uint32_t flip_surface_reg(uint32_t stored) {
    return (stored & 0x88FFFFFF & ~0x00700000u) | (g_flip_write & 7) << 20 | (g_flip_read & 7) << 24 |
           (g_flip_modulo & 7) << 28;
}

/* the driver has resolved what the push buffer was waiting for */
static void resume(int reason) {
    AcquireSRWLockExclusive(&g_flip_lock);
    int go = g_stalled == reason;
    if (go) g_stalled = 0;
    ReleaseSRWLockExclusive(&g_flip_lock);
    if (go) kick();
}

/* PGRAPH_INCREMENT READ_3D: the driver has flipped the display; a flip-stalled push buffer may continue */
static void flip_read_increment(void) {
    AcquireSRWLockExclusive(&g_flip_lock);
    g_read_incs++;
    g_flip_read = (g_flip_read + 1) % g_flip_modulo;
    ReleaseSRWLockExclusive(&g_flip_lock);
    resume(STALL_FLIP);
}

static uint32_t g_vblanks;
uint32_t nv2a_flip_count(void) { return g_flips; }
uint32_t nv2a_vblank_count(void) { return g_vblanks; }

/* NV2A_STATS=1: method histogram at exit */
void nv2a_report(void) {
    fprintf(stderr, "[nv2a] %u frames flipped (last at %08X), %u push buffer words; methods:", g_flips,
            g_display_start, g_words);
    for (int i = 0; i < 0x800; i++)
        if (g_method_count[i]) fprintf(stderr, " %04X:%u", i * 4, g_method_count[i]);
    fprintf(stderr, "\n[nv2a] get %08X put %08X stalled %d (flip read %u write %u modulo %u) pgraph intr %08X\n", g_get,
            g_put, g_stalled, g_flip_read, g_flip_write, g_flip_modulo, g_pgraph_intr);
    fprintf(stderr, "[nv2a] %u software-method traps, %u flip read increments\n", g_traps, g_read_incs);
    fprintf(stderr, "[nv2a] words before get:");
    for (uint32_t a = g_get - 0x100; a < g_get + 0x20; a += 4) fprintf(stderr, "%s%08X", a % 32 ? " " : "\n  ", M32(a));
    fprintf(stderr, "\n");
}

/* ---------------- registers ---------------- */

#define PMC_INTR_0 0x000100
#define PMC_INTR_EN_0 0x000140
#define PFB_WC_CACHE 0x100410
#define PCRTC_INTR_0 0x600100
#define PCRTC_INTR_EN_0 0x600140
#define PCRTC_START 0x600800
#define USER_DMA_PUT 0x800040 /* channel 0 user area */
#define PGRAPH_SURFACE 0x400710
#define PGRAPH_INCREMENT 0x40071C

static uint32_t g_pcrtc_pending; /* NV_PCRTC_INTR_0 bit 0: vblank */

/* the CRTC latches the display start once per vblank: show that frame, then raise the interrupt */
uint32_t nv2a_display(void) { return g_display_start; }

void nv2a_vblank(void) {
    g_vblanks++;
    if (g_display_start) {
        gpu_enter();
        render_present(g_display_start & 0x0FFFFFFF);
        gpu_leave();
    }
    g_pcrtc_pending |= 1;
}

/* NV_PMC_INTR_0 summarises the units: PGRAPH (bit 12, software methods) and PCRTC (bit 24, vblank) */
static uint32_t pmc_intr(void) {
    return (g_pgraph_intr ? 1u << 12 : 0) | (g_pcrtc_pending & REG(PCRTC_INTR_EN_0) ? 1u << 24 : 0);
}

/* the interrupt line is up: something pending and the hardware enable (PMC_INTR_EN_0 bit 0) set */
int nv2a_irq_pending(void) { return pmc_intr() && (REG(PMC_INTR_EN_0) & 1); }

uint32_t mmio_read(uint32_t addr, int bits) {
    uint32_t off = addr - NV2A_BASE;
    switch (off) {
    case PMC_INTR_0: return pmc_intr();
    case PCRTC_INTR_0: return g_pcrtc_pending;
    case PFB_WC_CACHE: return REG(off) & ~0x10000u; /* write-combine flush finishes at once */
    case PGRAPH_SURFACE: return flip_surface_reg(REG(off));
    case PGRAPH_INCREMENT: return 0; /* write-only strobe */
    case PGRAPH_INTR: return (uint32_t)g_pgraph_intr;
    default: break;
    }
    return bits == 32 ? M32(addr) : bits == 16 ? M16(addr) : M8(addr);
}

void mmio_write(uint32_t addr, uint32_t v, int bits) {
    uint32_t off = addr - NV2A_BASE;
    if (ktrace() && (off & 0xFFF000) == 0x400000)
        fprintf(stderr, "[nv2a] PGRAPH write %06X <- %08X (intr %08X, stalled %d)\n", off, v, g_pgraph_intr, g_stalled);
    if (bits != 32) {
        if (bits == 16) M16(addr) = (uint16_t)v;
        else M8(addr) = (uint8_t)v;
        return;
    }
    switch (off) {
    case PMC_INTR_0: return; /* a summary of other registers; nothing to clear here */
    case PCRTC_INTR_0: g_pcrtc_pending &= ~v; return;
    case USER_DMA_GET: REG(off) = v; nv2a_pushbuffer_set_get(v); return;
    case USER_DMA_PUT:
        REG(off) = v;
        g_put = v;
        kick();
        return;
    case PGRAPH_INCREMENT:
        if (v & 2) flip_read_increment(); /* READ_3D */
        if (ktrace()) fprintf(stderr, "[nv2a] PGRAPH_INCREMENT %08X (flip read %u write %u)\n", v, g_flip_read, g_flip_write);
        return;
    case PGRAPH_INTR: InterlockedAnd(&g_pgraph_intr, ~(LONG)v); return; /* write-1-to-clear */
    case PGRAPH_FIFO_ACCESS:
        REG(off) = v;
        if ((v & 1) && !g_pgraph_intr) resume(STALL_TRAP); /* the handler is done with the trapped method */
        return;
    case PGRAPH_SURFACE: /* the driver may set the flip fields directly */
        AcquireSRWLockExclusive(&g_flip_lock);
        REG(off) = v;
        g_flip_write = (v >> 20) & 7;
        g_flip_read = (v >> 24) & 7;
        g_flip_modulo = (v >> 28) & 7 ? (v >> 28) & 7 : 1;
        ReleaseSRWLockExclusive(&g_flip_lock);
        return;
    case PCRTC_START:
        REG(off) = v;
        g_display_start = v;
        g_flips++;
        if (ktrace()) fprintf(stderr, "[nv2a] display start %08X\n", v);
        return;
    default: REG(off) = v; return;
    }
}

/* Called from the DPC thread (holding the guest lock) every millisecond. */
void nv2a_tick(void) {
    /* NV_PTIMER_TIME_0/1: nanoseconds, low 5 bits of TIME_0 always zero */
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    uint64_t ns = (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
    REG(0x009400) = (uint32_t)ns & ~0x1Fu;
    REG(0x009410) = (uint32_t)(ns >> 32) & 0x1FFFFFFF;
}
