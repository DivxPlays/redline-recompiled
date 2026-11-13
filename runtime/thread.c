/* Guest threads, the global guest lock, and dispatcher objects (events, semaphores, mutants, threads).
 *
 * Every guest thread is a host thread, but only the holder of g_gil runs guest code: lifted code is not
 * safe to run concurrently (the Xbox had one CPU). Blocking kernel calls drop the lock while they wait.
 * Waitable objects live in guest memory as DISPATCHER_HEADERs, and a handle is the object's guest address.
 */
#include <windows.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "xbox.h"

#define STATUS_SUCCESS 0x00000000u
#define NT_CURRENT_THREAD 0xFFFFFFFEu

/* DISPATCHER_HEADER: u8 Type, Absolute, Size, Inserted; i32 SignalState; LIST_ENTRY WaitListHead */
enum { OBJ_EVENT_NOTIFY = 0, OBJ_EVENT_SYNC = 1, OBJ_MUTANT = 2, OBJ_SEMAPHORE = 5, OBJ_THREAD = 6,
       OBJ_TIMER_NOTIFY = 8, OBJ_TIMER_SYNC = 9 };
#define OBJ_TYPE(o) M8(o)
#define OBJ_SIGNAL(o) (*(int32_t *)(g_mem + (uint32_t)(o) + 4))
#define MUTANT_OWNER(o) M32((o) + 0x18)
#define SEMAPHORE_LIMIT(o) (*(int32_t *)(g_mem + (uint32_t)(o) + 0x10))

/* after a change to an object's signal state: the threads waiting on it alone sleep on that word (wait_objects) */
static void wake(uint32_t o) { WakeByAddressAll(&OBJ_SIGNAL(o)); }

static THREAD_LOCAL jmp_buf *t_exit;
THREAD_LOCAL X86Ctx *t_ctx; /* this host thread's guest context, for crash reports */

/* The guest lock is a FIFO ticket lock, so a holder that yields at a SAFEPOINT really hands over instead of
 * winning the lock straight back. Waiters sleep on the "now serving" counter with WaitOnAddress. A holder
 * yields only after a 1 ms quantum: handing over at every back-edge made a polling thread and a busy one
 * trade the lock millions of times a second, each a host thread switch. */
#pragma comment(lib, "synchronization.lib")
static volatile long g_gil_next, g_gil_serving;
volatile long g_gil_waiters;
static LONGLONG g_gil_since, g_gil_quantum; /* QPC time the holder took the lock; 1 ms in QPC ticks */

/* GILLOG=1: every 2 s, how busy the lock was, who held it most (thread, guest function last entered, ms) and how
 * long threads waited for it; all kept by the holder, so no locking of its own */
static int gil_log(void) {
    static int log = -1;
    if (log < 0) log = getenv("GILLOG") != NULL;
    return log;
}
typedef struct {
    DWORD tid;
    uint32_t fn;
    double held, waited, worst_wait, worst_hold;
    int n;
    uint32_t worst_fn; /* guest function last entered when the longest hold ended */
} GilStat;
static GilStat g_gil_stat[64];
static LONGLONG g_gil_window;
DWORD g_dpc_tid; /* the vblank/DPC thread, named in the GILLOG report */

static int by_held(const void *a, const void *b) {
    double d = ((const GilStat *)b)->held - ((const GilStat *)a)->held;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static int gil_stat(void) {
    DWORD tid = GetCurrentThreadId();
    int i;
    for (i = 0; i < 63 && g_gil_stat[i].tid && g_gil_stat[i].tid != tid; i++) {}
    g_gil_stat[i].tid = tid;
    return i;
}

/* The DPC thread (vblank, timers) is the interrupt: while it waits for the lock, a holder hands over at its next
 * safepoint instead of finishing its quantum, so the 60 Hz vblank is not late by however many threads queued */
static volatile long g_gil_urgent;
static void gil_acquire_urgent(void) {
    InterlockedExchange(&g_gil_urgent, 1);
    gil_acquire();
    InterlockedExchange(&g_gil_urgent, 0);
}

void gil_acquire(void) {
    LARGE_INTEGER asked;
    if (gil_log()) QueryPerformanceCounter(&asked);
    long ticket = InterlockedIncrement(&g_gil_next) - 1;
    InterlockedIncrement(&g_gil_waiters);
    for (long now; (now = g_gil_serving) != ticket;) WaitOnAddress(&g_gil_serving, &now, sizeof now, INFINITE);
    InterlockedDecrement(&g_gil_waiters);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_gil_since = now.QuadPart;
    if (gil_log()) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        double ms = (now.QuadPart - asked.QuadPart) * 1000.0 / f.QuadPart;
        int i = gil_stat();
        g_gil_stat[i].waited += ms, g_gil_stat[i].n++;
        if (ms > g_gil_stat[i].worst_wait) g_gil_stat[i].worst_wait = ms;
    }
}

void gil_release(void) {
    if (gil_log()) {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        int i = gil_stat();
        double ms = (now.QuadPart - g_gil_since) * 1000.0 / f.QuadPart;
        g_gil_stat[i].held += ms;
        if (ms > g_gil_stat[i].worst_hold) g_gil_stat[i].worst_hold = ms, g_gil_stat[i].worst_fn = t_ctx ? t_ctx->last_fn : 0;
        if (t_ctx) g_gil_stat[i].fn = t_ctx->last_fn;
        if (!g_gil_window) g_gil_window = now.QuadPart;
        double span = (now.QuadPart - g_gil_window) * 1000.0 / f.QuadPart;
        if (span >= 2000) {
            double held = 0;
            int n = 0;
            for (int k = 0; k < 64 && g_gil_stat[k].tid; k++) held += g_gil_stat[k].held, n += g_gil_stat[k].n;
            fprintf(stderr, "[gil] busy %.0f%%, %d takes/s; by holder:", 100 * held / span, (int)(n * 1000 / span));
            int count;
            for (count = 0; count < 64 && g_gil_stat[count].tid; count++) {}
            qsort(g_gil_stat, count, sizeof g_gil_stat[0], by_held);
            for (int k = 0; k < count && k < 8; k++)
                fprintf(stderr, " %s%lu/%08X %.0fms (hold worst %.1f at %08X; wait %.1f avg %.1f worst)",
                        g_gil_stat[k].tid == g_dpc_tid ? "DPC " : "", g_gil_stat[k].tid, g_gil_stat[k].fn,
                        g_gil_stat[k].held, g_gil_stat[k].worst_hold, g_gil_stat[k].worst_fn,
                        g_gil_stat[k].n ? g_gil_stat[k].waited / g_gil_stat[k].n : 0, g_gil_stat[k].worst_wait);
            fprintf(stderr, "\n");
            memset(g_gil_stat, 0, sizeof g_gil_stat);
            g_gil_window = now.QuadPart;
        }
    }
    InterlockedIncrement(&g_gil_serving);
    WakeByAddressAll((void *)&g_gil_serving);
}

void gil_yield(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_gil_quantum) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_gil_quantum = f.QuadPart / 1000;
    }
    if (now.QuadPart - g_gil_since < g_gil_quantum && !g_gil_urgent) return;
    /* at DISPATCH_LEVEL or above nothing may preempt: not DPCs (vblank, flip queue) a thread's raised-IRQL code
     * guards against, nor threads in the middle of a DPC */
    if (t_ctx && M8(t_ctx->fs_base + 0x24) >= 2) {
        static LONG told;
        if (gil_log() && now.QuadPart - g_gil_since > 20 * g_gil_quantum && InterlockedIncrement(&told) <= 20)
            fprintf(stderr, "[gil] thread %lu at IRQL %u keeps the lock %.0f ms in %08X\n", GetCurrentThreadId(),
                    M8(t_ctx->fs_base + 0x24), (double)(now.QuadPart - g_gil_since) / g_gil_quantum, t_ctx->last_fn);
        return;
    }
    gil_release();
    gil_acquire();
}

uint32_t current_thread(X86Ctx *c) { return M32(c->fs_base + 0x28); }

static void obj_init(uint32_t o, uint8_t type, uint8_t size_dwords, int32_t signal) {
    memset(g_mem + o, 0, (size_t)size_dwords * 4);
    M8(o) = type;
    M8(o + 2) = size_dwords;
    OBJ_SIGNAL(o) = signal;
    M32(o + 8) = o + 8; /* empty WaitListHead */
    M32(o + 12) = o + 8;
}

static uint32_t handle_obj(X86Ctx *c, uint32_t h) { return h == NT_CURRENT_THREAD ? current_thread(c) : h; }

/* ---------------- thread environment ---------------- */

uint32_t thread_env_init(X86Ctx *c, uint32_t stack_size, uint32_t tls_size) {
    /* KPCR: NT_TIB 0x00..0x1B, SelfPcr 0x1C, Prcb 0x20, Irql 0x24, PrcbData (KPRCB) 0x28.. ; one per thread */
    uint32_t kpcr = kernel_alloc(0x400);
    uint32_t thread = kernel_alloc(0x200); /* KTHREAD; its DISPATCHER_HEADER signals on exit */
    obj_init(thread, OBJ_THREAD, 4, 0);
    uint32_t stack = guest_alloc(stack_size, 4096);

    /* TLS: the game reads [fs:4][tls_index] for its block, so fs:4 holds a one-slot array */
    uint32_t tls_block = 0, tls_slots = kernel_alloc(16);
    if (g_xbe.tls_dir) {
        uint32_t start = M32(g_xbe.tls_dir), end = M32(g_xbe.tls_dir + 4);
        uint32_t index_va = M32(g_xbe.tls_dir + 8), zero_fill = M32(g_xbe.tls_dir + 16);
        uint32_t need = end - start + zero_fill;
        tls_block = guest_alloc((tls_size > need ? tls_size : need) + 16, 16);
        if (end > start) memcpy(g_mem + tls_block, g_mem + start, end - start);
        if (index_va) M32(index_va) = 0;
    }
    M32(tls_slots) = tls_block;

    M32(kpcr + 0x00) = 0xFFFFFFFFu;   /* ExceptionList: end of SEH chain */
    M32(kpcr + 0x04) = tls_slots;     /* "StackBase" slot doubles as TLS pointer on Xbox */
    M32(kpcr + 0x08) = stack;         /* StackLimit */
    M32(kpcr + 0x18) = kpcr;          /* NtTib.Self */
    M32(kpcr + 0x1C) = kpcr;          /* SelfPcr */
    M32(kpcr + 0x20) = kpcr + 0x28;   /* Prcb */
    M32(kpcr + 0x28) = thread;        /* Prcb.CurrentThread */
    M32(thread + 0x28) = tls_block;   /* KTHREAD.TlsData */
    /* KTHREAD.ApcState.Process: the title is the one process. KPROCESS: ReadyListHead 0x00, ThreadListHead 0x08,
     * StackCount 0x10, ThreadQuantum 0x14, BasePriority 0x18 (XSetProcessQuantumLength writes the quantum) */
    static uint32_t process;
    if (!process) {
        process = kernel_alloc(0x20);
        memset(g_mem + process, 0, 0x20);
        M32(process + 0x14) = 60;
        M8(process + 0x18) = 8;
    }
    M32(thread + 0x44) = process;

    memset(c, 0, sizeof *c);
    c->fs_base = kpcr;
    c->esp = stack + stack_size - 16;
    c->fcw = 0x027F; /* Xbox default: 53-bit precision, all exceptions masked */
    c->mxcsr = 0x1F80;
    return thread;
}

/* Runs guest code at `target` with the stack already prepared; returns when it returns or the
 * thread calls PsTerminateSystemThread. Caller holds the GIL. */
/* every guest context, for the hang watchdog */
static X86Ctx *g_contexts[64];
static volatile LONG g_ncontexts;

static LONG track(X86Ctx *c) {
    LONG i = InterlockedIncrement(&g_ncontexts) - 1;
    if (i < 64) g_contexts[i] = c;
    return i;
}

/* HANGDUMP=<seconds>: after that long, print where every guest thread is and exit; creating build/hang.now prints
 * the same and carries on. Reads are unlocked on purpose: a stuck thread may be holding the guest lock. */
static uint32_t g_interrupts[16]; /* connected KINTERRUPTs (interrupts section below) */

/* where every guest thread is: its current function and the game's return addresses on its stack (HITCHLOG's
 * stalls too) */
void guest_threads_report(const char *tag) {
    for (LONG i = 0; i < g_ncontexts && i < 64; i++) {
        X86Ctx *c = g_contexts[i];
        if (!c) continue;
        fprintf(stderr, "[%s] thread %08X in function %08X stack:", tag, M32(c->fs_base + 0x28), c->last_fn);
        for (uint32_t k = 0, n = 0; k < 1024 && n < 16; k++) {
            uint32_t v = M32(c->esp + 4 * k);
            if (v > g_xbe.base + 0x1000 && v < g_xbe.base + g_xbe.image_size) fprintf(stderr, " %08X", v), n++;
        }
        fprintf(stderr, "\n");
    }
}

static void hang_report(void) {
    for (LONG i = 0; i < g_ncontexts && i < 64; i++) {
        X86Ctx *c = g_contexts[i];
        if (!c) continue;
        fprintf(stderr, "[hang] thread %08X in function %08X esp=%08X eax=%08X ecx=%08X edx=%08X stack:",
                M32(c->fs_base + 0x28), c->last_fn, c->esp, c->eax, c->ecx, c->edx);
        for (uint32_t k = 0, n = 0; k < 1024 && n < 24; k++) {
            uint32_t v = M32(c->esp + 4 * k);
            if (v > g_xbe.base + 0x1000 && v < g_xbe.base + g_xbe.image_size) {
                fprintf(stderr, " %08X", v);
                n++;
            }
        }
        fprintf(stderr, "\n");
    }
    /* sample each thread's current function for a second: shows every function in a spin loop */
    for (LONG i = 0; i < g_ncontexts && i < 64; i++) {
        uint32_t fn[16] = {0}, hits[16] = {0};
        if (!g_contexts[i]) continue;
        for (int s = 0; s < 1000; s++) {
            X86Ctx *c = g_contexts[i];
            if (!c) break;
            uint32_t f = c->last_fn;
            int k;
            for (k = 0; k < 16 && fn[k] && fn[k] != f; k++) {}
            if (k < 16) fn[k] = f, hits[k]++;
            Sleep(0);
            if (s % 10 == 0) Sleep(1);
        }
        fprintf(stderr, "[hang] thread %d samples:", (int)i);
        for (int k = 0; k < 16 && fn[k]; k++) fprintf(stderr, " %08X x%u", fn[k], hits[k]);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < 16; i++) { /* the GPU handler's context: D3D's flip queue at +0x174 */
        uint32_t ctx = g_interrupts[i] ? M32(g_interrupts[i] + 4) : 0;
        if (!ctx) continue;
        fprintf(stderr, "[hang] isr ctx %08X +174..+1D8:", ctx);
        for (uint32_t o = 0x174; o <= 0x1D8; o += 4) fprintf(stderr, " %08X", M32(ctx + o));
        fprintf(stderr, "\n");
    }
    nv2a_report();
    fflush(stderr);
}

static DWORD WINAPI watchdog_proc(void *p) {
    thread_name("watchdog");
    ULONGLONG due = p ? GetTickCount64() + 1000ull * (uintptr_t)p : ~0ull;
    for (;;) {
        Sleep(1000);
        if (DeleteFileA("build/prof.now")) profile_run(5);
        memwatch_poll();
        if (DeleteFileA("build/ram.now")) { /* guest RAM as it is (no lock: a snapshot taken while threads run), at
                                              * file offset = guest address; the unmapped first 64 KiB as zeros */
            static const uint8_t zero[GUEST_RAM_BASE];
            FILE *f = fopen("build/ram.bin", "wb");
            if (f) fwrite(zero, 1, sizeof zero, f), fwrite(g_mem + GUEST_RAM_BASE, 1, GUEST_RAM_END - GUEST_RAM_BASE, f), fclose(f);
            fprintf(stderr, "[ram] wrote build/ram.bin\n");
        }
        int asked = DeleteFileA("build/hang.now");
        if (!asked && GetTickCount64() < due) continue;
        hang_report();
        if (!asked) ExitProcess(5);
    }
}

void watchdog_start(void) {
    const char *s = getenv("HANGDUMP");
    CreateThread(NULL, 0, watchdog_proc, (void *)(uintptr_t)(s && atoi(s) > 0 ? atoi(s) : 0), 0, NULL);
}

static void run_guest(X86Ctx *c, uint32_t target) {
    t_ctx = c;
    LONG slot = track(c);
    jmp_buf jb;
    jmp_buf *outer = t_exit;
    t_exit = &jb;
    if (!setjmp(jb)) x86_call_indirect(c, target);
    t_exit = outer;
    OBJ_SIGNAL(current_thread(c)) = 1;
    wake(current_thread(c));
    if (slot < 64) g_contexts[slot] = NULL; /* the context is freed once the thread exits */
}

void thread_run_main(X86Ctx *c, uint32_t entry) {

    dpc_start();
    gil_acquire();
    PUSH32(0); /* return address for the entry point */
    run_guest(c, entry);
    fprintf(stderr, "main thread returned, eax=%08X\n", c->eax);
    gil_release();
}

typedef struct {
    X86Ctx ctx;
    uint32_t start_routine, start_context, system_routine;
} ThreadStart;

/* this host thread's name (SetThreadDescription, Windows 10 1607 and later): profilers and HITCHLOG show it */
void thread_name(const char *fmt, ...) {
    static HRESULT(WINAPI * set)(HANDLE, PCWSTR);
    static int init;
    if (!init) set = (HRESULT(WINAPI *)(HANDLE, PCWSTR))(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription"), init = 1;
    if (!set) return;
    char a[64];
    wchar_t w[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a, sizeof a, fmt, ap);
    va_end(ap);
    MultiByteToWideChar(CP_UTF8, 0, a, -1, w, 64);
    set(GetCurrentThread(), w);
}

static DWORD WINAPI thread_proc(void *p) {
    ThreadStart *ts = p;
    thread_name("guest %08X", ts->start_routine);
    X86Ctx *c = &ts->ctx;
    gil_acquire();
    uint32_t target;
    if (ts->system_routine) { /* SystemRoutine(StartRoutine, StartContext), stdcall */
        PUSH32(ts->start_context);
        PUSH32(ts->start_routine);
        target = ts->system_routine;
    } else {
        PUSH32(ts->start_context);
        target = ts->start_routine;
    }
    PUSH32(0);
    run_guest(c, target);
    gil_release();
    free(ts);
    return 0;
}

static uint32_t k_PsCreateSystemThreadEx(X86Ctx *c, const uint32_t *a) {
    /* (*ThreadHandle, ThreadExtensionSize, KernelStackSize, TlsDataSize, *ThreadId, StartRoutine,
     *  StartContext, CreateSuspended, DebuggerThread, SystemRoutine) */
    ThreadStart *ts = calloc(1, sizeof *ts);
    uint32_t stack = a[2] ? (a[2] + 0xFFFF) & ~0xFFFFu : 0x10000;
    uint32_t thread = thread_env_init(&ts->ctx, stack, a[3]);
    ts->start_routine = a[5];
    ts->start_context = a[6];
    ts->system_routine = a[9];
    DWORD tid;
    HANDLE h = CreateThread(NULL, 1 << 20, thread_proc, ts, (uint8_t)a[7] ? CREATE_SUSPENDED : 0, &tid);
    if (!h) {
        fprintf(stderr, "CreateThread failed\n");
        fatal_exit(1);
    }
    M32(thread + 0x1F0) = (uint32_t)(uintptr_t)h; /* host handle stashed in an unused KTHREAD tail slot */
    if (a[0]) M32(a[0]) = thread;
    if (a[4]) M32(a[4]) = tid;
    fprintf(stderr, "[thread] created %08X start=%08X ctx=%08X sys=%08X%s\n", thread, a[5], a[6], a[9],
            (uint8_t)a[7] ? " (suspended)" : "");
    return STATUS_SUCCESS;
}

static uint32_t k_PsTerminateSystemThread(X86Ctx *c, const uint32_t *a) {
    fprintf(stderr, "[thread] %08X terminated (%08X)\n", current_thread(c), a[0]);
    longjmp(*t_exit, 1);
}

static uint32_t k_NtResumeThread(X86Ctx *c, const uint32_t *a) {
    HANDLE h = (HANDLE)(uintptr_t)M32(handle_obj(c, a[0]) + 0x1F0);
    DWORD prev = h ? ResumeThread(h) : 0;
    if (a[1]) M32(a[1]) = prev;
    return STATUS_SUCCESS;
}
static uint32_t k_KeSetBasePriorityThread(X86Ctx *c, const uint32_t *a) { return 0; }

/* ---------------- waiting ---------------- */

/* Consume the object for `thread` if it is signaled. */
static int try_acquire(uint32_t o, uint32_t thread) {
    switch (OBJ_TYPE(o)) {
    case OBJ_EVENT_SYNC:
    case OBJ_TIMER_SYNC:
        if (OBJ_SIGNAL(o) <= 0) return 0;
        OBJ_SIGNAL(o) = 0;
        return 1;
    case OBJ_SEMAPHORE:
        if (OBJ_SIGNAL(o) <= 0) return 0;
        OBJ_SIGNAL(o) -= 1;
        return 1;
    case OBJ_MUTANT:
        if (OBJ_SIGNAL(o) <= 0 && MUTANT_OWNER(o) != thread) return 0;
        OBJ_SIGNAL(o) -= 1;
        MUTANT_OWNER(o) = thread;
        return 1;
    default: /* notification event/timer, thread */
        return OBJ_SIGNAL(o) > 0;
    }
}

static int is_signaled(uint32_t o, uint32_t thread) {
    if (OBJ_TYPE(o) == OBJ_MUTANT) return OBJ_SIGNAL(o) > 0 || MUTANT_OWNER(o) == thread;
    return OBJ_SIGNAL(o) > 0;
}

/* User APCs (I/O completion routines) wait on their thread's queue until it waits alertably, as on the Xbox:
 * run straight from the read, ReadFileEx callbacks found their caller's request half set up. Only a thread's own
 * I/O queues them (no NtQueueApcThread), so a thread-local queue checked as the wait starts is enough. */
static THREAD_LOCAL struct { uint32_t routine, args[3]; } t_apcs[256];
static THREAD_LOCAL int t_napcs;

void queue_user_apc(X86Ctx *c, uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (t_napcs == 256) { /* never seen: a thread that issues this much I/O without waiting alertably */
        fprintf(stderr, "[apc] queue full, running %08X now\n", routine);
        guest_call(c, routine, 3, (uint32_t[]){a0, a1, a2});
        return;
    }
    t_apcs[t_napcs].routine = routine;
    t_apcs[t_napcs].args[0] = a0, t_apcs[t_napcs].args[1] = a1, t_apcs[t_napcs++].args[2] = a2;
}

/* runs the queued APCs in order; returns whether there were any (the wait then ends with STATUS_USER_APC) */
static int deliver_apcs(X86Ctx *c) {
    int ran = t_napcs > 0;
    while (t_napcs) {
        uint32_t routine = t_apcs[0].routine, args[3];
        memcpy(args, t_apcs[0].args, sizeof args);
        memmove(&t_apcs[0], &t_apcs[1], --t_napcs * sizeof t_apcs[0]);
        guest_call(c, routine, 3, args);
    }
    return ran;
}

/* Timeout: guest pointer to LARGE_INTEGER in 100ns units (negative = relative), 0 = infinite.
 * A wait on one object sleeps on its signal state, which every setter wakes (wake): polling each millisecond, a
 * race at 144 had the game thread waiting on the GPU's events (D3D making room in the push buffer) a millisecond
 * at a time. The millisecond stays as a fallback for a setter that does not wake (file.c's I/O events).
 * shortcut: a wait on several objects still polls each millisecond; give it a wait list if one shows up hot. */
static uint32_t wait_objects(X86Ctx *c, uint32_t count, const uint32_t *objs, int wait_all, uint32_t timeout_ptr,
                             int alertable) {
    if (alertable && deliver_apcs(c)) return STATUS_USER_APC;
    uint32_t thread = current_thread(c);
    ULONGLONG deadline = ~0ull;
    if (timeout_ptr) {
        int64_t t = (int64_t)M64(timeout_ptr);
        if (t < 0) {
            deadline = GetTickCount64() + (ULONGLONG)(-t / 10000);
        } else {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            int64_t now = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            deadline = GetTickCount64() + (t > now ? (ULONGLONG)((t - now) / 10000) : 0);
        }
    }
    for (;;) {
        if (wait_all) {
            uint32_t i;
            for (i = 0; i < count && is_signaled(objs[i], thread); i++) {}
            if (i == count) {
                for (i = 0; i < count; i++) try_acquire(objs[i], thread);
                return STATUS_SUCCESS;
            }
        } else {
            for (uint32_t i = 0; i < count; i++)
                if (try_acquire(objs[i], thread)) return STATUS_SUCCESS + i;
        }
        if (GetTickCount64() >= deadline) return STATUS_TIMEOUT;
        int32_t seen = count == 1 ? OBJ_SIGNAL(objs[0]) : 0; /* read with the lock held: a change ends the sleep */
        gil_release();
        if (count == 1) WaitOnAddress(&OBJ_SIGNAL(objs[0]), &seen, sizeof seen, 1);
        else Sleep(1);
        gil_acquire();
    }
}

static uint32_t k_KeWaitForSingleObject(X86Ctx *c, const uint32_t *a) {
    /* (Object, WaitReason, WaitMode, Alertable, *Timeout) */
    return wait_objects(c, 1, &a[0], 0, a[4], a[3]);
}
static uint32_t k_KeWaitForMultipleObjects(X86Ctx *c, const uint32_t *a) {
    /* (Count, Object[], WaitType (0 all, 1 any), WaitReason, WaitMode, Alertable, *Timeout, WaitBlockArray) */
    uint32_t objs[64];
    uint32_t n = a[0] < 64 ? a[0] : 64;
    for (uint32_t i = 0; i < n; i++) objs[i] = M32(a[1] + 4 * i);
    return wait_objects(c, n, objs, a[2] == 0, a[6], a[5]);
}
static uint32_t k_NtWaitForSingleObject(X86Ctx *c, const uint32_t *a) {
    uint32_t o = handle_obj(c, a[0]);
    return wait_objects(c, 1, &o, 0, a[2], a[1]); /* (Handle, Alertable, *Timeout) */
}
static uint32_t k_NtWaitForSingleObjectEx(X86Ctx *c, const uint32_t *a) {
    uint32_t o = handle_obj(c, a[0]);
    return wait_objects(c, 1, &o, 0, a[3], a[2]); /* (Handle, WaitMode, Alertable, *Timeout) */
}
static uint32_t k_NtWaitForMultipleObjectsEx(X86Ctx *c, const uint32_t *a) {
    /* (Count, Handles[], WaitType, WaitMode, Alertable, *Timeout) */
    uint32_t objs[64];
    uint32_t n = a[0] < 64 ? a[0] : 64;
    for (uint32_t i = 0; i < n; i++) objs[i] = handle_obj(c, M32(a[1] + 4 * i));
    return wait_objects(c, n, objs, a[2] == 0, a[5], a[4]);
}
static uint32_t k_KeDelayExecutionThread(X86Ctx *c, const uint32_t *a) {
    /* (WaitMode, Alertable, *Interval) in 100ns units, negative = relative */
    if (a[1] && deliver_apcs(c)) return STATUS_USER_APC;
    int64_t t = (int64_t)M64(a[2]);
    gil_release();
    Sleep(t < 0 ? (DWORD)(-t / 10000) : 0);
    gil_acquire();
    return STATUS_SUCCESS;
}
static uint32_t k_NtYieldExecution(X86Ctx *c, const uint32_t *a) {
    gil_release();
    SwitchToThread();
    gil_acquire();
    return STATUS_SUCCESS;
}

/* ---------------- objects ---------------- */

static uint32_t k_NtCreateEvent(X86Ctx *c, const uint32_t *a) {
    /* (*EventHandle, ObjectAttributes, EventType (0 notification, 1 sync), InitialState); names ignored */
    uint32_t o = kernel_alloc(0x10);
    obj_init(o, a[2] ? OBJ_EVENT_SYNC : OBJ_EVENT_NOTIFY, 4, (uint8_t)a[3]);
    M32(a[0]) = o;
    return STATUS_SUCCESS;
}
static uint32_t k_NtCreateSemaphore(X86Ctx *c, const uint32_t *a) {
    /* (*SemaphoreHandle, ObjectAttributes, InitialCount, MaximumCount) */
    uint32_t o = kernel_alloc(0x14);
    obj_init(o, OBJ_SEMAPHORE, 5, (int32_t)a[2]);
    SEMAPHORE_LIMIT(o) = (int32_t)a[3];
    M32(a[0]) = o;
    return STATUS_SUCCESS;
}
static uint32_t k_NtCreateMutant(X86Ctx *c, const uint32_t *a) {
    /* (*MutantHandle, ObjectAttributes, InitialOwner) */
    uint32_t o = kernel_alloc(0x20);
    obj_init(o, OBJ_MUTANT, 8, (uint8_t)a[2] ? 0 : 1);
    if ((uint8_t)a[2]) MUTANT_OWNER(o) = current_thread(c);
    M32(a[0]) = o;
    return STATUS_SUCCESS;
}
static uint32_t k_KeSetEvent(X86Ctx *c, const uint32_t *a) {
    int32_t prev = OBJ_SIGNAL(a[0]);
    OBJ_SIGNAL(a[0]) = 1;
    wake(a[0]);
    return (uint32_t)prev;
}
static uint32_t k_NtSetEvent(X86Ctx *c, const uint32_t *a) {
    int32_t prev = OBJ_SIGNAL(a[0]);
    OBJ_SIGNAL(a[0]) = 1;
    wake(a[0]);
    if (a[1]) M32(a[1]) = (uint32_t)prev;
    return STATUS_SUCCESS;
}
static uint32_t k_NtClearEvent(X86Ctx *c, const uint32_t *a) {
    OBJ_SIGNAL(a[0]) = 0;
    return STATUS_SUCCESS;
}
static uint32_t k_NtReleaseSemaphore(X86Ctx *c, const uint32_t *a) {
    /* (Handle, ReleaseCount, *PreviousCount) */
    int32_t prev = OBJ_SIGNAL(a[0]);
    if (prev + (int32_t)a[1] > SEMAPHORE_LIMIT(a[0])) return 0xC0000047u; /* STATUS_SEMAPHORE_LIMIT_EXCEEDED */
    OBJ_SIGNAL(a[0]) = prev + (int32_t)a[1];
    wake(a[0]);
    if (a[2]) M32(a[2]) = (uint32_t)prev;
    return STATUS_SUCCESS;
}
static uint32_t k_NtReleaseMutant(X86Ctx *c, const uint32_t *a) {
    /* (Handle, *PreviousCount) */
    int32_t prev = OBJ_SIGNAL(a[0]);
    if (MUTANT_OWNER(a[0]) != current_thread(c)) return 0xC0000046u; /* STATUS_MUTANT_NOT_OWNED */
    if (++OBJ_SIGNAL(a[0]) > 0) MUTANT_OWNER(a[0]) = 0;
    wake(a[0]);
    if (a[1]) M32(a[1]) = (uint32_t)prev;
    return STATUS_SUCCESS;
}
static uint32_t k_ObReferenceObjectByHandle(X86Ctx *c, const uint32_t *a) {
    /* (Handle, ObjectType, *Object) */
    M32(a[2]) = handle_obj(c, a[0]);
    return STATUS_SUCCESS;
}
static uint32_t k_ObfDereferenceObject(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_NtClose(X86Ctx *c, const uint32_t *a) {
    file_close(a[0]); /* shortcut: other objects leak */
    return STATUS_SUCCESS;
}
static uint32_t k_NtDuplicateObject(X86Ctx *c, const uint32_t *a) {
    /* (SourceHandle, *TargetHandle, Options) */
    M32(a[1]) = handle_obj(c, a[0]);
    return STATUS_SUCCESS;
}

/* ---------------- critical sections ---------------- */

/* RTL_CRITICAL_SECTION (Xbox): DISPATCHER_HEADER 0x00, LockCount 0x10, RecursionCount 0x14, OwningThread 0x18 */
static uint32_t k_RtlInitializeCriticalSection(X86Ctx *c, const uint32_t *a) {
    memset(g_mem + a[0], 0, 0x1C);
    M32(a[0] + 0x10) = 0xFFFFFFFFu;
    return 0;
}
static int cs_try(X86Ctx *c, uint32_t cs) {
    uint32_t me = current_thread(c);
    if (M32(cs + 0x14) && M32(cs + 0x18) != me) return 0;
    M32(cs + 0x10) += 1;
    M32(cs + 0x14) += 1;
    M32(cs + 0x18) = me;
    return 1;
}
static uint32_t k_RtlEnterCriticalSection(X86Ctx *c, const uint32_t *a) {
    while (!cs_try(c, a[0])) {
        gil_release();
        SwitchToThread();
        gil_acquire();
    }
    return 0;
}
static uint32_t k_RtlTryEnterCriticalSection(X86Ctx *c, const uint32_t *a) { return cs_try(c, a[0]); }
static uint32_t k_RtlLeaveCriticalSection(X86Ctx *c, const uint32_t *a) {
    M32(a[0] + 0x10) -= 1;
    if (--M32(a[0] + 0x14) == 0) M32(a[0] + 0x18) = 0;
    return 0;
}

/* ---------------- guest calls from the kernel ---------------- */

/* Call guest code `target(args...)` (stdcall or cdecl) on context c; caller holds the GIL. */
uint32_t guest_call(X86Ctx *c, uint32_t target, int n, const uint32_t *args) {
    uint32_t saved_esp = c->esp;
    for (int i = n - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0);
    x86_call_indirect(c, target);
    c->esp = saved_esp;
    return c->eax;
}

/* ---------------- DPCs and timers ---------------- */

/* KDPC: u16 Type, u8 Inserted, u8 pad, LIST_ENTRY 0x04, DeferredRoutine 0x0C, DeferredContext 0x10,
 *       SystemArgument1 0x14, SystemArgument2 0x18
 * KTIMER: DISPATCHER_HEADER, DueTime 0x10, TimerListEntry 0x18, Dpc 0x20, Period 0x24 */
#define MAX_DPCS 256
#define MAX_TIMERS 256
static uint32_t g_dpc_queue[MAX_DPCS];
static int g_dpc_count;
static struct {
    uint32_t timer;
    ULONGLONG due;
    uint32_t period;
} g_timers[MAX_TIMERS];
static X86Ctx g_dpc_ctx;

static ULONGLONG due_ms(uint32_t lo, uint32_t hi) {
    int64_t t = (int64_t)(((uint64_t)hi << 32) | lo);
    if (t < 0) return GetTickCount64() + (ULONGLONG)(-t / 10000);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t now = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return GetTickCount64() + (t > now ? (ULONGLONG)((t - now) / 10000) : 0);
}

static int queue_dpc(uint32_t dpc, uint32_t a1, uint32_t a2) {
    if (M8(dpc + 2)) return 0; /* already queued */
    if (g_dpc_count == MAX_DPCS) {
        fprintf(stderr, "DPC queue full\n");
        fatal_exit(1);
    }
    M8(dpc + 2) = 1;
    M32(dpc + 0x14) = a1;
    M32(dpc + 0x18) = a2;
    g_dpc_queue[g_dpc_count++] = dpc;
    return 1;
}

static int timer_slot(uint32_t timer) {
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].timer == timer) return i;
    return -1;
}

#define GPU_VECTOR (0x30 + 3) /* HalGetInterruptVector: 0x30 + bus level; the GPU is level 3 */
static void raise_interrupt(uint32_t vector);

/* the GPU raised an interrupt (nv2a.c): the DPC thread runs the driver's handler now, not at its next millisecond.
 * D3D makes room in the push buffer by a software method the handler answers; the GPU waits on it meanwhile. */
static HANDLE g_dpc_kick;
void dpc_kick(void) {
    if (g_dpc_kick) SetEvent(g_dpc_kick);
}

static DWORD WINAPI dpc_proc(void *p) {
    thread_name("vblank / DPC");
    t_ctx = &g_dpc_ctx;
    g_dpc_tid = GetCurrentThreadId();
    track(&g_dpc_ctx);
    /* a high-resolution timer to the next millisecond, or to the vblank when that comes sooner: Sleep(1) woke
     * anywhere up to 2 ms late, and the vblank paces the game's frames */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    g_dpc_kick = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    static double next_vblank;
    for (;;) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        double ms = q.QuadPart * 1000.0 / qf.QuadPart;
        int vblank = next_vblank && next_vblank - ms < 1; /* this wake is the vblank's: it jumps ahead */
        double wait = vblank ? next_vblank - ms : 1;
        if (timer && wait > 0) {
            LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(wait * 10000)};
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            HANDLE h[2] = {timer, g_dpc_kick};
            WaitForMultipleObjects(g_dpc_kick ? 2 : 1, h, FALSE, INFINITE);
        } else if (!timer) Sleep(1);
        if (vblank) gil_acquire_urgent();
        else gil_acquire();
        nv2a_tick();
        M32(0xFE820010u) = (uint32_t)(GetTickCount64() * 48); /* APU global sample counter (XGSCNT), 48 kHz */
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < MAX_TIMERS; i++) {
            uint32_t t = g_timers[i].timer;
            if (!t || g_timers[i].due > now) continue;
            OBJ_SIGNAL(t) = 1;
            wake(t);
            if (M32(t + 0x20)) queue_dpc(M32(t + 0x20), 0, 0);
            if (g_timers[i].period) g_timers[i].due = now + g_timers[i].period;
            else g_timers[i].timer = 0;
        }
        /* the vertical blank, 60 Hz (FRAME RATE above 60: that rate, which the game then renders at, forza_vblank):
         * flag it in the NV2A registers; D3D's interrupt handler acks it */
        QueryPerformanceCounter(&q);
        ms = q.QuadPart * 1000.0 / qf.QuadPart;
        if (ms >= next_vblank - 0.05) {
            static double period;
            static int lock = -1; /* VSYNCLOCK=0: the emulated vblank keeps its own clock (testing) */
            static double aim;    /* VSYNCPHASE=<0..1>: where in the screen's refresh (testing) */
            if (!period) {
                period = 1000.0 / g_vblank_hz, lock = !getenv("VSYNCLOCK") || atoi(getenv("VSYNCLOCK"));
                aim = getenv("VSYNCPHASE") ? atof(getenv("VSYNCPHASE")) : 0.25;
            }
            next_vblank = (next_vblank && ms - next_vblank < 100 ? next_vblank : ms) + period;
            /* keep step with the screen when this rate is its refresh rate or a whole fraction of it (60 on 120 Hz):
             * on its own clock the emulated vblank drifts against the screen's, and wherever the two cross a refresh
             * shows a frame twice or a frame is never shown. Each vblank moves a tenth of the way to a quarter of a
             * refresh after the screen's: presents only ever come late, and a windowed one must be in by about 1.5 ms
             * before the vblank, when the compositor takes it (NOTES.md). A late wake moves it little. */
            double last, real;
            if (lock && display_vblank(&last, &real) && ms - last < 1000) {
                int n = (int)(period / real + 0.5);
                if (n >= 1 && fabs(n * real - period) < period * 0.01) { /* screens run a little off: 60.27, 143.98 */
                    double err = fmod(next_vblank - last - real * aim, real);
                    if (err < 0) err += real;
                    if (err > real / 2) err -= real;
                    next_vblank -= err / 10;
                }
            }
            forza_vblank();
            nv2a_vblank();
        }
        if (nv2a_irq_pending()) raise_interrupt(GPU_VECTOR); /* level-triggered: until the driver acks */
        /* DPC routines may queue more DPCs; run until the queue drains */
        while (g_dpc_count) {
            uint32_t dpc = g_dpc_queue[0];
            memmove(g_dpc_queue, g_dpc_queue + 1, --g_dpc_count * sizeof g_dpc_queue[0]);
            M8(dpc + 2) = 0;
            uint32_t args[4] = {dpc, M32(dpc + 0x10), M32(dpc + 0x14), M32(dpc + 0x18)};
            guest_call(&g_dpc_ctx, M32(dpc + 0x0C), 4, args);
        }
        gil_release();
    }
}

void dpc_start(void) {
    thread_env_init(&g_dpc_ctx, 64 * 1024, 0);
    M8(g_dpc_ctx.fs_base + 0x24) = 2; /* DISPATCH_LEVEL */
    if (!CreateThread(NULL, 1 << 20, dpc_proc, NULL, 0, NULL)) {
        fprintf(stderr, "cannot start DPC thread\n");
        fatal_exit(1);
    }
}

static uint32_t k_KeInitializeDpc(X86Ctx *c, const uint32_t *a) {
    /* (Dpc, DeferredRoutine, DeferredContext) */
    memset(g_mem + a[0], 0, 0x1C);
    M16(a[0]) = 19; /* DpcObject */
    M32(a[0] + 0x0C) = a[1];
    M32(a[0] + 0x10) = a[2];
    return 0;
}
static uint32_t k_KeInsertQueueDpc(X86Ctx *c, const uint32_t *a) { return queue_dpc(a[0], a[1], a[2]); }
static uint32_t k_KeRemoveQueueDpc(X86Ctx *c, const uint32_t *a) {
    for (int i = 0; i < g_dpc_count; i++)
        if (g_dpc_queue[i] == a[0]) {
            memmove(g_dpc_queue + i, g_dpc_queue + i + 1, (--g_dpc_count - i) * sizeof g_dpc_queue[0]);
            M8(a[0] + 2) = 0;
            return 1;
        }
    return 0;
}
static uint32_t k_KeInitializeTimerEx(X86Ctx *c, const uint32_t *a) {
    /* (Timer, Type: 0 notification, 1 synchronization) */
    obj_init(a[0], a[1] ? OBJ_TIMER_SYNC : OBJ_TIMER_NOTIFY, 10, 0);
    return 0;
}
static uint32_t set_timer(uint32_t timer, uint32_t lo, uint32_t hi, uint32_t period, uint32_t dpc) {
    int i = timer_slot(timer), was = i >= 0;
    if (i < 0) i = timer_slot(0);
    if (i < 0) {
        fprintf(stderr, "too many active timers\n");
        fatal_exit(1);
    }
    OBJ_SIGNAL(timer) = 0;
    M32(timer + 0x20) = dpc;
    M32(timer + 0x24) = period;
    g_timers[i].timer = timer;
    g_timers[i].due = due_ms(lo, hi);
    g_timers[i].period = period;
    return was;
}
static uint32_t k_KeSetTimer(X86Ctx *c, const uint32_t *a) { return set_timer(a[0], a[1], a[2], 0, a[3]); }
static uint32_t k_KeSetTimerEx(X86Ctx *c, const uint32_t *a) { return set_timer(a[0], a[1], a[2], a[3], a[4]); }
static uint32_t k_KeCancelTimer(X86Ctx *c, const uint32_t *a) {
    int i = timer_slot(a[0]);
    if (i < 0) return 0;
    g_timers[i].timer = 0;
    return 1;
}

/* ---------------- interrupts ---------------- */

/* Only the GPU raises interrupts (vblank, from the DPC thread); the others (USB, APU) never fire.
 * Our KINTERRUPT layout: routine 0x00, context 0x04, vector 0x10, irql 0x14. */
#define MAX_INTERRUPTS 16
static uint32_t g_interrupts[MAX_INTERRUPTS];

static uint32_t k_HalGetInterruptVector(X86Ctx *c, const uint32_t *a) {
    /* (BusInterruptLevel, *Irql) */
    if (a[1]) M8(a[1]) = (uint8_t)(26 - a[0]);
    return 0x30 + a[0];
}
static uint32_t k_KeInitializeInterrupt(X86Ctx *c, const uint32_t *a) {
    /* (Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, InterruptMode, ShareVector) */
    memset(g_mem + a[0], 0, 0x30);
    M32(a[0] + 0x00) = a[1];
    M32(a[0] + 0x04) = a[2];
    M32(a[0] + 0x10) = a[3];
    M32(a[0] + 0x14) = a[4];
    return 0;
}
static uint32_t k_KeConnectInterrupt(X86Ctx *c, const uint32_t *a) {
    for (int i = 0; i < MAX_INTERRUPTS; i++)
        if (!g_interrupts[i]) {
            g_interrupts[i] = a[0];
            return 1;
        }
    return 0;
}
static uint32_t k_KeDisconnectInterrupt(X86Ctx *c, const uint32_t *a) {
    for (int i = 0; i < MAX_INTERRUPTS; i++)
        if (g_interrupts[i] == a[0]) g_interrupts[i] = 0;
    return 1;
}

/* Call every service routine connected to `vector`, on the DPC thread's context at the interrupt's IRQL. */
static void raise_interrupt(uint32_t vector) {
    for (int i = 0; i < MAX_INTERRUPTS; i++) {
        uint32_t obj = g_interrupts[i];
        if (!obj || M32(obj + 0x10) != vector) continue;
        uint8_t irql = M8(g_dpc_ctx.fs_base + 0x24);
        M8(g_dpc_ctx.fs_base + 0x24) = (uint8_t)M32(obj + 0x14);
        uint32_t args[2] = {obj, M32(obj + 0x04)};
        guest_call(&g_dpc_ctx, M32(obj), 2, args);
        M8(g_dpc_ctx.fs_base + 0x24) = irql;
    }
}
static uint32_t k_KeSynchronizeExecution(X86Ctx *c, const uint32_t *a) {
    /* (Interrupt, SynchronizeRoutine, SynchronizeContext): run at the interrupt's IRQL so the DPC thread can't cut in */
    uint8_t irql = M8(c->fs_base + 0x24);
    M8(c->fs_base + 0x24) = (uint8_t)M32(a[0] + 0x14);
    uint32_t r = guest_call(c, a[1], 1, &a[2]);
    M8(c->fs_base + 0x24) = irql;
    return r;
}

#define K(name) {#name, k_##name}
const KImpl g_thread_impls[] = {
    K(KeInitializeDpc), K(KeInsertQueueDpc), K(KeRemoveQueueDpc), K(KeInitializeTimerEx), K(KeSetTimer),
    K(KeSetTimerEx), K(KeCancelTimer), K(HalGetInterruptVector), K(KeInitializeInterrupt),
    K(KeConnectInterrupt), K(KeDisconnectInterrupt), K(KeSynchronizeExecution),
    K(PsCreateSystemThreadEx), K(PsTerminateSystemThread), K(NtResumeThread), K(KeSetBasePriorityThread),
    K(KeWaitForSingleObject), K(KeWaitForMultipleObjects), K(NtWaitForSingleObject), K(NtWaitForSingleObjectEx),
    K(NtWaitForMultipleObjectsEx), K(KeDelayExecutionThread), K(NtYieldExecution),
    K(NtCreateEvent), K(NtCreateSemaphore), K(NtCreateMutant), K(KeSetEvent), K(NtSetEvent), K(NtClearEvent),
    K(NtReleaseSemaphore), K(NtReleaseMutant), K(ObReferenceObjectByHandle), K(ObfDereferenceObject),
    K(NtClose), K(NtDuplicateObject),
    K(RtlInitializeCriticalSection), K(RtlEnterCriticalSection), K(RtlTryEnterCriticalSection),
    K(RtlLeaveCriticalSection),
    {NULL, NULL},
};
