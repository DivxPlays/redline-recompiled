/* Forza-specific host replacements: the enhancements that need the game's own code to choose differently. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "xbox.h"

#define ARG(i) M32(c->esp + 4 + 4 * (i))

/* 0x1D96C0, bool __thiscall DisplayMode::Supported(max_width): the check Forza's display-mode picker (0x1DA960) runs
 * over its table {width, height, progressive, widescreen}: 1920x1080i, 1280x720p, 720x480p/i wide, 640x480p/i.
 * Every caller caps the width at 640, so retail Forza never leaves 480 lines though the table and engine hold HD
 * modes. This is the original check with the cap raised to the VIDEOMODE asked for (720: 1280, 1080: 1920); the
 * video flags are the ones the kernel reports from the same settings (XGetVideoFlags >> 16: 1 widescreen, 2 720p,
 * 4 1080i, 8 480p). */
void hle_ForzaDisplayModeSupported(X86Ctx *c) {
    uint32_t mode = c->ecx, max_w = ARG(0), w = M32(mode), h = M32(mode + 4);
    int lines = cfg_video_lines(), progressive = M8(mode + 8), wide = M8(mode + 9), ok = 1;
    uint32_t flags = (cfg_widescreen() ? 1 : 0) | (lines >= 720 ? 2 | 8 : 0) | (lines >= 1080 ? 4 : 0);
    if (lines >= 1080) max_w = 1920;
    else if (lines >= 720) max_w = 1280;
    if ((int32_t)max_w > 0 && w > max_w) ok = 0;
    if (h == 480 && ((wide && !(flags & 1)) || (progressive && !(flags & 8)))) ok = 0;
    if (h == 720 && !(flags & 2)) ok = 0;
    if (h == 1080 && !(flags & 4)) ok = 0;
    c->eax = ok;
    c->esp += 8; /* return address and the argument */
}

/* Frame rate (FRAME RATE: 30, 60, 120, 144). Forza paces itself by three values: the refresh rate (0x554F30: 60, 50
 * for PAL), vblanks per frame (0x554F24, 2 in the image), and from them D3D's presentation interval (0x1D95C0 builds
 * TWO_OR_IMMEDIATE into 0x5DA300, which 0x3B84B0 copies to D3D's 0x3C8F2C) and each frame's time step (0x1D9D10:
 * vblanks per frame / refresh, frame-time mode 0 at 0x5188FC): a fixed 1/30 s, so the game is locked at 30 and a
 * frame drawn late slows it. Above 30, vblanks per frame is 1 (ONE_OR_IMMEDIATE: every vblank may bring a frame), the
 * vblank runs at the rate chosen (thread.c), and the refresh the step divides by follows how many vblanks the last
 * frame actually took (144 / 3 = 48: 1/48 s), so the game keeps real time at whatever rate it manages (a race makes
 * ~65 a second today: the emulated GPU works under the guest lock). Not vblanks per frame itself: the game rebuilds
 * its presentation interval from that, and a slow frame then locked it to every third vblank. */
int g_vblank_hz = 60; /* the emulated vblank's rate (thread.c) */
static int g_fps, g_autodrive;

void forza_init(void) {
    g_fps = cfg_get("FRAMERATE", "framerate", 60);
    if (g_fps > 60) g_vblank_hz = g_fps;
    if (g_fps != 30) M32(0x554F24u) = 1; /* before the game builds its presentation interval from it */
    g_autodrive = getenv("AUTODRIVE") != NULL;
    /* ULTRAWIDE: the camera's aspect (0x554F00) comes from these, 16:9 and split-screen's 32:9 (0x95A54, 0x1DA9C0);
     * the game keeps the vertical view and sees more to the sides. The showroom's (0x33AB9E: viewport width / height
     * x 0x495CD4, the 4:3 aspect, which is how much wider a 16:9 screen shows a pixel than 640x480 does) too; the
     * 4:3 camera's other two reads of it (0x95A92, 0x1DA9E7) are for a 4:3 picture, which ULTRAWIDE leaves alone */
    MF32(0x495CDCu) *= render_widen(), MF32(0x495CE0u) *= render_widen(), MF32(0x495CD4u) *= render_widen();
}

/* AUTODRIVE=1 (testing): the player's car drives itself, so a script can finish races (results, replays, career
 * money). 0x597E0 is CHumanDriver's step from the pad to the car's controls (car+0xA0, +0xE88), which the human
 * driver's step (0x5A680) then records for the replay (0xC1E50); the game's debug CAutoHumanDriver (flag 0x588B0E)
 * steps an AI there instead (0x599E0), but one set up so that it wanders off the track. Here the car's own second
 * driver steps there: the AI every player car gets (car+0x44, 0x22A8CF; it takes over after the finish), in the AI
 * cars' mode 0 (in its mode 1 it holds the car on the grid). 0xB0A50 runs a car's drivers, the human one and then
 * that AI, which would write the controls again after they were recorded: for the player's car it runs without it,
 * so the AI steps once, inside the human driver.
 * shortcut: the replay of such a race leaves the race's path after half a minute or so (two plays of one replay agree,
 * so likely the AI changes more of the car than the controls a replay records): it shows that replays play, not that
 * they are faithful; drive a race by hand for that.
 * shortcut: not in a Drivatar lesson (no opponents): the AI's planning there reads a list the race never set up (null
 * [this+0x4C] in 0x2518D0, reached from the AI's step) and the game stops; lessons are driven by hand or W. */
static uint32_t g_auto_car, g_auto_ai; /* the player's car and its AI during 0xB0A50 */
extern GuestFn o_000B0A50;
void hle_ForzaCarDrivers(X86Ctx *c) {
    uint32_t car = M32(c->esp + 4), human = M32(car + 0x40), ai = M32(car + 0x44);
    achieve_car(car, human && M32(human) == 0x4902A4u);
    if (!g_autodrive || !human || M32(human) != 0x4902A4u || !ai) { /* CHumanDriver's vtable */
        o_000B0A50(c);
        return;
    }
    g_auto_car = car, g_auto_ai = ai;
    M32(car + 0x44) = 0;
    o_000B0A50(c);
    M32(car + 0x44) = ai;
    g_auto_car = g_auto_ai = 0;
}

extern GuestFn o_000597E0;
void hle_ForzaHumanControls(X86Ctx *c) {
    uint32_t self = c->ecx, car = M32(self + 0x18), ai = car && car == g_auto_car ? g_auto_ai : 0;
    if (!ai) {
        o_000597E0(c);
        return;
    }
    uint32_t ctl = M32(c->esp + 4), arg = M32(c->esp + 8), mode = M32(ai + 0x64); /* return address, the controls, the
                                                                                    * step's input */
    M32(ai + 0x64) = 0;
    M8(ai + 0x14) = 1; /* plan this step: the race loop shares planning out among the cars (0xB0DDA), but not to this one */
    c->ecx = ai;
    guest_call(c, M32(M32(ai) + 0xC), 1, &arg);
    c->ecx = self;
    M32(ai + 0x64) = mode;
    if (M8(ctl + 3) & 0x60) { /* steering as a float (bits 5-6, 0xDE9D0): into the byte a replay records, as 0x599E0 */
        float f = MF32(ctl + 4), x = (f < 0 ? -f : f) - 1.0f;
        int v = (int)((1.0 - (double)x * x * x * x) * 127.0);
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        M8(ctl + 2) = (uint8_t)(f < 0 ? -v : v);
        M8(ctl + 3) &= ~0x60;
        MF32(ctl + 4) = 0;
    }
    c->esp += 12; /* ret 8 */
}

/* D3D's Lock2DSurface(pixel container, level, face, locked rect, rect, flags): the CPU is about to read or write a
 * surface; once D3D has waited for the GPU, a level 0 the game rendered to is copied back from its host target
 * (render.c), which is all that was ever drawn. Data (+4) is the surface's physical address. */
void render_readback(uint32_t addr, uint32_t fmt, uint32_t size);
extern GuestFn o_003BFCA0;
void hle_Lock2DSurface(X86Ctx *c) {
    uint32_t p = ARG(0), level = ARG(1);
    o_003BFCA0(c);
    if (!level) render_readback(M32(p + 4), M32(p + 0xC), M32(p + 0x10));
}

void forza_vblank(void) {
    static uint32_t shown, vblanks;
    if (g_fps == 30) return;
    uint32_t now = nv2a_display();
    vblanks++;
    if (now != shown) { /* a new frame on screen: the next ones step as long as it took (at most 4 vblanks: loads) */
        if (shown) M32(0x554F30u) = g_vblank_hz / (vblanks < 4 ? vblanks : 4); /* 60, 120, 144: whole numbers */
        shown = now, vblanks = 0;
    }
}

/* ---------------- Recomp Settings menu ----------------
 * Main menu items after OPTIONS: ACHIEVEMENTS (page 4), RECOMP SETTINGS and QUIT GAME. RECOMP SETTINGS opens a
 * screen built like Options (0x15A870: a title, then items added by 0x199580(label, description, icon, screen factory,
 * 0)) whose rows are this port's settings and two pages of keyboard keys. A row's "factory" (called with ecx = the row's holder, which tells the rows apart) cycles its value in
 * recomp.ini, starts waiting for a new key, or opens a page; one that returns no screen is taken as a return to this
 * one. Menu text is a LocString {own text, -, string table 0x5BB308, u16 id} resolved by the table's Lookup (0x340C0)
 * whenever it is drawn, so ids from 0xF000 up are answered here, with the values as saved. */
extern GuestFn o_000340C0; /* lifted originals */
extern GuestFn o_0035B040;
extern GuestFn o_00098230;

#define STRINGS 0x5BB308u /* the UI string table */
#define MY_ID 0xF000u     /* 0: entry and title, 1: its description, then label and description per row
                           * (the game's own string tables stop at id 10010) */
#define SLOTS 256

typedef struct {
    const char *key, *label, *desc; /* descriptions get three lines of about 37 characters */
    int def, n, values[5];
    const char *names[5];
    void (*apply)(int value); /* now, or NULL: at the next start */
} Setting;
static const Setting g_settings[] = {
    {"widescreen", "WIDESCREEN", "Native 16:9 picture instead of 4:3. Takes effect the next time the game starts.",
     1, 2, {1, 0}, {"ON", "OFF"}, NULL},
    {"ultrawide", "ULTRAWIDE", "A wider view on an ultrawide screen; the HUD and menus stay 16:9. Takes effect the next time the game starts.",
     0, 2, {0, 1}, {"OFF", "ON"}, NULL},
    {"videomode", "VIDEO MODE", "Render resolution; 720p and 1080i are HD. Takes effect the next time the game starts.",
     480, 3, {480, 720, 1080}, {"480P", "720P", "1080I"}, NULL},
    {"renderscale", "RENDER SCALE", "Draws each frame this many times larger; sharper, but slower. Takes effect at the next start.",
     DEF_RENDERSCALE, 4, {1, 2, 3, 4}, {"1X", "2X", "3X", "4X"}, NULL},
    {"fullscreen", "FULLSCREEN", "Borderless: fills the whole screen, with no title bar or frame. Alt+Enter switches it too.",
     0, 2, {0, 1}, {"OFF", "ON"}, render_fullscreen},
    {"anisotropy", "TEXTURE FILTER", "Anisotropic filtering keeps the road and other textures sharp at a distance.",
     DEF_ANISOTROPY, 5, {1, 2, 4, 8, 16}, {"OFF", "2X", "4X", "8X", "16X"}, render_anisotropy},
    {"fsr", "UPSCALING", "DLSS (NVIDIA RTX) and FSR 3 rebuild the picture from several frames; FSR 1 sharpens one.",
     DEF_FSR, 4, {1, 2, 3, 0}, {"FSR 1", "DLSS", "FSR 3", "BILINEAR"}, render_fsr},
    {"framerate", "FRAME RATE", "30 as on the Xbox. Above 60, races reach as many as this PC draws (~65 now). Takes effect at the next start.",
     60, 4, {60, 120, 144, 30}, {"60", "120", "144", "30"}, NULL},
    {"prompts", "BUTTON PROMPTS", "The buttons the game shows. AUTO follows the keyboard or controller last used.",
     0, 5, {0, 1, 2, 3, 4}, {"AUTO", "KEYBOARD", "XBOX", "PLAYSTATION", "ORIGINAL"}, input_set_prompts},
    {"texpack", "TEXTURE PACK", "Replacement textures from the textures folder, such as the 4x upscaled pack.",
     1, 2, {1, 0}, {"ON", "OFF"}, render_texpack},
    {"videos", "UPSCALED VIDEOS", "The cutscenes upscaled 4x, from the videos folder, in place of the originals.",
     1, 2, {1, 0}, {"ON", "OFF"}, video_enable},
    {"enhaudio", "ENHANCED AUDIO", "Clearer, crisper sound, reverb for space, fuller bass and wider stereo. OFF: as the Xbox mixed it.",
     1, 2, {1, 0}, {"ON", "OFF"}, fx_enable},
    {"speakers", "SPEAKERS", "AUTO plays the game's 5.1 mix on 5.1 and 7.1 speakers. Takes effect at the next start.",
     0, 2, {0, 1}, {"AUTO", "STEREO"}, NULL},
    {"texdump", "TEXTURE DUMP", "Saves every new texture to textures/dump, to make a texture pack from.",
     0, 2, {0, 1}, {"OFF", "ON"}, render_texdump},
};
#define NSETTINGS (int)(sizeof g_settings / sizeof g_settings[0])

static int setting_index(int i) { /* of the saved value; the first if it is none of them */
    const Setting *s = &g_settings[i];
    int v = cfg_saved(s->key, s->def);
    for (int k = 0; k < s->n; k++)
        if (s->values[k] == v) return k;
    return 0;
}

/* screens: 0 Recomp Settings, 1 driving keys, 2 menu keys, 3 quit, 4 achievements; the rows of all of them in one list */
enum { R_SETTING, R_PAGE, R_KEY, R_QUIT, R_ACH };
typedef struct {
    int screen, kind, i, menu;
} Row;
static Row g_rows[(SLOTS - 2) / 2];
static int g_nrows;
static const char *const g_pages[5][2] = {
    {"", ""},
    {"DRIVING KEYS", "Keyboard keys for racing. The arrow keys also steer, accelerate and brake."},
    {"MENU KEYS", "Keyboard keys for the menus and the pause menu."},
    {"QUIT GAME", "Close the game and return to Windows."},
    {"ACHIEVEMENTS", "What this profile has earned, and how to earn the rest."}};
#define QUIT_ROW (NSETTINGS + 2) /* the main menu's QUIT GAME and its screen's title, listed on no screen (-1) */
#define ACH_ROW (NSETTINGS + 4)  /* the main menu's ACHIEVEMENTS, likewise */

static void build_rows(void) {
    if (g_nrows) return;
    for (int i = 0; i < NSETTINGS; i++) g_rows[g_nrows++] = (Row){0, R_SETTING, i, 0};
    g_rows[g_nrows++] = (Row){0, R_PAGE, 1, 0};
    g_rows[g_nrows++] = (Row){0, R_PAGE, 2, 0};
    g_rows[g_nrows++] = (Row){-1, R_PAGE, 3, 0};
    g_rows[g_nrows++] = (Row){3, R_QUIT, 0, 0};
    g_rows[g_nrows++] = (Row){-1, R_PAGE, 4, 0};
    for (int i = 0; i < achieve_count(); i++) g_rows[g_nrows++] = (Row){4, R_ACH, i, 0};
    for (int menu = 0; menu < 2; menu++)
        for (int k = 0; k < input_nbindings(menu); k++) g_rows[g_nrows++] = (Row){1 + menu, R_KEY, k, menu};
}

static void my_text(uint32_t k, char *t, size_t n) {
    int r = (int)(k - 2) / 2;
    build_rows();
    if (k == 0) snprintf(t, n, "RECOMP SETTINGS");
    else if (k == 1) snprintf(t, n, "Settings of this PC version: display, keyboard keys and more.");
    else if (r >= g_nrows) t[0] = 0;
    else if (g_rows[r].kind == R_SETTING) {
        const Setting *s = &g_settings[g_rows[r].i];
        if (k % 2 == 0) snprintf(t, n, "%s: %s", s->label, s->names[setting_index(g_rows[r].i)]);
        else snprintf(t, n, "%s", s->desc);
    } else if (g_rows[r].kind == R_PAGE && g_rows[r].i == 4 && k % 2 == 0)
        snprintf(t, n, "ACHIEVEMENTS: %d OF %d", achieve_earned(), achieve_count());
    else if (g_rows[r].kind == R_PAGE) snprintf(t, n, "%s", g_pages[g_rows[r].i][k % 2]);
    else if (g_rows[r].kind == R_ACH) {
        if (k % 2) snprintf(t, n, "%s%s", achieve_has(g_rows[r].i) ? "Earned. " : "", achieve_desc(g_rows[r].i));
        else {
            snprintf(t, n, "%s", achieve_name(g_rows[r].i));
            for (char *q = t; *q; q++) *q = (char)toupper((unsigned char)*q);
        }
    }
    else if (g_rows[r].kind == R_QUIT) snprintf(t, n, k % 2 ? "Closes the game. Back returns to the main menu." : "YES, QUIT");
    else if (k % 2 == 0) input_binding_text(g_rows[r].menu, g_rows[r].i, t, n);
    else snprintf(t, n, "Select, then press the new key.");
}

void hle_ForzaStringLookup(X86Ctx *c) { /* StringTable::Lookup(id) (thiscall) -> wide string */
    enum { CHARS = 256 };
    uint32_t id = ARG(0) & 0xFFFF;
    if (id < MY_ID || id >= MY_ID + SLOTS) {
        o_000340C0(c);
        return;
    }
    static uint32_t buf;
    if (!buf) buf = guest_alloc(SLOTS * CHARS * 2, 16);
    char t[CHARS];
    my_text(id - MY_ID, t, sizeof t);
    uint32_t p = buf + (id - MY_ID) * CHARS * 2;
    for (int i = 0; i == 0 || t[i - 1]; i++) M16(p + 2 * i) = (uint8_t)t[i];
    c->eax = p;
    c->esp += 8;
}

/* a LocString for id in guest scratch slot 0 or 1 (id 0: empty); the callees copy it */
static uint32_t locstr(int slot, uint32_t id) {
    static uint32_t buf;
    if (!buf) buf = guest_alloc(0x20, 16);
    uint32_t p = buf + 0x10 * slot;
    M32(p) = M32(p + 4) = 0;
    M32(p + 8) = id ? STRINGS : 0;
    M16(p + 0xC) = (uint16_t)id;
    return p;
}

static struct {
    uint32_t holder;
    int row;
} g_holders[256]; /* the item each row was added as, newest last (a screen is built anew whenever it opens) */
static int g_nholders;

/* screen->AddItem(label id, label id + 1, icon, fn) (0x199580); returns the item's holder, the vector's last entry */
static uint32_t add_item(X86Ctx *c, uint32_t screen, uint32_t label, uint32_t icon, void (*fn)(X86Ctx *c)) {
    uint32_t a[5] = {locstr(0, label), locstr(1, label + 1), icon, hle_thunk(fn), 0};
    c->ecx = screen;
    guest_call(c, 0x199580, 5, a);
    return M32(M32(screen + 0x2010) - 4);
}

static uint32_t make_screen(X86Ctx *c, int screen);

static void row_chosen(X86Ctx *c) { /* a row's factory (cdecl, no arguments; ecx = its holder) */
    uint32_t holder = c->ecx, s = 0;
    int r = -1;
    for (int k = g_nholders - 1; k >= 0 && k >= g_nholders - 256 && r < 0; k--)
        if (g_holders[k % 256].holder == holder) r = g_holders[k % 256].row;
    if (r >= 0 && g_rows[r].kind == R_SETTING) {
        const Setting *st = &g_settings[g_rows[r].i];
        int v = st->values[(setting_index(g_rows[r].i) + 1) % st->n];
        cfg_save(st->key, v);
        if (st->apply) st->apply(v);
    } else if (r >= 0 && g_rows[r].kind == R_KEY) input_capture(g_rows[r].menu, g_rows[r].i);
    else if (r >= 0 && g_rows[r].kind == R_QUIT) ExitProcess(0); /* as closing the window does */
    else if (r >= 0 && g_rows[r].kind == R_PAGE) s = make_screen(c, g_rows[r].i); /* an achievement: nothing */
    c->eax = s;
    c->esp += 4;
}

/* a new screen of the Options class with the rows of `screen` */
static uint32_t make_screen(X86Ctx *c, int screen) {
    uint32_t size = 0x2160, s = guest_call(c, 0x59520, 1, &size); /* operator new, as 0x35AE60 does */
    if (!s) return 0;
    build_rows();
    uint32_t title = MY_ID;
    for (int r = 0; r < g_nrows; r++)
        if (g_rows[r].kind == R_PAGE && g_rows[r].i == screen) title = MY_ID + 2 + 2 * r;
    uint32_t a[6] = {0, locstr(0, title), 0x4AC840 /* ico_main_options.xpr */, 0x4AC868 /* bkgd_OP_main.xpr */,
                     locstr(1, 0), 0x4A4708 /* "UIFullScreenMenu" */};
    c->ecx = s;
    guest_call(c, 0x199250, 6, a);
    M32(s) = 0x4AC7C4, M32(s + 0x28) = 0x4AC74C, M32(s + 0xE0) = 0x4AC744, M32(s + 0xE4) = 0x4AC73C;
    M8(s + 0x2158) = 0;
    for (int r = 0; r < g_nrows; r++)
        if (g_rows[r].screen == screen) {
            /* the HUD/UI icon (5); an achievement not yet earned an empty one (0: the set has no tick) */
            uint32_t icon = g_rows[r].kind == R_ACH && !achieve_has(g_rows[r].i) ? 0 : 5;
            uint32_t h = add_item(c, s, MY_ID + 2 + 2 * r, icon, row_chosen);
            g_holders[g_nholders % 256].holder = h, g_holders[g_nholders % 256].row = r, g_nholders++;
        }
    return s;
}

static void settings_screen(X86Ctx *c) { /* the main menu entry's factory */
    c->eax = make_screen(c, 0);
    c->esp += 4;
}

static void quit_screen(X86Ctx *c) {
    c->eax = make_screen(c, 3);
    c->esp += 4;
}

static void achievements_screen(X86Ctx *c) {
    c->eax = make_screen(c, 4);
    c->esp += 4;
}

void hle_ForzaMainMenu(X86Ctx *c) { /* MainMenu::MainMenu(arg) (thiscall), then our items after OPTIONS */
    o_0035B040(c);
    uint32_t screen = c->eax;
    build_rows();
    add_item(c, screen, MY_ID + 2 + 2 * ACH_ROW, 8 /* the OPTIONS icon of ico_Main.xpr */, achievements_screen);
    add_item(c, screen, MY_ID, 8, settings_screen);
    add_item(c, screen, MY_ID + 2 + 2 * QUIT_ROW, 8, quit_screen);
    c->eax = screen;
}

/* 0x98230, UIManager::Switch(old, new) (thiscall): the manager keeps the screen on top at +4. Remembered so the
 * keyboard can tell a menu (the pause menu too) from the race, whose screen is CGameWindow (vtable 0x4958E4), with
 * CCountDownWindow (0x4BCC04) over it at the start; the vtables' RTTI names them. */
static uint32_t g_ui;
void hle_ForzaScreenSwitch(X86Ctx *c) {
    g_ui = c->ecx;
    o_00098230(c);
}

/* a vtable's class from MSVC RTTI: the complete object locator sits at vtable-4, its type descriptor at +12, whose
 * name (".?AVCGameWindow@@") is at +8 */
static void class_name(uint32_t vt, char *n, int size) {
    uint32_t col = vt > 0x10004 && vt < 0x800000 ? M32(vt - 4) : 0, td = col > 0x10000 && col < 0x800000 ? M32(col + 12) : 0;
    int k = 0;
    for (uint32_t s = td > 0x10000 && td < 0x800000 ? td + 12 : 0; s && k < size - 1 && M8(s) && M8(s) != '@'; s++)
        n[k++] = (char)M8(s);
    n[k] = 0;
}

static char g_screen[64]; /* the class of the screen on top */
const char *forza_screen(void) { return g_screen; }

int forza_racing(void) { /* the race, or its countdown, on top (any thread) */
    uint32_t top = g_ui ? M32(g_ui + 4) : 0, vt = top ? M32(top) : 0;
    return vt == 0x4958E4 || vt == 0x4BCC04;
}

/* the race, its countdown, or its replay (CReplayOverlay over the race) while the replay's control bar is away: the
 * bar's top (+0x2A4, in 480 lines) slides up to 385 when shown and rests at 481 */
int forza_race_view(void) {
    uint32_t top = g_ui ? M32(g_ui + 4) : 0, vt = top ? M32(top) : 0;
    return vt == 0x4958E4 || vt == 0x4BCC04 || (vt == 0x4BEF3C && MF32(top + 0x2A4) >= 480);
}

/* the race, its countdown, or the pre-race screen over the track (CPreEventOverlay): pictures the game never keeps,
 * so a draw may go without a shader still compiling (render.c). Not replays, which may be photographed. */
int forza_throwaway_view(void) {
    uint32_t top = g_ui ? M32(g_ui + 4) : 0, vt = top ? M32(top) : 0;
    return vt == 0x4958E4 || vt == 0x4BCC04 || vt == 0x4BDD1C;
}

int forza_menu_open(void) {
    static uint32_t last = 1;
    static int log = -1;
    if (log < 0) log = getenv("UILOG") != NULL;
    uint32_t top = g_ui ? M32(g_ui + 4) : 0, vt = top ? M32(top) : 0;
    achieve_poll();
    if (top != last) {
        class_name(vt, g_screen, sizeof g_screen);
        achieve_screen(g_screen);
        if (log) {
            uint32_t live, htop, hfree, largest;
            guest_heap_stats(&live, &htop, &hfree, &largest);
            fprintf(stderr, "[ui] top screen %08X %s (vtable %08X) at %llu ms; heap live %X top %X free %X largest %X\n",
                    top, g_screen, vt, (unsigned long long)GetTickCount64(), live, htop, hfree, largest);
        }
    }
    last = top;
    return vt != 0x4958E4 && vt != 0x4BCC04;
}
