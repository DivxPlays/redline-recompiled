/* Host replacements for parts of the XDK Direct3D library (see runtime/hle.txt). */
#include <stdio.h>

#include "xbox.h"

/* stack argument i of the current guest call ([esp] is the return address) */
#define ARG(i) M32(c->esp + 4 + 4 * (i))

/* return from a stdcall/thiscall guest function that takes n stack arguments */
static void ret(X86Ctx *c, uint32_t value, int n) {
    c->eax = value;
    c->esp += 4 + 4 * n;
}

void o_003C2A52(X86Ctx *c); /* lifted original */

/* CMiniport::CreateCtxDmaObject(id, class, base, limit, pObject) (thiscall): tell the push buffer interpreter
 * where each DMA context points, then run the original. Class 0x3D is an in-memory DMA object. D3D sizes its
 * memory contexts for a 128 MiB devkit (limit 0x07FFAFFF, below the GPU's instance memory); with more guest RAM
 * (VIDEOMODE 1080) they reach all of it, as D3D's offsets already do. */
void hle_CMiniport_CreateCtxDmaObject(X86Ctx *c) {
    uint32_t limit = ARG(3);
    if (limit == 0x07FFAFFF && GUEST_RAM_END > 0x08000000) limit = GUEST_RAM_END - 1;
    nv2a_dma_object(ARG(0), ARG(1), ARG(2), limit);
    o_003C2A52(c);
}

/* ---------------- video overlay (Bink movies) ----------------
 * The NV2A's video unit scans out a YUY2 overlay surface over the framebuffer. EnableOverlay waits on that
 * hardware; here both calls just tell the renderer what to composite at present time. */
void hle_D3DDevice_EnableOverlay(X86Ctx *c) { /* (Enable) */
    render_overlay(ARG(0) != 0, 0, 0, 0, 0, 0, NULL, 0, 0);
    ret(c, 0, 1);
}
void hle_D3DDevice_UpdateOverlay(X86Ctx *c) {
    /* (pSurface, SrcRect, DstRect, EnableColorKey, ColorKey). D3DSurface: Data 0x04, Format 0x0C, Size 0x10
     * (linear: width-1 [11:0], height-1 [23:12], pitch/64-1 [31:24]) */
    uint32_t s = ARG(0);
    if (s) {
        uint32_t size = M32(s + 0x10), dst = ARG(2);
        int rect[4];
        if (dst) for (int i = 0; i < 4; i++) rect[i] = (int)M32(dst + 4 * i); /* RECT: left, top, right, bottom */
        render_overlay(1, M32(s + 4) & 0x0FFFFFFF, (size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1,
                       ((size >> 24) + 1) * 64, (M32(s + 0x0C) >> 8) & 0xFF, dst ? rect : NULL, ARG(3) != 0, ARG(4));
    }
    ret(c, 0, 5);
}
