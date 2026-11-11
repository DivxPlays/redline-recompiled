/* Loader: reserve guest memory, map Forza.xbe, wire up the kernel, and jump to the entry point. */
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <ctype.h>
#include <io.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"
#include "xbox.h"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shell32.lib") /* report_crash: the browser */
#pragma comment(lib, "shlwapi.lib") /* StrStrIA */
#pragma comment(lib, "winmm.lib") /* timeBeginPeriod */

XbeInfo g_xbe;
uint32_t g_ram_end = 0x08000000u;
static uint32_t g_heap_top = GUEST_HEAP_BASE, g_kernel_top = KERNEL_BASE;
static char g_root[MAX_PATH];

#pragma comment(lib, "onecore.lib") /* VirtualAlloc2 / MapViewOfFile3 */

static HANDLE g_ram; /* guest physical RAM; mapped at its own addresses and again at PHYS_ALIAS */

static void fail(const char *what, uint32_t va) {
    fprintf(stderr, "%s at %08X failed (%lu)\n", what, va, GetLastError());
    fatal_exit(1);
}

/* split [va, va+size) out of the 4 GiB placeholder reservation */
static void carve(uint32_t va, uint32_t size) {
    if (!VirtualFree(g_mem + va, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("carve", va);
}

static void map_ram(uint32_t va, uint32_t phys, uint32_t size) {
    carve(va, size);
    if (!MapViewOfFile3(g_ram, GetCurrentProcess(), g_mem + va, phys, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                        NULL, 0))
        fail("map RAM", va);
}

static void commit(uint32_t va, uint32_t size) {
    carve(va, size);
    if (!VirtualAlloc2(GetCurrentProcess(), g_mem + va, size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                       PAGE_READWRITE, NULL, 0))
        fail("commit", va);
}

/* MEMWATCH=<hex address>: log every write to that guest dword with the guest instruction that made it. Its host page
 * is read-only; a write faults, is logged, runs once single-stepped with the page writable, and the page is locked
 * again. Other writes to the page take the same path silently. MEMWATCH=r<hex address> logs reads too (the page is
 * then inaccessible), each from a new guest function once, with the code addresses found on the guest stack; it is
 * armed when build/watch.now appears, since a file read into an inaccessible page fails. Only the direct mapping
 * is watched, not the RAM aliases. */
static uint32_t g_watch, g_watch_reads;
static THREAD_LOCAL int t_watch_step; /* 1: stepping over a write to the page, 2: ... to the watched dword */
static void watch_lock(int on) {
    DWORD old;
    VirtualProtect(g_mem + (g_watch & ~0xFFFu), 4096, !on ? PAGE_READWRITE : g_watch_reads ? PAGE_NOACCESS : PAGE_READONLY,
                   &old);
}
void memwatch_poll(void) { /* the watchdog, once a second */
    if (g_watch_reads && DeleteFileA("build/watch.now")) fprintf(stderr, "[watch] armed\n"), watch_lock(1);
}

/* Guest RAM above GUEST_HEAP_BASE: first fit over an address-ordered free list, else the bump top. Each block's size
 * is kept by address, so the kernel's frees need only the address (a bump heap that never reused memory ran out of
 * 120 MiB loading a race on a 64 MiB console). */
#define MAX_FREE 8192
static struct { uint32_t base, size; } g_free[MAX_FREE];
static int g_nfree;
#define BLOCK_SLOTS (1u << 16) /* open addressing on the block's address; va 0 marks an empty slot */
static struct { uint32_t va, size; } g_blocks[BLOCK_SLOTS];
static uint32_t g_nblocks, g_live; /* blocks handed out, and their bytes */

static uint32_t block_hash(uint32_t va) { return (va >> 4) * 2654435761u >> 16; }
static uint32_t block_slot(uint32_t va) { /* va's slot, or the empty slot where it would go */
    uint32_t i = block_hash(va);
    while (g_blocks[i].va && g_blocks[i].va != va) i = (i + 1) & (BLOCK_SLOTS - 1);
    return i;
}
static void block_remove(uint32_t i) { /* backward-shift deletion keeps every probe chain unbroken */
    for (uint32_t j = i;;) {
        g_blocks[i].va = 0;
        for (;;) {
            j = (j + 1) & (BLOCK_SLOTS - 1);
            if (!g_blocks[j].va) return;
            uint32_t k = block_hash(g_blocks[j].va);
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue; /* already reachable from its home */
            break;
        }
        g_blocks[i] = g_blocks[j];
        i = j;
    }
}

static void free_range(uint32_t base, uint32_t size) {
    if (base + size == g_heap_top) { /* the top block: lower the top, and past a free range that now ends there */
        g_heap_top = base;
        if (g_nfree && g_free[g_nfree - 1].base + g_free[g_nfree - 1].size == g_heap_top) g_heap_top = g_free[--g_nfree].base;
        return;
    }
    int i = 0;
    while (i < g_nfree && g_free[i].base < base) i++;
    int prev = i > 0 && g_free[i - 1].base + g_free[i - 1].size == base, next = i < g_nfree && base + size == g_free[i].base;
    if (prev && next) {
        g_free[i - 1].size += size + g_free[i].size;
        memmove(&g_free[i], &g_free[i + 1], (--g_nfree - i) * sizeof g_free[0]);
    } else if (prev) {
        g_free[i - 1].size += size;
    } else if (next) {
        g_free[i].base = base, g_free[i].size += size;
    } else if (g_nfree < MAX_FREE) {
        memmove(&g_free[i + 1], &g_free[i], (g_nfree++ - i) * sizeof g_free[0]);
        g_free[i].base = base, g_free[i].size = size;
    } /* else the range is lost: only fragmentation past MAX_FREE ranges gets here */
}

/* bytes per allocating guest function, printed if the heap runs out */
static struct { uint32_t fn, bytes, n; } g_alloc_by_fn[64];

/* live bytes, the heap top, free bytes below it and the largest free range (UILOG prints them on each screen) */
void guest_heap_stats(uint32_t *live, uint32_t *top, uint32_t *free_bytes, uint32_t *largest) {
    *live = g_live, *top = g_heap_top, *free_bytes = *largest = 0;
    for (int i = 0; i < g_nfree; i++) {
        *free_bytes += g_free[i].size;
        if (g_free[i].size > *largest) *largest = g_free[i].size;
    }
}

uint32_t guest_alloc(uint32_t size, uint32_t align) {
    if (align < 16) align = 16;
    size = size ? (size + 15) & ~15u : 16;
    uint32_t va = 0;
    for (int i = 0; i < g_nfree && !va; i++) {
        uint32_t b = g_free[i].base, e = b + g_free[i].size, a = (b + align - 1) & ~(align - 1);
        if (a < b || a >= e || e - a < size) continue;
        if (a + size == e) {
            if (a == b) memmove(&g_free[i], &g_free[i + 1], (--g_nfree - i) * sizeof g_free[0]);
            else g_free[i].size = a - b;
        } else if (a == b) {
            g_free[i].base = a + size, g_free[i].size = e - a - size;
        } else {
            if (g_nfree == MAX_FREE) continue;
            memmove(&g_free[i + 2], &g_free[i + 1], (g_nfree++ - i - 1) * sizeof g_free[0]);
            g_free[i].size = a - b;
            g_free[i + 1].base = a + size, g_free[i + 1].size = e - a - size;
        }
        va = a;
    }
    if (!va) {
        va = (g_heap_top + align - 1) & ~(align - 1);
        if (va + size < va || va + size > GUEST_RAM_END) {
            uint32_t live, top, free_bytes, largest;
            guest_heap_stats(&live, &top, &free_bytes, &largest);
            fprintf(stderr, "guest heap exhausted (want %X aligned %X at %08X; %d free ranges, %X bytes, the largest %X; "
                    "%u live blocks, %X bytes)\n", size, align, va, g_nfree, free_bytes, largest, g_nblocks, live);
            for (int i = 0; i < 64 && g_alloc_by_fn[i].fn; i++)
                if (g_alloc_by_fn[i].bytes >= 0x100000)
                    fprintf(stderr, "  by %08X: %u allocations, %X bytes\n", g_alloc_by_fn[i].fn, g_alloc_by_fn[i].n,
                            g_alloc_by_fn[i].bytes);
            fatal_exit(1);
        }
        uint32_t gap = g_heap_top;
        g_heap_top = va + size;
        if (va > gap) free_range(gap, va - gap); /* what the alignment skipped */
    }
    uint32_t s = block_slot(va);
    if (g_nblocks < BLOCK_SLOTS * 3 / 4) g_blocks[s].va = va, g_blocks[s].size = size, g_nblocks++, g_live += size;
    uint32_t fn = t_ctx ? t_ctx->last_fn : 0;
    int k = 0;
    while (k < 63 && g_alloc_by_fn[k].fn && g_alloc_by_fn[k].fn != fn) k++;
    g_alloc_by_fn[k].fn = fn, g_alloc_by_fn[k].bytes += size, g_alloc_by_fn[k].n++;
    memset(g_mem + va, 0, size);
    if (g_watch && g_watch - va < size)
        fprintf(stderr, "[watch] %08X is in guest_alloc %08X+%X (function %08X)\n", g_watch, va, size, t_ctx ? t_ctx->last_fn : 0);
    return va;
}

/* returns a block guest_alloc handed out (any RAM alias of its address); returns its size, 0 if va is not one */
uint32_t guest_free(uint32_t va) {
    if (va - PHYS_ALIAS < GUEST_RAM_END) va -= PHYS_ALIAS;
    else if (va - WC_ALIAS < GUEST_RAM_END) va -= WC_ALIAS;
    uint32_t s = block_slot(va), size = g_blocks[s].size;
    if (!g_blocks[s].va) return 0;
    block_remove(s);
    g_nblocks--, g_live -= size;
    free_range(va, size);
    return size;
}

uint32_t guest_block_size(uint32_t va) {
    if (va - PHYS_ALIAS < GUEST_RAM_END) va -= PHYS_ALIAS;
    else if (va - WC_ALIAS < GUEST_RAM_END) va -= WC_ALIAS;
    uint32_t s = block_slot(va);
    return g_blocks[s].va ? g_blocks[s].size : 0;
}

/* allocate, free out of order, reuse holes: freeing everything must restore the heap exactly. 64 KiB alignment keeps
 * the layout fixed (no such block fits the gap below the first one, wherever the heap top starts). */
void guest_alloc_selftest(void) {
    uint32_t top = g_heap_top, nfree = g_nfree, a = guest_alloc(0x20000, 0x10000), b = guest_alloc(0x10000, 0x10000),
             big = guest_alloc(0x20000, 0x10000), d = guest_alloc(0x10000, 0x10000);
    int ok = guest_free(b) == 0x10000 && guest_free(big) == 0x20000;
    uint32_t e = guest_alloc(0x10000, 0x10000), f = guest_alloc(0x20000, 0x10000); /* first fit: b's hole, big's */
    ok = ok && e == b && f == big && d == big + 0x20000 && guest_block_size(PHYS_ALIAS + a) == 0x20000;
    ok = ok && guest_free(e) && guest_free(a) && guest_free(f) && guest_free(d) && !guest_free(d);    ok = ok && g_heap_top == top && (uint32_t)g_nfree == nfree;
    if (!ok) {
        fprintf(stderr, "runtime: guest_alloc self-test failed\n");
        abort();
    }
}

uint32_t guest_heap_top(void) { return g_heap_top; }

uint32_t kernel_alloc(uint32_t size) {
    uint32_t va = (g_kernel_top + 15) & ~15u;
    if (va + size > KERNEL_BASE + KERNEL_SIZE) {
        fprintf(stderr, "kernel area exhausted\n");
        fatal_exit(1);
    }
    g_kernel_top = va + size;
    return va;
}

const char *guest_path_root(void) { return g_root; }

/* Settings: an environment variable (for scripts) over recomp.ini [recomp] next to the exe, which the in-game Recomp
 * Settings menu writes (hle_forza.c). The running game keeps what it read first: changes apply at the next start. */
static char g_ini[MAX_PATH];
static const char *ini_path(void) {
    if (!g_ini[0] && GetModuleFileNameA(NULL, g_ini, MAX_PATH)) {
        char *slash = strrchr(g_ini, '\\');
        strcpy(slash ? slash + 1 : g_ini, "recomp.ini");
    }
    return g_ini;
}
int cfg_saved(const char *key, int def) { return (int)GetPrivateProfileIntA("recomp", key, def, ini_path()); }
void cfg_save(const char *key, int value) {
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    WritePrivateProfileStringA("recomp", key, v, ini_path());
}
int cfg_get(const char *env, const char *key, int def) {
    const char *s = getenv(env);
    return s ? atoi(s) : cfg_saved(key, def);
}

int cfg_widescreen(void) {
    static int v = -1;
    if (v < 0) v = cfg_get("WIDESCREEN", "widescreen", 1) != 0;
    return v;
}

int cfg_video_lines(void) {
    static int v = -1;
    if (v < 0) v = cfg_get("VIDEOMODE", "videomode", 480);
    return v;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        fatal_exit(1);
    }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size);
    if (!buf || fread(buf, 1, *size, f) != *size) {
        fprintf(stderr, "cannot read %s\n", path);
        fatal_exit(1);
    }
    fclose(f);
    return buf;
}

#define RD32(p, o) (*(uint32_t *)((p) + (o)))

static void load_xbe(const char *path) {
    size_t size;
    uint8_t *x = read_file(path, &size);
    if (size < 0x180 || memcmp(x, "XBEH", 4)) {
        fprintf(stderr, "%s is not an XBE\n", path);
        fatal_exit(1);
    }
    g_xbe.base = RD32(x, 0x104);
    g_xbe.image_size = RD32(x, 0x10C);
    g_xbe.cert = RD32(x, 0x118);
    g_xbe.tls_dir = RD32(x, 0x12C);
    /* retail XOR keys; debug keys are 0x94859D4B / 0xEFB1F152 */
    g_xbe.entry = RD32(x, 0x128) ^ 0xA8FC57ABu;
    g_xbe.kernel_thunk = RD32(x, 0x158) ^ 0x5B6D40B6u;

    uint32_t hdr_size = RD32(x, 0x108);
    memcpy(g_mem + g_xbe.base, x, hdr_size); /* titles read their own header/certificate at 0x10000 */

    uint32_t nsec = RD32(x, 0x11C), sec = RD32(x, 0x120) - g_xbe.base;
    for (uint32_t i = 0; i < nsec; i++) {
        uint8_t *s = x + sec + i * 0x38;
        uint32_t va = RD32(s, 4), raw = RD32(s, 12), rsize = RD32(s, 16);
        if (raw + rsize > size) {
            fprintf(stderr, "section %u out of file bounds\n", i);
            fatal_exit(1);
        }
        /* load every section up front (inserted ones too), so XeLoadSection only has to report success */
        memcpy(g_mem + va, x + raw, rsize);
    }
    free(x);
}

/* name the guest instruction a host crash address belongs to: the PDB gives the lifted C line, and the lifter
 * puts a "/ * VVVVVVVV insn * /" comment above each instruction's code */
static void guest_line_of(DWORD64 rip) {
    HANDLE proc = GetCurrentProcess();
    IMAGEHLP_LINE64 line = {sizeof line};
    DWORD disp;
    static int ready = -1;
    if (ready < 0) {
        SymSetOptions(SYMOPT_LOAD_LINES);
        ready = SymInitialize(proc, NULL, TRUE);
    }
    if (!ready || !SymGetLineFromAddr64(proc, rip, &disp, &line)) { /* the module and offset still name it */
        HMODULE mod = NULL;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)rip, &mod))
            GetModuleFileNameA(mod, path, MAX_PATH);
        const char *base = strrchr(path, '\\');
        fprintf(stderr, "  (no line info for host address %llX: %s+%llX)\n", (unsigned long long)rip,
                base ? base + 1 : path, mod ? (unsigned long long)(rip - (DWORD64)mod) : 0ull);
        return;
    }
    char text[512], last[512] = "";
    FILE *f = fopen(line.FileName, "r");
    for (DWORD n = 1; f && n <= line.LineNumber && fgets(text, sizeof text, f); n++) {
        const char *c = strstr(text, "/* ");
        if (c && strlen(c) > 12 && isxdigit((unsigned char)c[3]) && c[11] == ' ') snprintf(last, sizeof last, "%s", c);
    }
    if (f) fclose(f);
    fprintf(stderr, "  at %s:%lu, guest %s", line.FileName, line.LineNumber, last[0] ? last : "(no guest comment)\n");
}

static LONG CALLBACK on_crash(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (g_watch && r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch_step) {
        if (t_watch_step == 2) fprintf(stderr, "[watch]   now %08X\n", M32(g_watch));
        t_watch_step = 0;
        watch_lock(1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_watch && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (r->ExceptionInformation[0] == 1 || g_watch_reads)) {
        uint8_t *w = (uint8_t *)r->ExceptionInformation[1];
        if (w >= g_mem && (uint32_t)(w - g_mem) - (g_watch & ~0xFFFu) < 4096) {
            uint32_t ga = (uint32_t)(w - g_mem);
            t_watch_step = 1;
            watch_lock(0); /* before logging, which reads the watched dword */
            if (r->ExceptionInformation[0] == 0 && ga - g_watch < 4 && t_ctx) {
                static uint32_t seen[64];
                int i = 0;
                while (i < 64 && seen[i] && seen[i] != t_ctx->last_fn) i++;
                if (i < 64 && !seen[i]) {
                    seen[i] = t_ctx->last_fn;
                    fprintf(stderr, "[watch] read of %08X in function %08X, stack:", ga, t_ctx->last_fn);
                    for (uint32_t s = t_ctx->esp, n = 0; s < t_ctx->esp + 0x400 && n < 12; s += 4) {
                        uint32_t v = M32(s);
                        if (v - g_xbe.base < g_xbe.image_size && v - g_xbe.base > 0x1000) fprintf(stderr, " %08X", v), n++;
                    }
                    fprintf(stderr, "\n");
                    guest_line_of(e->ContextRecord->Rip);
                }
            } else if (r->ExceptionInformation[0] == 1 && (ga - g_watch < 4 || g_watch - ga < 4)) {
                fprintf(stderr, "[watch] write to %08X (was %08X) in function %08X, thread %08X\n", ga, M32(g_watch),
                        t_ctx ? t_ctx->last_fn : 0, t_ctx ? current_thread(t_ctx) : 0);
                guest_line_of(e->ContextRecord->Rip);
                t_watch_step = 2;
            }
            e->ContextRecord->EFlags |= 0x100; /* trap after this one instruction */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uint8_t *p = (uint8_t *)r->ExceptionInformation[1];
        if (p >= g_mem && p < g_mem + 0x100000000ull)
            fprintf(stderr, "guest memory %s at guest address %08X (unmapped) in function %08X\n",
                    r->ExceptionInformation[0] ? "write" : "read", (uint32_t)(p - g_mem), t_ctx ? t_ctx->last_fn : 0);
        else
            fprintf(stderr, "host access violation at %p\n", p);
        guest_line_of(e->ContextRecord->Rip);
        /* heuristic backtrace: values on the guest stack that point into the image; only from a stack in RAM, as a
         * fault here would land in this handler again */
        if (t_ctx && t_ctx->esp - GUEST_RAM_BASE < GUEST_RAM_END - GUEST_RAM_BASE - 1024) {
            fprintf(stderr, "  guest stack (esp=%08X):", t_ctx->esp);
            for (uint32_t i = 0, n = 0; i < 256 && n < 12; i++) {
                uint32_t v = M32(t_ctx->esp + 4 * i);
                if (v > g_xbe.base + 0x1000 && v < g_xbe.base + g_xbe.image_size) {
                    fprintf(stderr, " %08X", v);
                    n++;
                }
            }
            fprintf(stderr, "\n  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X ebp=%08X\n", t_ctx->eax,
                    t_ctx->ecx, t_ctx->edx, t_ctx->ebx, t_ctx->esi, t_ctx->edi, t_ctx->ebp);
        }
        fflush(stderr);
        fatal_exit(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* s with every `what` (any case) replaced by `with`, in place */
static void replace_all(char *s, size_t cap, const char *what, const char *with) {
    size_t wl = what ? strlen(what) : 0, nl = strlen(with);
    if (wl < 3) return; /* nothing worth hiding, and a short one would hit ordinary words */
    for (char *p = s; (p = StrStrIA(p, what)) != NULL; p += nl) {
        if (strlen(s) - wl + nl >= cap) return;
        memmove(p + nl, p + wl, strlen(p + wl) + 1);
        memcpy(p, with, nl);
    }
}

/* GitHub's new-issue page in the browser, filled in from this crash: what the box said, the version, Windows, the
 * game's video settings and the end of the log, with the player's user name, PC name and folders taken out. Nothing
 * is sent: the page is the player's to submit or not. CRASHREPORT_FILE=<path> writes the address there instead
 * (tests). */
static void report_crash(const char *why, const char *tail) {
    static char body[6000], url[20000];
    char port[MAX_PATH] = "";
    if (GetModuleFileNameA(NULL, port, MAX_PATH)) { /* the port's folder: the exe is in its build\ */
        for (int i = 0; i < 2 && strrchr(port, '\\'); i++) *strrchr(port, '\\') = 0;
    }
    RTL_OSVERSIONINFOW os = {sizeof os};
    LONG(WINAPI * rtl_version)(RTL_OSVERSIONINFOW *) =
        (LONG(WINAPI *)(RTL_OSVERSIONINFOW *))GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (rtl_version) rtl_version(&os);
    const char *t = tail;
    for (int lines = 0, i = (int)strlen(tail) - 1; i > 0; i--) /* the last 30 lines at most */
        if (tail[i] == '\n' && ++lines == 30) {
            t = tail + i + 1;
            break;
        }
    if (strlen(t) > 2200) t += strlen(t) - 2200;
    snprintf(body, sizeof body,
             "**What were you doing?** (a menu, a race: which track and car)\n\n\n"
             "**The error:** %s\n\n**Version:** " RECOMP_NAME " " RECOMP_VERSION "\n**Windows:** %lu.%lu.%lu\n"
             "**Settings:** video mode %d, render scale %d, upscaler %d, ultrawide %d, widescreen %d\n\n"
             "**The end of the log** (build\\redline.log):\n```\n%s\n```\n",
             why, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, cfg_video_lines(),
             cfg_get("RENDERSCALE", "renderscale", 2), cfg_get("FSR", "fsr", 1), cfg_get("ULTRAWIDE", "ultrawide", 0),
             cfg_widescreen(), t);
    char title[200];
    snprintf(title, sizeof title, "Crash: %.120s", why);
    for (int k = 0; k < 2; k++) { /* folders first (the port's may be in the profile), then what is left of the names */
        char *s = k ? title : body;
        size_t cap = k ? sizeof title : sizeof body;
        replace_all(s, cap, port, "<port>");
        replace_all(s, cap, g_root, "<game>");
        replace_all(s, cap, getenv("USERPROFILE"), "<home>");
        replace_all(s, cap, getenv("USERNAME"), "<user>");
        replace_all(s, cap, getenv("COMPUTERNAME"), "<pc>");
    }
    if (strlen(title) > 90) strcpy(title + 87, "...");
    char *u = url + sprintf(url, "https://github.com/DivxPlays/redline-recompiled/issues/new?labels=bug&title=");
    for (int part = 0; part < 2; part++) { /* percent-encoded, as UTF-8 bytes */
        for (const unsigned char *c = (const unsigned char *)(part ? body : title); *c && u < url + sizeof url - 8; c++)
            u += isalnum(*c) || strchr("-_.~", *c) ? sprintf(u, "%c", *c) : sprintf(u, "%%%02X", *c);
        if (!part) u += sprintf(u, "&body=");
    }
    const char *file = getenv("CRASHREPORT_FILE");
    if (file) {
        FILE *f = fopen(file, "w");
        if (f) fputs(url, f), fclose(f);
    } else {
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* The game stops on an error: tell the player why (the log's last report: stderr is a file, the launcher's
 * build\redline.log, a script's or our own) and where the log is, and offer to report it (report_crash). A test
 * script (TESTKEYS) gets no box. */
void fatal_exit(int code) {
    static volatile LONG once;
    fflush(stderr);
    if (InterlockedExchange(&once, 1)) Sleep(INFINITE); /* another thread is already telling */
    if (!getenv("TESTKEYS")) {
        char path[MAX_PATH] = "", why[400] = "", msg[1400];
        static char whole[4097]; /* the tail as it was, for the report (strtok cuts up the other) */
        DWORD n = GetFinalPathNameByHandleA(GetStdHandle(STD_ERROR_HANDLE), path, MAX_PATH, 0);
        const char *log = n && n < MAX_PATH ? path + (strncmp(path, "\\\\?\\", 4) ? 0 : 4) : "";
        HANDLE f = *log ? CreateFileA(log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                      OPEN_EXISTING, 0, NULL)
                        : INVALID_HANDLE_VALUE;
        if (f != INVALID_HANDLE_VALUE) {
            static char tail[4097];
            LARGE_INTEGER size;
            DWORD got = 0;
            if (GetFileSizeEx(f, &size) && size.QuadPart > 4096) SetFilePointer(f, -4096, NULL, FILE_END);
            ReadFile(f, tail, 4096, &got, NULL);
            CloseHandle(f);
            tail[got] = 0;
            memcpy(whole, tail, got + 1);
            for (char *line = strtok(tail, "\r\n"); line; line = strtok(NULL, "\r\n"))
                if (*line != ' ' && *line != '\t') snprintf(why, sizeof why, "%s", line); /* a report, not its detail */
        }
        snprintf(msg, sizeof msg,
                 "The game stopped because of an error:\n\n%s\n\n%s%s\n\nReport this crash on GitHub? It opens a new "
                 "issue in your browser, filled in with the version, your Windows version and the end of the log "
                 "(your user name and folders taken out). Nothing is sent unless you submit it there.",
                 *why ? why : "(no details)", *log ? "The whole log is in " : "", log);
        if (MessageBoxA(NULL, msg, RECOMP_NAME, MB_ICONERROR | MB_YESNO | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
            report_crash(*why ? why : "(no details)", whole);
    }
    ExitProcess(code);
}

static void on_abort(int sig) {
    (void)sig;
    fatal_exit(3);
}

static LONG CALLBACK on_unhandled(EXCEPTION_POINTERS *e) {
    fprintf(stderr, "host exception %08lX at %p\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    fatal_exit(4);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* A Windows program has no console: when nothing took its output (the launcher and the scripts do), it goes to
 * redline.log beside the exe */
static void log_to_file(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n && n < MAX_PATH ? strrchr(path, '\\') : NULL;
    if (!slash) return;
    strcpy(slash + 1, "redline.log");
    if (!freopen(path, "w", stderr)) return;
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), 1); /* stdout too */
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr))); /* for fatal_exit */
}

int main(int argc, char **argv) {
    log_to_file();
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, on_abort);
    SetUnhandledExceptionFilter(on_unhandled);
    const char *game = argc > 1 ? argv[1] : "game";
    if (!GetFullPathNameA(game, MAX_PATH, g_root, NULL)) return 1;
    char xbe[MAX_PATH];
    snprintf(xbe, sizeof xbe, "%s\\Forza.xbe", g_root);

    /* 1080's back buffers take ~25 MB more than 128 MiB leaves a race, and 720p ran out after a few races too
     * (GitHub #17); the write-combined alias of RAM must end below the NV2A registers (WC_ALIAS + RAM <= NV2A_BASE,
     * so 208 MiB at most) */
    if (cfg_video_lines() >= 720) g_ram_end = 0x0C000000u;
    g_mem = VirtualAlloc2(GetCurrentProcess(), NULL, 0x100000000ull, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                          PAGE_NOACCESS, NULL, 0);
    g_ram = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, GUEST_RAM_END, NULL);
    if (!g_mem || !g_ram) {
        fprintf(stderr, "cannot reserve guest address space (%lu)\n", GetLastError());
        fatal_exit(1);
    }
    /* physical address == low guest address; drivers also reach RAM through PHYS_ALIAS + physical.
     * The first MiB of the alias is where the real kernel lives (0x80010000), so it stays separate and
     * holds a stub kernel PE header that XAPI inspects; below 1 MiB the game only has XBE header/code. */
    map_ram(GUEST_RAM_BASE, GUEST_RAM_BASE, GUEST_RAM_END - GUEST_RAM_BASE);
    commit(PHYS_ALIAS, 0x100000);
    map_ram(PHYS_ALIAS + 0x100000, 0x100000, GUEST_RAM_END - 0x100000);
    map_ram(WC_ALIAS, 0, GUEST_RAM_END);
    game_load();
    M16(0x80010000u) = 0x5A4D;   /* "MZ" */
    M32(0x8001003Cu) = 0x80;     /* e_lfanew */
    M32(0x80010080u) = 0x4550;   /* "PE\0\0", NumberOfSections = 0, SizeOfOptionalHeader = 0 */
    commit(KERNEL_BASE, KERNEL_SIZE);
    /* shortcut: USB (OHCI) registers as plain zeroed memory, so the XDK USB driver sees no devices;
     * goes away once XPP input is replaced with host input at the library level */
    commit(0xFED00000u, 0x10000);
    commit(NV2A_BASE, NV2A_SIZE);
    /* shortcut: audio processor (APU) and AC97 registers as zeroed memory: the game runs silent until
     * DirectSound gets a host implementation */
    commit(0xFE800000u, 0x80000);
    commit(0xFEC00000u, 0x1000);
    commit(0xFEF00000u, 0x10000); /* shortcut: Ethernet (nForce NIC) registers; PhyGetLinkState reports no link */
    nv2a_init();
    render_init();
    AddVectoredExceptionHandler(1, on_crash);
    if (getenv("MEMWATCH")) {
        const char *w = getenv("MEMWATCH");
        g_watch_reads = *w == 'r';
        g_watch = (uint32_t)strtoul(w + g_watch_reads, NULL, 16) & ~3u;
        fprintf(stderr, "[watch] watching %s %08X\n", g_watch_reads ? "reads and writes of" : "writes to", g_watch);
        if (!g_watch_reads) watch_lock(1);
    }

    load_xbe(xbe);
    forza_init();
    achieve_init();
    kernel_init();

    static X86Ctx ctx;
    thread_env_init(&ctx, 256 * 1024, 0);
    fprintf(stderr, RECOMP_NAME " " RECOMP_VERSION ": entry %08X, kernel thunk %08X\n", g_xbe.entry, g_xbe.kernel_thunk);
    watchdog_start();
    /* 1 ms sleeps: without this Sleep(1) lasts a 15.6 ms timer tick, so the DPC thread's 60 Hz vblank ran at 48 Hz
     * (the race clock counts vblanks) and every polling kernel wait woke 15 times late */
    timeBeginPeriod(1);
    thread_run_main(&ctx, g_xbe.entry);
    /* the startup thread may end while game threads keep running */
    ExitThread(0);
}
