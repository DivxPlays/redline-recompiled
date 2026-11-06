/* XAPI fibers on host fibers.
 *
 * XAPI's SwitchToFiber saves esp, loads the other fiber's and returns on its stack. Lifted code returns to its host
 * caller instead, so the guest and host stacks would part ways. Here each guest fiber runs on a host fiber with its
 * own guest stack, and a switch saves the guest registers of the fiber being left and restores them when it resumes.
 * The guest-visible state stays XAPI's: the thread's TLS block holds the current fiber at +0x0C (read inline by
 * GetCurrentFiber/GetFiberData), and a fiber object is { parameter, StackBase, StackLimit, saved esp } at the top
 * of the fiber's own stack (a converted thread's lives in its TLS block at +0x10), with the stack sized as XAPI
 * sizes it (too small a stack overflowed into neighbouring heap objects).
 */
#include <windows.h>
#include <stdio.h>

#include "xbox.h"

static int flog(void) { /* FIBERLOG=1 */
    static int on = -1;
    if (on < 0) on = getenv("FIBERLOG") != NULL;
    return on;
}

#define ARG(i) M32(c->esp + 4 + 4 * (i))
static void ret(X86Ctx *c, uint32_t value, int n) {
    c->eax = value;
    c->esp += 4 + 4 * n;
}

#define MAX_FIBERS 64
static struct Fiber {
    void *host;
    X86Ctx regs;   /* guest registers while switched out (a new fiber: its initial state) */
    uint32_t start, param;
    uint32_t seh;  /* fs:[0], the exception-registration chain, which XAPI switches with the fiber */
    uint32_t obj;  /* the guest fiber object */
} g_fibers[MAX_FIBERS];

static struct Fiber *fiber_of(uint32_t obj) {
    for (int i = 0; obj && i < MAX_FIBERS; i++)
        if (g_fibers[i].obj == obj) return &g_fibers[i];
    return NULL;
}

/* a free slot for the guest fiber object `obj`, which is set to { param, stack base, stack limit, 0 } */
static struct Fiber *new_fiber(uint32_t obj, uint32_t param, uint32_t base, uint32_t limit) {
    for (int i = 0; i < MAX_FIBERS; i++)
        if (!g_fibers[i].obj) {
            M32(obj) = param, M32(obj + 4) = base, M32(obj + 8) = limit, M32(obj + 12) = 0;
            memset(&g_fibers[i], 0, sizeof g_fibers[i]);
            g_fibers[i].obj = obj, g_fibers[i].param = param;
            return &g_fibers[i];
        }
    fprintf(stderr, "too many fibers\n");
    fatal_exit(1);
}

/* the thread's TLS block (XBE TLS index), whose +0x0C XAPI uses for the current fiber */
static uint32_t tls_block(X86Ctx *c) { return M32(M32(c->fs_base + 4) + 4 * M32(M32(g_xbe.tls_dir + 8))); }

static void WINAPI fiber_main(void *p) {
    struct Fiber *f = p;
    X86Ctx *c = t_ctx;
    *c = f->regs;
    M32(c->fs_base) = f->seh;
    guest_call(c, f->start, 1, &f->param);
    fprintf(stderr, "fiber routine %08X returned\n", f->start); /* XAPI exits the thread here; nothing does that */
    fatal_exit(7);
}

/* ConvertThreadToFiber(lpParameter) */
void hle_ConvertThreadToFiber(X86Ctx *c) {
    uint32_t obj = tls_block(c) + 0x10, thread = M32(c->fs_base + 0x28);
    struct Fiber *f = fiber_of(obj);
    if (!f) f = new_fiber(obj, ARG(0), M32(thread + 0x1C), 0); /* KTHREAD+0x1C: the thread's stack base */
    f->host = ConvertThreadToFiber(NULL);
    if (!f->host) f->host = GetCurrentFiber(); /* already a fiber (a second guest call) */
    M32(tls_block(c) + 0x0C) = obj;
    if (flog()) fprintf(stderr, "[fiber] convert -> %08X (tls %08X)\n", obj, tls_block(c));
    ret(c, obj, 1);
}

/* CreateFiber(dwStackSize, lpStartAddress, lpParameter) */
void hle_CreateFiber(X86Ctx *c) {
    /* size 0 means the XBE's default stack (the header's stack commit, +0x130), as XAPI does: Forza's race loader
     * passes 0 and needs more than 64 KiB */
    uint32_t size = ARG(0) ? ARG(0) : M32(g_xbe.base + 0x130);
    size = (size < 0x3000 ? 0x3000 : size + 0xFFF) & ~0xFFFu;
    /* a page of slack below the stack limit, where MmCreateKernelStack's guard page would be */
    uint32_t top = guest_alloc(size + 0x1000, 16) + size + 0x1000, obj = top - 16;
    struct Fiber *f = new_fiber(obj, ARG(2), top, top - size);
    f->start = ARG(1);
    f->regs = *c;
    f->regs.esp = obj - 16; /* the routine's frame starts just below the object */
    f->seh = 0xFFFFFFFF;    /* empty chain */
    f->host = CreateFiber(1 << 20, fiber_main, f); /* lifted code nests deeply on the host stack */
    if (flog()) fprintf(stderr, "[fiber] create %08X start %08X param %08X stack %08X..%08X\n", obj, f->start, f->param,
                        top - size, top);
    if (!f->host) memset(f, 0, sizeof *f);
    ret(c, f->host ? obj : 0, 3);
}

/* SwitchToFiber(lpFiber): returns in the fiber being left once something switches back to it */
void hle_SwitchToFiber(X86Ctx *c) {
    uint32_t to_obj = ARG(0);
    ret(c, 0, 1);
    struct Fiber *to = fiber_of(to_obj), *from = fiber_of(M32(tls_block(c) + 0x0C));
    if (flog())
        fprintf(stderr, "[fiber] switch %08X -> %08X (esp %08X, target esp %08X)\n", M32(tls_block(c) + 0x0C), to_obj,
                c->esp, to ? to->regs.esp : 0);
    if (!to || !from || to == from) return;
    from->regs = *c;
    from->seh = M32(c->fs_base);
    to->regs.fs_base = c->fs_base; /* runs on this thread whichever thread created it */
    M32(tls_block(c) + 0x0C) = to_obj;
    SwitchToFiber(to->host);
    *c = from->regs; /* back in `from` */
    M32(c->fs_base) = from->seh;
}

/* DeleteFiber(lpFiber) */
void hle_DeleteFiber(X86Ctx *c) {
    struct Fiber *f = fiber_of(ARG(0));
    if (f && f->start) {
        DeleteFiber(f->host);
        memset(f, 0, sizeof *f); /* shortcut: its guest stack is never reused (the bump heap does not free) */
    }
    ret(c, 0, 1);
}
