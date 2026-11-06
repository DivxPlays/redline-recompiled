/* Bink video (RAD's library, statically linked in the BINK section): opens and frame copies, for the upscaled
 * cutscenes (video.c). The game opens videos through CBinkWrapper::Open(name, flags) (0x3A99B0, thiscall; vtable
 * 0x4CE4B8), which keeps the Bink handle at +0x48 (and tries D:\MEDIA\placeholder.bik when a file is missing);
 * frames reach memory through BinkCopyToBuffer (0x3ACF90: bink, dest, pitch, height, x, y, flags), found among the
 * wrapper's calls into the library by its seven arguments. The handle starts {width, height, frames, frame number
 * (1-based)}. BINKLOG=1 logs both. */
#include <stdio.h>
#include <stdlib.h>

#include "xbox.h"

#define ARG(i) M32(c->esp + 4 + 4 * (i))

void o_003ACF90(X86Ctx *c); /* lifted originals */
void o_003A99B0(X86Ctx *c);

static int bink_log(void) {
    static int log = -1;
    if (log < 0) log = getenv("BINKLOG") != NULL;
    return log;
}

void hle_BinkCopyToBuffer(X86Ctx *c) {
    uint32_t b = ARG(0), dest = ARG(1), span = ARG(2) * ARG(3); /* pitch x height */
    if (bink_log())
        fprintf(stderr, "[bink] copy %08X (%ux%u, %u frames, frame %u) -> %08X pitch %u height %u at %u,%u flags %08X\n", b,
                M32(b), M32(b + 4), M32(b + 8), M32(b + 12), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    /* the frame shows upscaled (video.c): the game's own conversion into its buffer, lifted MMX that held the guest
     * lock up to ~95 ms a frame (the vblank waited, and cutscenes stuttered), is passed over. stdcall, 7 arguments;
     * the one caller ignores the result. */
    if (video_ready(b, M32(b + 12), dest)) c->eax = 0, c->esp += 4 + 7 * 4;
    else o_003ACF90(c);
    video_copied(b, M32(b + 12), dest, span, M8(dest + 3));
}

void hle_BinkWrapperOpen(X86Ctx *c) {
    uint32_t wrapper = c->ecx, name = ARG(0), flags = ARG(1);
    char n[160] = "";
    for (int i = 0; name && i < 159 && M8(name + i); i++) n[i] = (char)M8(name + i), n[i + 1] = 0;
    o_003A99B0(c);
    uint32_t bink = M32(wrapper + 0x48);
    if (bink_log()) fprintf(stderr, "[bink] open \"%s\" flags %08X -> %08X\n", n, flags, bink);
    video_open(bink, n);
}
