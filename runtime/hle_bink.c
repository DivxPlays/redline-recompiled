/* Bink video (RAD's library, statically linked in the BINK section): opens and frame copies, for the upscaled
 * cutscenes (video.c). The game opens videos through CBinkWrapper::Open(name, flags) (0x3A99B0, thiscall; vtable
 * 0x4CE4B8), which keeps the Bink handle at +0x48 (and tries D:\MEDIA\placeholder.bik when a file is missing);
 * frames reach memory through BinkCopyToBuffer (0x3ACF90: bink, dest, pitch, height, x, y, flags), found among the
 * wrapper's calls into the library by its seven arguments. The handle starts {width, height, frames, frame number
 * (1-based)}; its frame rate is +0x14 / +0x18, which BinkWait (0x3AC840) paces by. BINKLOG=1 logs both. */
#include <stdio.h>
#include <stdlib.h>

#include "xbox.h"

#define ARG(i) M32(c->esp + 4 + 4 * (i))

extern GuestFn o_003ACF90; /* lifted originals */
extern GuestFn o_003A99B0;

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
    static int fps;
    uint32_t wrapper = c->ecx, name = ARG(0), flags = ARG(1);
    char n[160] = "";
    for (int i = 0; name && i < 159 && M8(name + i); i++) n[i] = (char)M8(name + i), n[i + 1] = 0;
    o_003A99B0(c);
    uint32_t bink = M32(wrapper + 0x48);
    /* Flags 1 opens a video at the game's own frame rate (refresh / vblanks per frame): the event list's track
     * flythroughs (Flythrough.bik, 29.97 fps), a frame per game frame, 30 on the Xbox (60 / 2). Above 30 that rate
     * is 72-144 and they ran 2.4-4.8x fast: they get their file's own rate back (BIK header +28 / +32). */
    if (!fps) fps = forza_fps();
    if (bink && (flags & 1) && fps > 30 && n[0] && n[1] == ':') {
        char p[512];
        uint32_t h[9];
        snprintf(p, sizeof p, "%s%s", guest_path_root(), n + 2); /* D: is the game folder */
        FILE *f = fopen(p, "rb");
        if (f && fread(h, 4, 9, f) == 9 && h[7] && h[8]) M32(bink + 0x14) = h[7], M32(bink + 0x18) = h[8];
        if (f) fclose(f);
    }
    if (bink_log())
        fprintf(stderr, "[bink] open \"%s\" flags %08X -> %08X at %u/%u fps\n", n, flags, bink, bink ? M32(bink + 0x14) : 0,
                bink ? M32(bink + 0x18) : 0);
    video_open(bink, n);
}
