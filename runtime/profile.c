/* Sampling profiler: creating build/prof.now samples every thread of the process for 5 s and prints the host
 * functions they were in, self and inclusive (lifted guest functions are f_<address>), each thread's CPU use and
 * the frames flipped meanwhile. The samples are unwound with the image's x64 unwind data, so the host stack is
 * the guest call stack. Nothing allocates while a thread is suspended: it may hold the heap lock. */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

#include "xbox.h"

#pragma comment(lib, "dbghelp.lib")

#define NSLOT 8192 /* distinct functions */
#define MAXT 64
typedef struct {
    DWORD64 fn;
    unsigned self, incl;
} Slot;
static Slot g_slot[NSLOT];
/* each sampled thread's own inclusive counts: the summed table hides which thread waits where */
#define NTSLOT 2048
typedef struct {
    DWORD64 fn;
    unsigned incl;
    int t;
} TSlot;
static TSlot g_tslot[NTSLOT];

static Slot *slot(DWORD64 fn) {
    for (unsigned i = (unsigned)(fn >> 4) * 2654435761u % NSLOT;; i = (i + 1) % NSLOT)
        if (g_slot[i].fn == fn || !g_slot[i].fn) return g_slot[i].fn = fn, &g_slot[i];
}
static void tslot_hit(DWORD64 fn, int t) { /* a full table drops the hit */
    for (unsigned n = 0, i = (unsigned)((fn >> 4) + t * 7919u) * 2654435761u % NTSLOT; n < NTSLOT; n++, i = (i + 1) % NTSLOT)
        if ((g_tslot[i].fn == fn && g_tslot[i].t == t) || !g_tslot[i].fn) {
            g_tslot[i].fn = fn, g_tslot[i].t = t, g_tslot[i].incl++;
            return;
        }
}
static int by_tincl(const void *a, const void *b) { return (int)((TSlot *)b)->incl - (int)((TSlot *)a)->incl; }

static int by_self(const void *a, const void *b) { return (int)((Slot *)b)->self - (int)((Slot *)a)->self; }
static int by_incl(const void *a, const void *b) { return (int)((Slot *)b)->incl - (int)((Slot *)a)->incl; }

static const char *name(DWORD64 fn) {
    static char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *s = (SYMBOL_INFO *)buf;
    static int sym;
    if (!sym) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        sym = SymInitialize(GetCurrentProcess(), NULL, TRUE);
    }
    s->SizeOfStruct = sizeof *s;
    s->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), fn, NULL, s)) return s->Name;
    HMODULE m; /* no symbols: the module at least, e.g. "nvwgf2umx.dll+1234" */
    static char mod[300];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)fn, &m))
        return "?";
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char *base = strrchr(path, '\\');
    snprintf(mod, sizeof mod, "%s+%llX", base ? base + 1 : path, (unsigned long long)(fn - (DWORD64)m));
    return mod;
}

/* one sample of a suspended thread: every distinct function on its stack gets an inclusive hit, the top one a self hit */
static void sample(HANDLE th, int t) {
    CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
    DWORD64 seen[64];
    int n = 0;
    if (!GetThreadContext(th, &ctx)) return;
    for (int d = 0; d < 64 && ctx.Rip; d++) {
        DWORD64 base;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        DWORD64 fn = f ? base + f->BeginAddress : ctx.Rip;
        Slot *s = slot(fn);
        if (!d) s->self++;
        int k;
        for (k = 0; k < n && seen[k] != fn; k++) {}
        if (k == n) seen[n++] = fn, s->incl++, tslot_hit(fn, t);
        if (!f) { /* leaf without unwind data: the return address is on top of the stack */
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void *handler_data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &handler_data, &frame, NULL);
    }
}

/* HITCHLOG's stalls: the host call stack of each thread whose name (thread_name) starts with one of `names` */
void profile_stacks(const char *const *names, int n) {
    static HRESULT(WINAPI * get)(HANDLE, PWSTR *);
    if (!get) get = (HRESULT(WINAPI *)(HANDLE, PWSTR *))(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    if (!get) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        PWSTR wname = NULL;
        char tname[64] = "";
        if (SUCCEEDED(get(th, &wname)) && wname) WideCharToMultiByte(CP_UTF8, 0, wname, -1, tname, sizeof tname, NULL, NULL);
        if (wname) LocalFree(wname);
        int want = 0;
        for (int i = 0; i < n && !want; i++) want = tname[0] && !strncmp(tname, names[i], strlen(names[i]));
        DWORD64 pcs[24];
        int depth = 0;
        if (want && SuspendThread(th) != (DWORD)-1) { /* the walk allocates nothing: the thread may hold the heap lock */
            CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
            if (GetThreadContext(th, &ctx))
                for (; depth < 24 && ctx.Rip; depth++) {
                    pcs[depth] = ctx.Rip;
                    DWORD64 base;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (!f) {
                        ctx.Rip = *(DWORD64 *)ctx.Rsp, ctx.Rsp += 8;
                        continue;
                    }
                    void *hd;
                    DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &hd, &frame, NULL);
                }
            ResumeThread(th);
            fprintf(stderr, "[stall] %s:", tname);
            for (int d = 0; d < depth; d++) fprintf(stderr, " %s;", name(pcs[d]));
            fprintf(stderr, "\n");
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

static ULONGLONG cpu_time(HANDLE th) {
    FILETIME a, b, k, u;
    return GetThreadTimes(th, &a, &b, &k, &u) ? ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                                                     ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)
                                               : 0;
}

void profile_run(int seconds) {
    HANDLE th[MAXT];
    DWORD tid[MAXT];
    ULONGLONG cpu0[MAXT];
    int nt = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && nt < MAXT; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) tid[nt] = te.th32ThreadID, cpu0[nt] = cpu_time(h), th[nt++] = h;
    }
    CloseHandle(snap);
    /* only the threads that are busy: suspending the 60-odd idle ones (driver, Media Foundation, pools) each sample
     * left a handful of samples in 5 s */
    Sleep(300);
    int busy = 0;
    for (int i = 0; i < nt; i++) {
        ULONGLONG used = cpu_time(th[i]) - cpu0[i];
        if (used > 300 * 10000 / 20) th[busy] = th[i], tid[busy] = tid[i], cpu0[busy++] = cpu_time(th[i]); /* over 5% */
        else CloseHandle(th[i]);
    }
    nt = busy;
    memset(g_slot, 0, sizeof g_slot);
    memset(g_tslot, 0, sizeof g_tslot);
    uint32_t flips0 = nv2a_flip_count(), vblanks0 = nv2a_vblank_count();
    ULONGLONG t0 = GetTickCount64();
    unsigned samples = 0;
    while (GetTickCount64() - t0 < 1000ull * seconds) {
        for (int i = 0; i < nt; i++) {
            if (SuspendThread(th[i]) == (DWORD)-1) continue;
            sample(th[i], i);
            ResumeThread(th[i]);
        }
        samples++;
        Sleep(1);
    }
    double wall = (GetTickCount64() - t0) / 1000.0;
    fprintf(stderr, "[prof] %.1f s, %u samples per thread, %.1f frames/s flipped, %.1f vblanks/s\n", wall, samples,
            (nv2a_flip_count() - flips0) / wall, (nv2a_vblank_count() - vblanks0) / wall);
    for (int i = 0; i < nt; i++) {
        double used = (cpu_time(th[i]) - cpu0[i]) / 1e7 / wall;
        PVOID start = NULL; /* the thread's start routine names whose it is */
        typedef LONG(WINAPI * NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
        static NtQIT query;
        if (!query) query = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
        if (query) query(th[i], 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL);
        if (used > 0.01)
            fprintf(stderr, "[prof] thread %5lu: %3.0f%% of a core, started at %s\n", tid[i], used * 100,
                    start ? name((DWORD64)start) : "?");
        CloseHandle(th[i]);
    }
    unsigned total = samples;
    qsort(g_tslot, NTSLOT, sizeof *g_tslot, by_tincl);
    for (int i = 0; i < nt; i++) {
        fprintf(stderr, "[prof] thread %lu, inclusive:", tid[i]);
        for (int k = 0, shown = 0; k < NTSLOT && g_tslot[k].incl && shown < 28; k++)
            if (g_tslot[k].t == i) fprintf(stderr, " %s %.0f%%;", name(g_tslot[k].fn), 100.0 * g_tslot[k].incl / total), shown++;
        fprintf(stderr, "\n");
    }
    qsort(g_slot, NSLOT, sizeof *g_slot, by_self);
    fprintf(stderr, "[prof] self (summed over threads, %% of one thread's time):\n");
    for (int i = 0; i < 40 && g_slot[i].self; i++)
        fprintf(stderr, "[prof]   %5.1f%% %s\n", 100.0 * g_slot[i].self / total, name(g_slot[i].fn));
    qsort(g_slot, NSLOT, sizeof *g_slot, by_incl);
    fprintf(stderr, "[prof] inclusive:\n");
    for (int i = 0; i < 60 && g_slot[i].incl; i++)
        fprintf(stderr, "[prof]   %5.1f%% %s\n", 100.0 * g_slot[i].incl / total, name(g_slot[i].fn));
    fflush(stderr);
}
