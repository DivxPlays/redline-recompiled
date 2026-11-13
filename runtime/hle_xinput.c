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
    thread_name("input");
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
       IN_START, IN_BACKBTN, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_RY_NEG, IN_RY_POS };
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

/* The driving keys press what the game's controller layout (Options > Controller Options) has for their action, so
 * E-BRAKE, GEAR UP and the rest do that in every layout; g_bind's are the Default Layout's. The game keeps a record
 * per controller at 0x51C0D4 + 12 * n {?, layout, vibration}; the keyboard is controller 1. Its 7 layouts, as their
 * diagrams show them: Default, Shift on Triggers, Accel on Thumbstick, Shift on Thumbstick, Shift on A/B Buttons,
 * MadCatz Wheel, Speedster Wheel. */
#define LAYOUT 0x51C0D8u
static const uint8_t g_layout_bind[] = {0, 1, 4, 5, 6, 8, 9}; /* accelerate, brake, e-brake, gear up, gear down,
                                                                 * look left, look right */
static const uint8_t g_layout[7][7] = {
    {IN_RT, IN_LT, IN_A, IN_B, IN_X, IN_RX_NEG, IN_RX_POS},
    {IN_A, IN_X, IN_B, IN_RT, IN_LT, IN_RX_NEG, IN_RX_POS},
    {IN_RT, IN_LT, IN_A, IN_B, IN_X, IN_RX_NEG, IN_RX_POS},
    {IN_RT, IN_LT, IN_A, IN_RY_POS, IN_RY_NEG, IN_X, IN_B},
    {IN_RT, IN_LT, IN_X, IN_A, IN_B, IN_RX_NEG, IN_RX_POS},
    {IN_RT, IN_LT, IN_B, IN_X, IN_A, IN_RX_NEG, IN_RX_POS},
    {IN_RT, IN_LT, IN_B, IN_A, IN_X, IN_RX_NEG, IN_RX_POS},
};
static int driving_in(uint32_t port, int i) { /* g_bind[i]'s action in controller port + 1's layout */
    uint32_t layout = M32(LAYOUT + 12 * port);
    for (int k = 0; k < (int)sizeof g_layout_bind && layout < 7; k++)
        if (g_layout_bind[k] == i) return g_layout[layout][k];
    return g_bind[i].in;
}

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

/* an input at strength v (0-255: an analog button's pressure, a stick's tilt) */
static void put(Pad *p, int in, int v) {
    static const int an[] = {[IN_LT] = AN_LT, [IN_RT] = AN_RT, [IN_A] = AN_A, [IN_B] = AN_B, [IN_X] = AN_X,
                             [IN_Y] = AN_Y, [IN_BLACK] = AN_BLACK, [IN_WHITE] = AN_WHITE};
    static const uint16_t dig[] = {[IN_START] = XB_START, [IN_BACKBTN] = XB_BACK, [IN_UP] = XB_UP, [IN_DOWN] = XB_DOWN,
                                   [IN_LEFT] = XB_LEFT, [IN_RIGHT] = XB_RIGHT};
    int16_t neg = (int16_t)(-32768 * v / 255), pos = (int16_t)(32767 * v / 255);
    if (v <= 0) return;
    switch (in) {
    case IN_LX_NEG: p->thumb[0] = neg; break;
    case IN_LX_POS: p->thumb[0] = pos; break;
    case IN_RX_NEG: p->thumb[2] = neg; break;
    case IN_RX_POS: p->thumb[2] = pos; break;
    case IN_RY_NEG: p->thumb[3] = neg; break;
    case IN_RY_POS: p->thumb[3] = pos; break;
    case IN_START: case IN_BACKBTN: case IN_UP: case IN_DOWN: case IN_LEFT: case IN_RIGHT:
        if (v >= 128) p->buttons |= dig[in];
        break;
    default:
        if (v > p->analog[an[in]]) p->analog[an[in]] = (uint8_t)v;
    }
}
static void press(Pad *p, int in) { put(p, in, 255); }

/* INPUTLOG=1: the test keys and the menu/driving switches as they change */
static int ktrace_input(void) {
    static int log = -1;
    if (log < 0) log = getenv("INPUTLOG") != NULL;
    return log;
}

/* TESTKEYS=<file>: test scripts hold keys by listing their virtual-key codes in the file, one a line, and the real
 * keyboard is ignored; the game needs no focus then, so a script leaves the desktop to whoever is using it */
static int g_test2[256]; /* TESTPAD2: keys a script holds for port 1 (vk + 4096 in the file) */
static int g_testpad0;   /* a pad's buttons a script holds on port 0: 8193 RB, 8194 LB, 8195 R3, 8196 L3, 8197 A,
                          * 8198 B, 8199 X, 8200 Y, 8201 RT, 8202 LT, 8203 Start */
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
        int pad0 = 0;
        for (const char *t = text; sscanf(t, "%d%n", &vk, &used) == 1; t += used)
            if (vk > 0 && vk < 256) now[vk] = 1;
            else if (vk > 4096 && vk < 4096 + 256) g_test2[vk - 4096] = 1;
            else if (vk > 8192 && vk <= 8203) pad0 |= 1 << (vk - 8193);
        g_testpad0 = pad0;
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

static volatile LONG g_pad_capture; /* CONTROLLER BUTTONS, below */

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
            InterlockedExchange(&g_pad_capture, -1); /* the keyboard gives up a wait for a controller button */
            press(p, menu ? g_bind[i].in : driving_in(0, i));
            InterlockedExchange(&g_last_device, DEV_KEYBOARD);
        }
    }
}

/* SHIFT ON SHOULDERS, in a race: RB and LB (the Xbox's Black and White) press what the controller's layout has for gear
 * up and down; the camera and HUD they were move to the right and left stick clicks */
static int g_shoulders = -1;
void input_set_shoulders(int on) { g_shoulders = on; }
static void shoulder_shift(uint32_t port, Pad *p) {
    int up = p->analog[AN_BLACK], down = p->analog[AN_WHITE];
    p->analog[AN_BLACK] = p->buttons & XB_RTHUMB ? 255 : 0, p->analog[AN_WHITE] = p->buttons & XB_LTHUMB ? 255 : 0;
    p->buttons &= ~(XB_RTHUMB | XB_LTHUMB);
    if (up) press(p, driving_in(port, 5));   /* g_bind: gear up */
    if (down) press(p, driving_in(port, 6)); /* gear down */
}

static int shoulders_on(void) {
    if (g_shoulders < 0 && (g_shoulders = cfg_get("SHOULDERS", "shoulders", 0)))
        fprintf(stderr, "[input] shift on shoulders\n");
    return g_shoulders;
}

/* CONTROLLER BUTTONS (Recomp Settings): a controller's buttons for the racing actions. Until one is changed, buttons
 * reach the game by position, and its Controller Options layout (and SHIFT ON SHOULDERS) decide what they do. Once
 * one is (recomp.ini pad_custom = 1 and pad_<action> = a source below, -1 none, all saved together), in a race each
 * action's button presses what that controller's layout has for the action, so the choice holds in every layout. The
 * face buttons, shoulders, triggers and stick clicks then do only what they are bound to; the d-pad, Back and the
 * right stick's axes keep their own job unless bound. Menus always get the buttons by position. */
enum { S_A, S_B, S_X, S_Y, S_RB, S_LB, S_RT, S_LT, S_R3, S_L3, S_RS_UP, S_RS_DOWN, S_RS_LEFT, S_RS_RIGHT, S_UP, S_DOWN,
       S_LEFT, S_RIGHT, S_BACK, NSRC, S_START = 31 /* cancels a capture; never bound */ };
static const char *const g_src_xbox[NSRC] = {"A", "B", "X", "Y", "RB", "LB", "RT", "LT", "RS CLICK", "LS CLICK", "RS UP",
                                             "RS DOWN", "RS LEFT", "RS RIGHT", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT",
                                             "D-PAD RIGHT", "BACK"};
static const char *const g_src_ps[S_L3 + 1] = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "R1", "L1", "R2", "L2", "R3", "L3"};
static const int g_src_an[S_R3] = {AN_A, AN_B, AN_X, AN_Y, AN_BLACK, AN_WHITE, AN_RT, AN_LT};
static const uint16_t g_src_bit[NSRC] = {[S_R3] = XB_RTHUMB, [S_L3] = XB_LTHUMB, [S_UP] = XB_UP, [S_DOWN] = XB_DOWN,
                                         [S_LEFT] = XB_LEFT, [S_RIGHT] = XB_RIGHT, [S_BACK] = XB_BACK};
static const uint8_t g_act[] = {0, 1, 4, 5, 6, 7, 8, 9, 10, 11}; /* g_bind's driving actions a button can do */
#define NACT (int)sizeof g_act
static int g_pad[NACT];                  /* each action's source, as saved */
static int g_pad_custom = -1;            /* the bindings above apply (else the game's layout) */
static volatile LONG g_pad_capture = -1; /* the action waiting for its button */
static volatile LONG g_pad_serial;       /* captures begun */
static ULONGLONG g_pad_until;            /* when the wait gives up */
static uint32_t g_pad_mute[4];           /* sources held when a capture began or ended: unseen until released */

static int src_value(const Pad *p, int s) { /* 0-255 */
    int t;
    switch (s) {
    case S_RS_UP: t = p->thumb[3]; break;
    case S_RS_DOWN: t = -p->thumb[3]; break;
    case S_RS_LEFT: t = -p->thumb[2]; break;
    case S_RS_RIGHT: t = p->thumb[2]; break;
    case S_START: return p->buttons & XB_START ? 255 : 0;
    default: return s < S_R3 ? p->analog[g_src_an[s]] : p->buttons & g_src_bit[s] ? 255 : 0;
    }
    return t <= 0 ? 0 : t >= 32767 ? 255 : t * 255 / 32767;
}

static void src_clear(Pad *p, int s) {
    if (s < S_R3) p->analog[g_src_an[s]] = 0;
    else if (s == S_RS_UP || s == S_RS_DOWN) p->thumb[3] = 0;
    else if (s == S_RS_LEFT || s == S_RS_RIGHT) p->thumb[2] = 0;
    else if (s == S_START) p->buttons &= ~XB_START;
    else p->buttons &= ~g_src_bit[s];
}

static uint32_t src_held(const Pad *p) {
    uint32_t m = src_value(p, S_START) ? 1u << S_START : 0;
    for (int s = 0; s < NSRC; s++)
        if (src_value(p, s) > 160) m |= 1u << s;
    return m;
}

static const char *pad_key(int a) { /* "pad_" and the keyboard binding's name: pad_gear_up */
    static char k[NACT][40];
    snprintf(k[a], sizeof k[a], "pad_%s", g_bind[g_act[a]].key + 4);
    return k[a];
}

static int pad_custom(void) {
    if (g_pad_custom < 0) {
        for (int a = 0; a < NACT; a++) {
            g_pad[a] = cfg_saved(pad_key(a), -1);
            if (g_pad[a] >= NSRC) g_pad[a] = -1;
        }
        g_pad_custom = cfg_saved("pad_custom", 0) == 1;
    }
    return g_pad_custom;
}

/* the button controller 1's layout (with SHIFT ON SHOULDERS) has action a on: what the page shows until one changes */
static int layout_source(int a) {
    int i = g_act[a];
    if (shoulders_on() && (i == 5 || i == 6 || i == 10 || i == 11))
        return i == 5 ? S_RB : i == 6 ? S_LB : i == 10 ? S_R3 : S_L3;
    switch (driving_in(0, i)) {
    case IN_A: return S_A;
    case IN_B: return S_B;
    case IN_X: return S_X;
    case IN_Y: return S_Y;
    case IN_BLACK: return S_RB;
    case IN_WHITE: return S_LB;
    case IN_RT: return S_RT;
    case IN_LT: return S_LT;
    case IN_RX_NEG: return S_RS_LEFT;
    case IN_RX_POS: return S_RS_RIGHT;
    case IN_RY_POS: return S_RS_UP;
    case IN_RY_NEG: return S_RS_DOWN;
    }
    return -1;
}

int input_pad_actions(void) { return NACT; }
int input_pad_is_custom(void) { return pad_custom(); }

/* "GEAR UP: RB", by the names of the pad last used, or the prompt while it waits */
void input_pad_text(int a, char *t, size_t n) {
    if (a < 0 || a >= NACT) {
        t[0] = 0;
        return;
    }
    int s = pad_custom() ? g_pad[a] : layout_source(a);
    const char *name = g_pad_capture == a                         ? "?"
                       : s < 0                                     ? "NONE"
                       : g_last_device == DEV_PLAYSTATION && s <= S_L3 ? g_src_ps[s]
                                                                      : g_src_xbox[s];
    snprintf(t, n, "%s: %s", g_bind[g_act[a]].label, name);
}

void input_pad_capture(int a) {
    pad_custom();
    g_pad_until = GetTickCount64() + 5000;
    InterlockedIncrement(&g_pad_serial);
    InterlockedExchange(&g_pad_capture, a);
}

void input_pad_reset(void) {
    pad_custom();
    cfg_save("pad_custom", 0), g_pad_custom = 0;
}

/* while a capture waits, a press on any controller (not held when it began) becomes the action's button; the game
 * sees nothing of the controllers meanwhile. Start, or 5 s, gives up. */
static void pad_capture(uint32_t port, Pad *p) {
    static LONG seen[4];
    uint32_t held = src_held(p);
    g_pad_mute[port] &= held;
    int a = g_pad_capture;
    if (a < 0) return;
    if (seen[port] != g_pad_serial) seen[port] = g_pad_serial, g_pad_mute[port] |= held;
    uint32_t fresh = held & ~g_pad_mute[port];
    if (fresh >> S_START & 1 || GetTickCount64() > g_pad_until) InterlockedExchange(&g_pad_capture, -1);
    else if (fresh) {
        int s = 0;
        while (!(fresh >> s & 1)) s++;
        if (!pad_custom())
            for (int k = 0; k < NACT; k++) g_pad[k] = layout_source(k); /* from what was in use */
        for (int k = 0; k < NACT; k++)
            if (g_pad[k] == s) g_pad[k] = -1; /* a button does one thing: the action it had loses it */
        g_pad[a] = s;
        for (int k = 0; k < NACT; k++) cfg_save(pad_key(k), g_pad[k]);
        cfg_save("pad_custom", 1), g_pad_custom = 1;
        fprintf(stderr, "[input] %s on %s\n", g_bind[g_act[a]].label, g_src_xbox[s]);
        InterlockedExchange(&g_pad_capture, -1);
    }
    g_pad_mute[port] |= held;
    memset(p, 0, sizeof *p);
}

/* in a race with the player's buttons: each action from its button, through the controller's layout */
static void pad_actions(uint32_t port, Pad *p) {
    Pad raw = *p;
    uint32_t bound = 0;
    for (int a = 0; a < NACT; a++)
        if (g_pad[a] >= 0) bound |= 1u << g_pad[a];
    for (int s = 0; s < NSRC; s++)
        if (s <= S_L3 || bound >> s & 1) src_clear(p, s);
    for (int a = 0; a < NACT; a++)
        if (g_pad[a] >= 0) put(p, driving_in(port, g_act[a]), src_value(&raw, g_pad[a]));
}

static void read_pad(uint32_t port, Pad *p) {
    AcquireSRWLockShared(&g_lock);
    int pad = g_slot[port].pad != NULL;
    if (pad) *p = g_slot[port].state;
    else memset(p, 0, sizeof *p);
    ReleaseSRWLockShared(&g_lock);
    static int testing = -1;
    if (testing < 0) testing = getenv("TESTKEYS") != NULL;
    if (port == 0 && testing) { /* testing: a pad's buttons held by a script (TESTKEYS), over a real one's or none */
        static const int an[] = {AN_BLACK, AN_WHITE, -1, -1, AN_A, AN_B, AN_X, AN_Y, AN_RT, AN_LT};
        for (int b = 0; b < 10; b++)
            if (g_testpad0 >> b & 1 && an[b] >= 0) p->analog[an[b]] = 255;
        p->buttons |= (g_testpad0 & 4 ? XB_RTHUMB : 0) | (g_testpad0 & 8 ? XB_LTHUMB : 0) |
                      (g_testpad0 & 1024 ? XB_START : 0);
        pad = 1;
    }
    if (pad) {
        pad_capture(port, p);
        for (int s = 0; s < 32; s++)
            if ((s < NSRC || s == S_START) && g_pad_mute[port] >> s & 1) src_clear(p, s);
        if (forza_racing()) {
            if (pad_custom()) pad_actions(port, p);
            else if (shoulders_on()) shoulder_shift(port, p);
        }
    }
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
            fprintf(stderr, "[input] port %d packet %u: buttons %04X A %u B %u X %u Y %u black %u white %u LT %u RT %u "
                    "RS %d,%d\n", port, g_packet[port] + 1, p.buttons, p.analog[AN_A], p.analog[AN_B], p.analog[AN_X],
                    p.analog[AN_Y], p.analog[AN_BLACK], p.analog[AN_WHITE], p.analog[AN_LT], p.analog[AN_RT], p.thumb[2],
                    p.thumb[3]);
        g_last[port] = p, g_packet[port]++;
    }
    M32(s) = g_packet[port];
    M16(s + 4) = p.buttons;
    memcpy(g_mem + s + 6, p.analog, 8);
    memcpy(g_mem + s + 14, p.thumb, 8);
    ret(c, ERROR_SUCCESS, 2);
}

static int g_rumble = -1; /* RUMBLE STRENGTH: percent of the game's motor speeds */
void input_set_rumble(int percent) { g_rumble = percent; }

/* XInputSetState(hDevice, pFeedback): XINPUT_FEEDBACK_HEADER (dwStatus, hEvent, 58 reserved) then Rumble.
 * Handed to the SDL thread and completed at once; ERROR_IO_PENDING is what a queued request returns. */
void hle_XInputSetState(X86Ctx *c) {
    int port = port_of(ARG(0));
    uint32_t f = ARG(1);
    if (port < 0) {
        ret(c, ERROR_DEVICE_NOT_CONNECTED_, 2);
        return;
    }
    if (g_rumble < 0) g_rumble = cfg_get("RUMBLE", "rumble", 100);
    uint16_t lo = (uint16_t)(M16(f + 0x42) * g_rumble / 100), hi = (uint16_t)(M16(f + 0x44) * g_rumble / 100);
    AcquireSRWLockExclusive(&g_lock);
    if (ktrace_input() && (lo != g_slot[port].rumble[0] || hi != g_slot[port].rumble[1]))
        fprintf(stderr, "[input] port %d rumble %u %u (the game's %u %u)\n", port, lo, hi, M16(f + 0x42), M16(f + 0x44));
    g_slot[port].rumble[0] = lo, g_slot[port].rumble[1] = hi;
    ReleaseSRWLockExclusive(&g_lock);
    M32(f) = ERROR_SUCCESS;
    if (M32(f + 4)) M32(M32(f + 4) + 4) = 1; /* signal hEvent (handle = KEVENT) */
    ret(c, ERROR_IO_PENDING, 2);
}
