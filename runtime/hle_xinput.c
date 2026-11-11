/* Controllers: the XDK input API (XPP) replaced by host gamepads (SDL3: Xbox, PlayStation, Switch and other pads)
 * plus the keyboard.
 *
 * Guest port n is the n-th host gamepad connected; the keyboard is merged into port 0, which therefore always reports
 * a controller. Only the gamepad type is ever connected (memory units and the rest report none), so the USB stack is
 * never needed. SDL runs on its own thread, which pumps its events and copies each pad's state for the guest's polls.
 *
 * The keyboard has two sets of bindings, rebindable in Recomp Settings (recomp.ini key_<name> = virtual-key code):
 * driving keys apply while the race is on screen, menu keys while any menu is (the pause menu too), so Escape can
 * pause a race and still mean Back in menus. A key held when the set changes counts again only once released.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "xbox.h"

#define ARG(i) M32(c->esp + 4 + 4 * (i))
static void ret(X86Ctx *c, uint32_t value, int n) {
    c->eax = value;
    c->esp += 4 + 4 * n;
}

#define DEVICE_TYPE_GAMEPAD 0x47D53Cu /* &XDEVICE_TYPE_GAMEPAD in this XBE: the only type XInputOpen gets */
#define HANDLE_BASE 0xC0DE0000u        /* XInputOpen handle = HANDLE_BASE | port */
#define ERROR_DEVICE_NOT_CONNECTED_ 1167

/* Xbox XINPUT_GAMEPAD: wButtons, bAnalogButtons[8] (A B X Y Black White LT RT), sThumbLX/LY/RX/RY */
enum { XB_UP = 1, XB_DOWN = 2, XB_LEFT = 4, XB_RIGHT = 8, XB_START = 0x10, XB_BACK = 0x20, XB_LTHUMB = 0x40,
       XB_RTHUMB = 0x80 };
enum { AN_A, AN_B, AN_X, AN_Y, AN_BLACK, AN_WHITE, AN_LT, AN_RT };
typedef struct {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t thumb[4];
} Pad;

/* ---------------- host gamepads (SDL thread) ---------------- */

static SRWLOCK g_lock = SRWLOCK_INIT;
static struct {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    Pad state;
    uint16_t rumble[2]; /* left (low frequency), right motor, as the guest last set them */
} g_slot[4];
static volatile LONG g_last_device = DEV_KEYBOARD;

static int device_of(SDL_Gamepad *p) {
    switch (SDL_GetGamepadType(p)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return DEV_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return DEV_XBOX;
    default: return DEV_OTHER;
    }
}

static void read_gamepad(SDL_Gamepad *g, Pad *p) {
    static const struct { SDL_GamepadButton b; uint16_t xb; } dig[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XB_UP},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XB_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XB_LEFT},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XB_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, XB_START},      {SDL_GAMEPAD_BUTTON_BACK, XB_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XB_LTHUMB}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XB_RTHUMB}};
    /* by position, so Cross is A on a PlayStation pad; the Xbox's Black and White are the shoulder buttons */
    static const struct { SDL_GamepadButton b; int an; } ana[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, AN_A}, {SDL_GAMEPAD_BUTTON_EAST, AN_B}, {SDL_GAMEPAD_BUTTON_WEST, AN_X},
        {SDL_GAMEPAD_BUTTON_NORTH, AN_Y}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, AN_BLACK},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, AN_WHITE}};
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 8; i++)
        if (SDL_GetGamepadButton(g, dig[i].b)) p->buttons |= dig[i].xb;
    for (int i = 0; i < 6; i++)
        if (SDL_GetGamepadButton(g, ana[i].b)) p->analog[ana[i].an] = 255;
    p->analog[AN_LT] = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
    p->analog[AN_RT] = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
    int16_t ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY), ry = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    p->thumb[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    p->thumb[1] = ly == -32768 ? 32767 : -ly; /* SDL's y grows downwards, the Xbox's upwards */
    p->thumb[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    p->thumb[3] = ry == -32768 ? 32767 : -ry;
}

static DWORD WINAPI sdl_thread(void *ready) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); /* the window belongs to another thread */
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[input] SDL_Init failed: %s; keyboard only\n", SDL_GetError());
        SetEvent(ready);
        return 0;
    }
    SetEvent(ready);
    uint16_t sent[4][2] = {0};
    ULONGLONG resend[4] = {0};
    for (;;) {
        SDL_Event e;
        for (int wait = 4; SDL_WaitEventTimeout(&e, wait); wait = 0) {
            AcquireSRWLockExclusive(&g_lock);
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                int i = 0;
                while (i < 4 && g_slot[i].pad) i++;
                if (i < 4 && (g_slot[i].pad = SDL_OpenGamepad(e.gdevice.which))) {
                    g_slot[i].id = e.gdevice.which;
                    fprintf(stderr, "[input] port %d: %s\n", i, SDL_GetGamepadName(g_slot[i].pad));
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (int i = 0; i < 4; i++)
                    if (g_slot[i].pad && g_slot[i].id == e.gdevice.which) {
                        SDL_CloseGamepad(g_slot[i].pad);
                        memset(&g_slot[i], 0, sizeof g_slot[i]);
                        fprintf(stderr, "[input] port %d: disconnected\n", i);
                    }
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                       (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.value > 16000 || e.gaxis.value < -16000))) {
                SDL_Gamepad *g = SDL_GetGamepadFromID(e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.which : e.gaxis.which);
                if (g) InterlockedExchange(&g_last_device, device_of(g));
            }
            ReleaseSRWLockExclusive(&g_lock);
        }
        ULONGLONG now = GetTickCount64();
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < 4; i++) {
            if (!g_slot[i].pad) continue;
            read_gamepad(g_slot[i].pad, &g_slot[i].state);
            uint16_t *r = g_slot[i].rumble; /* Xbox rumble lasts until changed; SDL's for a duration */
            if (r[0] != sent[i][0] || r[1] != sent[i][1] || ((r[0] || r[1]) && now >= resend[i])) {
                SDL_RumbleGamepad(g_slot[i].pad, r[0], r[1], 1000);
                sent[i][0] = r[0], sent[i][1] = r[1], resend[i] = now + 500;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

static void input_start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        CreateThread(NULL, 0, sdl_thread, ready, 0, NULL);
        WaitForSingleObject(ready, 3000); /* the pads present at start, before the game first asks */
        Sleep(50);
        InitOnceComplete(&once, 0, NULL);
    }
}

/* ports with a controller: 0 always (keyboard), others while that many pads are connected */
static uint32_t connected_ports(void) {
    input_start();
    uint32_t mask = getenv("TESTPAD2") ? 3 : 1; /* testing: TESTPAD2=1 adds port 1, held by test keys + 4096 */
    AcquireSRWLockShared(&g_lock);
    for (int i = 1; i < 4; i++)
        if (g_slot[i].pad) mask |= 1u << i;
    ReleaseSRWLockShared(&g_lock);
    return mask;
}

/* ---------------- keyboard ---------------- */

enum { IN_LX_NEG, IN_LX_POS, IN_RX_NEG, IN_RX_POS, IN_LT, IN_RT, IN_A, IN_B, IN_X, IN_Y, IN_BLACK, IN_WHITE,
       IN_START, IN_BACKBTN, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT };
typedef struct {
    const char *key, *label;
    int menu, in, def, alt; /* alt: a second key that is not rebindable (arrows beside WASD and the like) */
} Binding;
static const Binding g_bind[] = {
    {"key_accelerate", "ACCELERATE", 0, IN_RT, 'W', VK_UP},
    {"key_brake", "BRAKE/REVERSE", 0, IN_LT, 'S', VK_DOWN},
    {"key_steer_left", "STEER LEFT", 0, IN_LX_NEG, 'A', VK_LEFT},
    {"key_steer_right", "STEER RIGHT", 0, IN_LX_POS, 'D', VK_RIGHT},
    {"key_ebrake", "E-BRAKE", 0, IN_A, VK_SPACE, 0},
    {"key_gear_up", "GEAR UP", 0, IN_B, 'E', 0},
    {"key_gear_down", "GEAR DOWN", 0, IN_X, 'Q', 0},
    {"key_look_back", "LOOK BACK", 0, IN_Y, 'C', 0},
    {"key_look_left", "LOOK LEFT", 0, IN_RX_NEG, 'Z', 0},
    {"key_look_right", "LOOK RIGHT", 0, IN_RX_POS, 'X', 0},
    {"key_camera", "CAMERA", 0, IN_BLACK, 'V', 0},
    {"key_hud", "HUD", 0, IN_WHITE, 'H', 0},
    {"key_pause", "PAUSE", 0, IN_START, VK_ESCAPE, 0},
    {"key_select", "SELECT", 1, IN_A, VK_RETURN, VK_SPACE},
    {"key_back", "BACK", 1, IN_B, VK_ESCAPE, 0},
    {"key_menu_up", "UP", 1, IN_UP, VK_UP, 'W'},
    {"key_menu_down", "DOWN", 1, IN_DOWN, VK_DOWN, 'S'},
    {"key_menu_left", "LEFT", 1, IN_LEFT, VK_LEFT, 'A'},
    {"key_menu_right", "RIGHT", 1, IN_RIGHT, VK_RIGHT, 'D'},
    {"key_menu_x", "X BUTTON", 1, IN_X, 'X', 0},
    {"key_menu_y", "Y BUTTON", 1, IN_Y, 'Y', 0},
    {"key_menu_black", "BLACK BUTTON", 1, IN_BLACK, 'E', 0},
    {"key_menu_white", "WHITE BUTTON", 1, IN_WHITE, 'Q', 0},
    {"key_menu_lt", "LEFT TRIGGER", 1, IN_LT, VK_PRIOR, 0},
    {"key_menu_rt", "RIGHT TRIGGER", 1, IN_RT, VK_NEXT, 0},
    {"key_menu_start", "START BUTTON", 1, IN_START, VK_TAB, 0},
    {"key_menu_backbutton", "BACK BUTTON", 1, IN_BACKBTN, VK_BACK, 0},
};
#define NBIND (int)(sizeof g_bind / sizeof g_bind[0])
static int g_key[NBIND]; /* as saved */
static volatile LONG g_capture = -1; /* binding waiting for its new key */
static volatile LONG g_rebinds;      /* times a binding changed, for the key prompts (icons.c) */
static uint8_t g_stale[256];         /* held across a change of set or a capture: ignored until released */

static void load_keys(void) {
    static int loaded;
    if (loaded) return;
    loaded = 1;
    int test = getenv("TESTKEYS") != NULL; /* test scripts press the default keys, whatever the player bound */
    for (int i = 0; i < NBIND; i++) g_key[i] = (test ? g_bind[i].def : cfg_saved(g_bind[i].key, g_bind[i].def)) & 0xFF;
}

int input_nbindings(int menu) {
    int n = 0;
    for (int i = 0; i < NBIND; i++) n += g_bind[i].menu == menu;
    return n;
}

static int binding(int menu, int k) { /* the k-th binding of a set */
    for (int i = 0; i < NBIND; i++)
        if (g_bind[i].menu == menu && k-- == 0) return i;
    return -1;
}

static void key_name(int vk, char *t, size_t n) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    int ext = vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_INSERT || vk == VK_DELETE ||
              vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_RCONTROL || vk == VK_RMENU;
    if (!vk || !GetKeyNameTextA((LONG)(sc << 16 | (ext ? 1 << 24 : 0)), t, (int)n)) snprintf(t, n, vk ? "KEY %d" : "NONE", vk);
    for (char *p = t; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
}

/* "ACCELERATE: W", or the prompt while it waits for a key */
void input_binding_text(int menu, int k, char *t, size_t n) {
    int i = binding(menu, k);
    char name[32];
    load_keys();
    if (i < 0) {
        t[0] = 0;
        return;
    }
    if (g_capture == i) snprintf(name, sizeof name, "PRESS A KEY");
    else key_name(g_key[i], name, sizeof name);
    snprintf(t, n, "%s: %s", g_bind[i].label, name);
}

void input_capture(int menu, int k) {
    load_keys();
    InterlockedExchange(&g_capture, binding(menu, k));
}

int input_last_device(void) { return g_last_device; }
static int g_prompts = -1; /* BUTTON PROMPTS: 0 follow the device last used, else that DEV_* + 1 */
void input_set_prompts(int v) {
    g_prompts = v;
    InterlockedIncrement(&g_rebinds);
}
int input_icon_state(void) {
    if (g_prompts < 0) g_prompts = cfg_saved("prompts", 0);
    return (g_prompts > 0 && g_prompts <= 4 ? g_prompts - 1 : g_last_device) | g_rebinds << 4;
}

/* a prompt's key cap: the name of the menu key standing in for a button (BTN_*), three characters at most, since
 * the glyph is a square the size of a letter */
void icons_key_name(int button, wchar_t *t, int n) {
    static const int in[BTN_COUNT] = {[BTN_A] = IN_A, [BTN_B] = IN_B, [BTN_X] = IN_X, [BTN_Y] = IN_Y, [BTN_LT] = IN_LT,
                                      [BTN_RT] = IN_RT, [BTN_BLACK] = IN_BLACK, [BTN_WHITE] = IN_WHITE,
                                      [BTN_START] = IN_START, [BTN_BACK] = IN_BACKBTN, [BTN_LEFT] = IN_LEFT,
                                      [BTN_DOWN] = IN_DOWN, [BTN_RIGHT] = IN_RIGHT, [BTN_UP] = IN_UP};
    int vk = 0;
    load_keys();
    for (int i = 0; i < NBIND && button >= 0 && button < BTN_COUNT; i++)
        if (g_bind[i].menu && g_bind[i].in == in[button]) vk = g_key[i];
    static const struct { int vk; const wchar_t *name; } short_names[] = {
        {VK_RETURN, L"\x21B5"}, {VK_ESCAPE, L"ESC"}, {VK_SPACE, L"SPC"}, {VK_BACK, L"\x232B"}, {VK_TAB, L"TAB"},
        {VK_UP, L"\x2191"}, {VK_DOWN, L"\x2193"}, {VK_LEFT, L"\x2190"}, {VK_RIGHT, L"\x2192"}, {VK_PRIOR, L"PG\x2191"},
        {VK_NEXT, L"PG\x2193"}, {VK_LSHIFT, L"\x21E7"}, {VK_RSHIFT, L"\x21E7"}, {VK_LCONTROL, L"CTL"},
        {VK_RCONTROL, L"CTL"}, {VK_LMENU, L"ALT"}, {VK_RMENU, L"ALT"}, {VK_DELETE, L"DEL"}, {VK_INSERT, L"INS"},
        {VK_HOME, L"HOM"}, {VK_END, L"END"}, {0, L"?"}};
    for (int i = 0; i < (int)(sizeof short_names / sizeof short_names[0]); i++)
        if (short_names[i].vk == vk) {
            wcsncpy(t, short_names[i].name, n - 1), t[n - 1] = 0;
            return;
        }
    char name[32];
    key_name(vk, name, sizeof name);
    MultiByteToWideChar(CP_ACP, 0, name, -1, t, n);
    if (wcslen(t) > 3) t[3] = 0;
}

static int our_window_focused(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void press(Pad *p, int in) {
    static const int an[] = {[IN_LT] = AN_LT, [IN_RT] = AN_RT, [IN_A] = AN_A, [IN_B] = AN_B, [IN_X] = AN_X,
                             [IN_Y] = AN_Y, [IN_BLACK] = AN_BLACK, [IN_WHITE] = AN_WHITE};
    static const uint16_t dig[] = {[IN_START] = XB_START, [IN_BACKBTN] = XB_BACK, [IN_UP] = XB_UP, [IN_DOWN] = XB_DOWN,
                                   [IN_LEFT] = XB_LEFT, [IN_RIGHT] = XB_RIGHT};
    switch (in) {
    case IN_LX_NEG: p->thumb[0] = -32768; break;
    case IN_LX_POS: p->thumb[0] = 32767; break;
    case IN_RX_NEG: p->thumb[2] = -32768; break;
    case IN_RX_POS: p->thumb[2] = 32767; break;
    case IN_START: case IN_BACKBTN: case IN_UP: case IN_DOWN: case IN_LEFT: case IN_RIGHT: p->buttons |= dig[in]; break;
    default: p->analog[an[in]] = 255;
    }
}

/* INPUTLOG=1: the test keys and the menu/driving switches as they change */
static int ktrace_input(void) {
    static int log = -1;
    if (log < 0) log = getenv("INPUTLOG") != NULL;
    return log;
}

/* TESTKEYS=<file>: test scripts hold keys by listing their virtual-key codes in the file, one a line, and the real
 * keyboard is ignored; the game needs no focus then, so a script leaves the desktop to whoever is using it */
static int g_test2[256]; /* TESTPAD2: keys a script holds for port 1 (vk + 4096 in the file) */
static int test_keys(int down[256]) {
    static const char *path;
    static int init, last[256];
    if (!init) path = getenv("TESTKEYS"), init = 1;
    if (!path) return 0;
    /* shared for delete, so a script can replace the file while it is open here; one that will not open keeps the
     * keys as they were, or a held key would read as released and pressed again */
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char text[1024];
        DWORD got = 0;
        ReadFile(h, text, sizeof text - 1, &got, NULL);
        CloseHandle(h);
        text[got] = 0;
        int now[256] = {0}, vk, used;
        memset(g_test2, 0, sizeof g_test2);
        for (const char *t = text; sscanf(t, "%d%n", &vk, &used) == 1; t += used)
            if (vk > 0 && vk < 256) now[vk] = 1;
            else if (vk > 4096 && vk < 4096 + 256) g_test2[vk - 4096] = 1;
        if (memcmp(now, last, sizeof now) && ktrace_input()) {
            fprintf(stderr, "[input] test keys:");
            for (int k = 0; k < 256; k++)
                if (now[k]) fprintf(stderr, " %d", k);
            fprintf(stderr, "\n");
        }
        memcpy(last, now, sizeof now);
    }
    memcpy(down, last, sizeof last);
    return 1;
}

/* the keyboard's part of port 0 */
static void read_keyboard(Pad *p) {
    static int last_menu = -1, last_capture = -1;
    load_keys();
    int down[256], test = test_keys(down);
    int menu = forza_menu_open(), capture = g_capture; /* first: it also follows the screens (achievements) */
    if (!test && !our_window_focused()) return;
    for (int vk = 8; vk < 256; vk++) {
        if (!test) down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!down[vk]) g_stale[vk] = 0;
    }
    if (menu != last_menu || capture != last_capture) /* keys held now belonged to the other set, or the capture */
        for (int vk = 8; vk < 256; vk++) g_stale[vk] |= down[vk];
    if (menu != last_menu && ktrace_input()) fprintf(stderr, "[input] %s keys\n", menu ? "menu" : "driving");
    last_menu = menu, last_capture = capture;
    if (capture >= 0) {
        for (int vk = 8; vk < 256; vk++) /* the plain keys only: Shift/Ctrl/Alt report as their left/right halves */
            if (down[vk] && !g_stale[vk] && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN &&
                vk != VK_RWIN) {
                g_key[capture] = vk;
                cfg_save(g_bind[capture].key, vk);
                InterlockedIncrement(&g_rebinds);
                InterlockedExchange(&g_capture, -1);
                break;
            }
        return;
    }
    for (int i = 0; i < NBIND; i++) {
        if (g_bind[i].menu != menu) continue;
        int k = g_key[i], a = g_bind[i].alt;
        if ((k && down[k] && !g_stale[k]) || (a && down[a] && !g_stale[a])) {
            press(p, g_bind[i].in);
            InterlockedExchange(&g_last_device, DEV_KEYBOARD);
        }
    }
}

static void read_pad(uint32_t port, Pad *p) {
    AcquireSRWLockShared(&g_lock);
    if (g_slot[port].pad) *p = g_slot[port].state;
    else memset(p, 0, sizeof *p);
    ReleaseSRWLockShared(&g_lock);
    if (port == 0) read_keyboard(p);
    if (port == 1 && !g_slot[1].pad && getenv("TESTPAD2")) { /* a script's second player: Enter/Space A, Esc B, arrows,
                                                                 * Tab Start, W/S triggers, A/D steer */
        const int *k = g_test2;
        p->analog[AN_A] = k[13] || k[32] ? 255 : 0, p->analog[AN_B] = k[27] ? 255 : 0;
        p->analog[AN_RT] = k['W'] ? 255 : 0, p->analog[AN_LT] = k['S'] ? 255 : 0;
        p->buttons = (k[38] ? XB_UP : 0) | (k[40] ? XB_DOWN : 0) | (k[37] ? XB_LEFT : 0) | (k[39] ? XB_RIGHT : 0) |
                     (k[9] ? XB_START : 0);
        p->thumb[0] = k['A'] ? -32767 : k['D'] ? 32767 : 0;
    }
}

/* ---------------- XPP replacements ---------------- */

static uint32_t g_reported; /* gamepad ports last reported through XGetDevices / XGetDeviceChanges */
static uint32_t g_packet[4];
static Pad g_last[4];

/* XInitDevices(dwPreallocTypeCount, PreallocTypes) */
void hle_XInitDevices(X86Ctx *c) {
    input_start();
    ret(c, 0, 2);
}

/* XGetDevices(DeviceType): bitmask of ports with that device */
void hle_XGetDevices(X86Ctx *c) {
    uint32_t mask = ARG(0) == DEVICE_TYPE_GAMEPAD ? (g_reported = connected_ports()) : 0;
    ret(c, mask, 1);
}

/* XGetDeviceChanges(DeviceType, pdwInsertions, pdwRemovals): TRUE when something changed since last asked */
void hle_XGetDeviceChanges(X86Ctx *c) {
    uint32_t ins = 0, rem = 0;
    if (ARG(0) == DEVICE_TYPE_GAMEPAD) {
        uint32_t now = connected_ports();
        ins = now & ~g_reported, rem = g_reported & ~now;
        g_reported = now;
    }
    M32(ARG(1)) = ins, M32(ARG(2)) = rem;
    ret(c, (ins | rem) != 0, 3);
}

/* XInputOpen(DeviceType, dwPort, dwSlot, pPollingParameters) */
void hle_XInputOpen(X86Ctx *c) {
    uint32_t port = ARG(1);
    int ok = ARG(0) == DEVICE_TYPE_GAMEPAD && port < 4 && (connected_ports() >> port & 1);
    if (ktrace() || ktrace_input()) fprintf(stderr, "[xinput] XInputOpen port %u -> %s\n", port, ok ? "ok" : "none");
    ret(c, ok ? HANDLE_BASE | port : 0, 4);
}

void hle_XInputClose(X86Ctx *c) { ret(c, 0, 1); }

static int port_of(uint32_t h) { return (h & ~3u) == HANDLE_BASE ? (int)(h & 3) : -1; }

/* XInputGetCapabilities(hDevice, pCapabilities): SubType, Reserved, In.Gamepad (every input), Out.Rumble */
void hle_XInputGetCapabilities(X86Ctx *c) {
    int port = port_of(ARG(0));
    uint32_t p = ARG(1);
    if (port < 0) {
        ret(c, ERROR_DEVICE_NOT_CONNECTED_, 2);
        return;
    }
    memset(g_mem + p, 0, 26);
    M8(p) = 1; /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    memset(g_mem + p + 4, 0xFF, 18);
    M16(p + 22) = M16(p + 24) = 0xFFFF;
    ret(c, ERROR_SUCCESS, 2);
}

/* XInputGetState(hDevice, pState): dwPacketNumber, XINPUT_GAMEPAD */
void hle_XInputGetState(X86Ctx *c) {
    int port = port_of(ARG(0));
    uint32_t s = ARG(1);
    if (port < 0 || !(connected_ports() >> port & 1)) {
        ret(c, ERROR_DEVICE_NOT_CONNECTED_, 2);
        return;
    }
    Pad p;
    read_pad(port, &p);
    if (memcmp(&p, &g_last[port], sizeof p)) {
        if (ktrace_input())
            fprintf(stderr, "[input] port %d packet %u: buttons %04X A %u B %u\n", port, g_packet[port] + 1, p.buttons,
                    p.analog[AN_A], p.analog[AN_B]);
        g_last[port] = p, g_packet[port]++;
    }
    M32(s) = g_packet[port];
    M16(s + 4) = p.buttons;
    memcpy(g_mem + s + 6, p.analog, 8);
    memcpy(g_mem + s + 14, p.thumb, 8);
    ret(c, ERROR_SUCCESS, 2);
}

/* XInputSetState(hDevice, pFeedback): XINPUT_FEEDBACK_HEADER (dwStatus, hEvent, 58 reserved) then Rumble.
 * Handed to the SDL thread and completed at once; ERROR_IO_PENDING is what a queued request returns. */
void hle_XInputSetState(X86Ctx *c) {
    int port = port_of(ARG(0));
    uint32_t f = ARG(1);
    if (port < 0) {
        ret(c, ERROR_DEVICE_NOT_CONNECTED_, 2);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_slot[port].rumble[0] = M16(f + 0x42), g_slot[port].rumble[1] = M16(f + 0x44);
    ReleaseSRWLockExclusive(&g_lock);
    M32(f) = ERROR_SUCCESS;
    if (M32(f + 4)) M32(M32(f + 4) + 4) = 1; /* signal hEvent (handle = KEVENT) */
    ret(c, ERROR_IO_PENDING, 2);
}
