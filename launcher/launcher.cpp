/* Redline Recompiled launcher: point it at your own copy of the game (an Xbox ISO or an extracted game folder) and it
 * builds the PC version from it, then starts it.
 *
 * The recompiled game is made on the player's PC from their files; nothing of the game ships with the port. Build runs
 * the pipeline NOTES.md describes, each step a command whose output fills the details log:
 *   ISO only:  python tools\xiso_extract.py <iso> game
 *   always:    the game's Forza.xbe must be the one this port was made for (SHA-1 below)
 *   once:      python tools\find_functions.py, tools\oovpa_pp.bat, python tools\find_symbols.py (analysis\)
 *   always:    python tools\lift.py (recomp\), python tools\build.py (build\redline.exe)
 * It needs Python 3 with capstone, Pillow and numpy (it offers to install them with pip) and the Visual Studio C++
 * build tools (it says where to get them). launcher.ini next to it remembers the game. Everything is relative to the
 * launcher's own folder, the root of the port.
 *
 * The window is drawn with Direct2D and DirectWrite (hence C++, written C-style): a dark page with the tachometer art,
 * one main action at a time (Build, then Play), a settings page for build\recomp.ini (the same settings as the
 * game's Recomp Settings), and links to the saves, the game's log and the game folder.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/version.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define XBE_SHA1 "e60a968e7ea73a1065b87dc1120f0b02e2eb1b58" /* Forza Motorsport (USA) (Rev 1), Forza.xbe */

#define WM_LOG (WM_APP + 1)  /* lParam: malloc'd text to append */
#define WM_DONE (WM_APP + 2) /* wParam: 1 built */
#define WM_STEP (WM_APP + 3) /* the build moved to another step */

static HWND g_wnd, g_log;
static char g_root[MAX_PATH], g_ini[MAX_PATH], g_rini[MAX_PATH], g_source[MAX_PATH], g_python[64];
static volatile LONG g_busy, g_cancel;
static volatile HANDLE g_child; /* the step's process, for Cancel */
static int g_failed;            /* the last build stopped */
static char g_fail_line[256];   /* its last log line that says why */
static volatile LONG g_step;
static const char *const g_step_names[] = {"Checking Python and the C++ build tools", "Extracting the disc image",
                                           "Checking the game's files", "Finding the game's code",
                                           "Translating it to C", "Compiling"};
#define STEPS 6

/* ---------------- the build ---------------- */

static void post_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    PostMessageA(g_wnd, WM_LOG, 0, (LPARAM)_strdup(buf));
}

static void step(int n) {
    InterlockedExchange(&g_step, n);
    PostMessageA(g_wnd, WM_STEP, 0, 0);
}

static int is_iso(const char *p) {
    const char *dot = strrchr(p, '.');
    return dot && !_stricmp(dot, ".iso");
}

/* the game folder the build and the game use: the chosen folder, or game\ for an ISO (extracted there) */
static void game_dir(char *out) {
    if (is_iso(g_source)) snprintf(out, MAX_PATH, "%s\\game", g_root);
    else snprintf(out, MAX_PATH, "%s", g_source);
}

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static int built(void) {
    char exe[MAX_PATH];
    snprintf(exe, sizeof exe, "%s\\build\\redline.exe", g_root);
    return exists(exe) && g_source[0];
}

/* run cmd (through cmd.exe) in the root folder, its output to the log; returns its exit code */
static DWORD run(const char *cmd) {
    if (g_cancel) return 1;
    post_log("> %s\r\n", cmd);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    char line[4096];
    snprintf(line, sizeof line, "cmd /d /s /c \"%s\"", cmd);
    SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_root, &si, &pi)) {
        CloseHandle(rd), CloseHandle(wr);
        post_log("cannot start: error %lu\r\n", GetLastError());
        return 1;
    }
    g_child = pi.hProcess;
    CloseHandle(wr);
    char buf[1024], out[2100];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf - 1, &n, NULL) && n) { /* bare \n to \r\n for the edit control */
        size_t k = 0;
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) out[k++] = '\r';
            out[k++] = buf[i];
        }
        out[k] = 0;
        PostMessageA(g_wnd, WM_LOG, 0, (LPARAM)_strdup(out));
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    g_child = NULL;
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return g_cancel ? 1 : code;
}

static int sha1_file(const char *path, char hex[41]) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    UCHAR digest[20];
    FILE *f = fopen(path, "rb");
    if (!f || BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0)) {
        if (f) fclose(f);
        return 0;
    }
    BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    static UCHAR buf[1 << 20];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) BCryptHashData(h, buf, (ULONG)n, 0);
    fclose(f);
    BCryptFinishHash(h, digest, 20, 0);
    BCryptDestroyHash(h), BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    return 1;
}

static int have_python(void) { /* py -3 (the launcher Python installs) or python on PATH, with the packages */
    static const char *const cands[] = {"py -3", "python"};
    for (int i = 0; i < 2; i++) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "%s -c \"import sys; assert sys.version_info >= (3, 8)\" >nul 2>&1", cands[i]);
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        char line[256];
        snprintf(line, sizeof line, "cmd /d /s /c \"%s\"", cmd);
        if (CreateProcessA(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_root, &si, &pi)) {
            DWORD code = 1;
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
            if (!code) {
                snprintf(g_python, sizeof g_python, "%s", cands[i]);
                return 1;
            }
        }
    }
    return 0;
}

static int have_msvc(void) {
    char vswhere[MAX_PATH];
    ExpandEnvironmentStringsA("%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe", vswhere, MAX_PATH);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "\"%s\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
             "-find VC\\Auxiliary\\Build\\vcvars64.bat | findstr vcvars64 >nul", vswhere);
    return exists(vswhere) && run(cmd) == 0;
}

static DWORD WINAPI build_thread(void *arg) {
    (void)arg;
    char game[MAX_PATH], xbe[MAX_PATH], cmd[2048], hex[41];
    int ok = 0;
    game_dir(game);
    snprintf(xbe, sizeof xbe, "%s\\Forza.xbe", game);
    step(0);
    if (!have_python()) {
        post_log("Python 3 was not found. Install it from https://www.python.org/downloads/ (tick \"Add python.exe to "
                 "PATH\"), then press Build again.\r\n");
        goto done;
    }
    snprintf(cmd, sizeof cmd, "%s -c \"import capstone, PIL, numpy\"", g_python);
    if (run(cmd)) {
        if (g_cancel || MessageBoxA(g_wnd, "The build needs the Python packages capstone, Pillow and numpy. Install them now "
                                           "with pip?", RECOMP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
            goto done;
        snprintf(cmd, sizeof cmd, "%s -m pip install --user capstone pillow numpy", g_python);
        if (run(cmd)) goto done;
    }
    if (!have_msvc()) {
        post_log("The Visual Studio C++ build tools were not found. Install \"Desktop development with C++\" from "
                 "https://visualstudio.microsoft.com/visual-cpp-build-tools/ then press Build again.\r\n");
        goto done;
    }
    if (is_iso(g_source)) {
        step(1);
        char have[41] = "";
        if (!exists(xbe) || !sha1_file(xbe, have) || strcmp(have, XBE_SHA1)) {
            post_log("Extracting the ISO (3.6 GB)...\r\n");
            snprintf(cmd, sizeof cmd, "%s tools\\xiso_extract.py \"%s\" game", g_python, g_source);
            if (run(cmd)) goto done;
        }
    }
    step(2);
    if (!sha1_file(xbe, hex)) {
        post_log("%s was not found. Choose the folder that holds the game's Forza.xbe and media folder.\r\n", xbe);
        goto done;
    }
    if (strcmp(hex, XBE_SHA1)) {
        post_log("This Forza.xbe (SHA-1 %s) is not the one this port was made for: Forza Motorsport (USA) (Rev 1).\r\n", hex);
        goto done;
    }
    post_log("Forza.xbe is Forza Motorsport (USA) (Rev 1).\r\n");
    step(3);
    CreateDirectoryA("analysis", NULL);
    if (!exists("analysis\\Forza.functions.json")) {
        snprintf(cmd, sizeof cmd, "%s tools\\find_functions.py \"%s\" analysis\\Forza.functions.json", g_python, xbe);
        if (run(cmd)) goto done;
    }
    if (!exists("analysis\\Forza.symbols.json")) {
        if (run("tools\\oovpa_pp.bat")) goto done;
        snprintf(cmd, sizeof cmd, "%s tools\\find_symbols.py \"%s\" build\\oovpa third_party\\nxdk\\xboxkrnl.exe.def "
                 "analysis\\Forza.symbols.json", g_python, xbe);
        if (run(cmd)) goto done;
    }
    step(4);
    snprintf(cmd, sizeof cmd, "%s tools\\lift.py \"%s\" analysis\\Forza.functions.json recomp --hle runtime\\hle.txt "
             "--symbols analysis\\Forza.symbols.json", g_python, xbe);
    if (run(cmd)) goto done;
    step(5);
    snprintf(cmd, sizeof cmd, "%s tools\\build.py", g_python);
    if (run(cmd)) goto done;
    post_log("\r\nDone: press Play.\r\n");
    ok = 1;
done:
    if (!ok) post_log(g_cancel ? "\r\nCancelled.\r\n" : "\r\nThe build stopped; the log above says why.\r\n");
    PostMessageA(g_wnd, WM_DONE, ok, 0);
    return 0;
}

static void start_build(void) {
    if (!g_source[0] || InterlockedCompareExchange(&g_busy, 1, 0)) return;
    g_cancel = 0, g_failed = 0, g_step = 0;
    SetWindowTextA(g_log, "");
    CloseHandle(CreateThread(NULL, 0, build_thread, NULL, 0, NULL));
}

static void cancel_build(void) {
    InterlockedExchange(&g_cancel, 1);
    HANDLE h = g_child;
    if (h) TerminateProcess(h, 1);
}

/* a dropped or chosen ISO or folder; returns 0 (and says why) when it is neither */
static int set_source(const char *path) {
    char xbe[MAX_PATH];
    snprintf(xbe, sizeof xbe, "%s\\Forza.xbe", path);
    if (!is_iso(path) && !exists(xbe)) {
        MessageBoxA(g_wnd, "That is neither a disc image (.iso) nor a folder holding the game's Forza.xbe.", RECOMP_NAME,
                    MB_ICONWARNING);
        return 0;
    }
    snprintf(g_source, sizeof g_source, "%s", path);
    WritePrivateProfileStringA("launcher", "game", g_source, g_ini);
    return 1;
}

static void choose(int iso) {
    char path[MAX_PATH] = "";
    if (iso) {
        OPENFILENAMEA ofn = {sizeof ofn};
        ofn.hwndOwner = g_wnd;
        ofn.lpstrFilter = "Xbox disc image (*.iso)\0*.iso\0All files\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameA(&ofn)) return;
    } else {
        BROWSEINFOA bi = {g_wnd};
        bi.lpszTitle = "The game folder: the one holding Forza.xbe and media";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST id = SHBrowseForFolderA(&bi);
        if (!id || !SHGetPathFromIDListA(id, path)) return;
        CoTaskMemFree(id);
    }
    set_source(path);
}

/* the game, its errors and progress in build\redline.log (the Logs link) */
static void play(void) {
    char game[MAX_PATH], exe[MAX_PATH], line[MAX_PATH * 2 + 8], dir[MAX_PATH], logp[MAX_PATH];
    game_dir(game);
    snprintf(exe, sizeof exe, "%s\\build\\redline.exe", g_root);
    snprintf(dir, sizeof dir, "%s\\build", g_root);
    snprintf(logp, sizeof logp, "%s\\build\\redline.log", g_root);
    snprintf(line, sizeof line, "\"%s\" \"%s\"", exe, game);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE log = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, NULL);
    STARTUPINFOA si = {sizeof si};
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = log;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi;
    if (CreateProcessA(exe, line, NULL, NULL, log != INVALID_HANDLE_VALUE, 0, NULL, dir, &si, &pi))
        CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    else MessageBoxA(g_wnd, "The game could not be started.", RECOMP_NAME, MB_ICONERROR);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
}

static void open_path(const char *rel_or_abs, int select_file) {
    char p[MAX_PATH * 2];
    if (rel_or_abs[1] == ':') snprintf(p, sizeof p, "%s", rel_or_abs);
    else snprintf(p, sizeof p, "%s\\%s", g_root, rel_or_abs);
    if (!exists(p)) {
        MessageBoxA(g_wnd, select_file ? "Nothing there yet: it appears once the game has run." : "That folder does not exist yet.",
                    RECOMP_NAME, MB_ICONINFORMATION);
        return;
    }
    ShellExecuteA(g_wnd, "open", p, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------------- settings (build\recomp.ini, as the game's Recomp Settings) ---------------- */

typedef struct {
    const char *key, *label, *desc;
    int def, n, values[5];
    const char *names[5];
} Setting;
static const Setting g_settings[] = {
    {"renderscale", "Render scale", "Each frame drawn this many times larger: sharper, slower.", 2, 4, {1, 2, 3, 4},
     {"1x", "2x", "3x", "4x"}},
    {"fsr", "Upscaling", "NVIDIA DLSS (RTX) and AMD FSR 3 rebuild the picture from several frames.", 1, 4, {1, 2, 3, 0},
     {"FSR 1", "DLSS", "FSR 3", "Bilinear"}},
    {"framerate", "Frame rate", "30 as on the Xbox. Above 60, races reach as many as this PC can draw.", 60, 4,
     {60, 120, 144, 30}, {"60", "120", "144", "30"}},
    {"anisotropy", "Texture filter", "Keeps the road and other textures sharp at a distance.", 8, 5, {1, 2, 4, 8, 16},
     {"Off", "2x", "4x", "8x", "16x"}},
    {"fullscreen", "Borderless fullscreen", "Fill the whole screen, with no title bar or frame (Alt+Enter in the game).",
     0, 2, {0, 1}, {"Off", "On"}},
    {"widescreen", "Widescreen", "Native 16:9 picture instead of 4:3.", 1, 2, {1, 0}, {"On", "Off"}},
    {"ultrawide", "Ultrawide", "A wider view on an ultrawide screen; the HUD and menus stay 16:9.", 0, 2, {0, 1},
     {"Off", "On"}},
    {"videomode", "Video mode", "The game's own resolution; 720p and 1080i are HD.", 480, 3, {480, 720, 1080},
     {"480p", "720p", "1080i"}},
    {"texpack", "Texture pack", "Replacement textures, such as the 4x upscaled pack.", 1, 2, {1, 0}, {"On", "Off"}},
    {"videos", "Upscaled videos", "The cutscenes upscaled 4x in place of the originals.", 1, 2, {1, 0}, {"On", "Off"}},
    {"enhaudio", "Enhanced audio", "Clearer sound, reverb, fuller bass and wider stereo.", 1, 2, {1, 0}, {"On", "Off"}},
    {"speakers", "Speakers", "Auto plays the game's 5.1 mix on 5.1 and 7.1 speakers.", 0, 2, {0, 1}, {"Auto", "Stereo"}},
};
#define NSETTINGS (int)(sizeof g_settings / sizeof g_settings[0])

static int setting_index(int i) { /* of the saved value; the default's if it is none of them */
    const Setting *s = &g_settings[i];
    int v = GetPrivateProfileIntA("recomp", s->key, s->def, g_rini);
    for (int k = 0; k < s->n; k++)
        if (s->values[k] == v) return k;
    for (int k = 0; k < s->n; k++)
        if (s->values[k] == s->def) return k;
    return 0;
}

static void setting_cycle(int i, int dir) {
    const Setting *s = &g_settings[i];
    int k = (setting_index(i) + dir + s->n) % s->n;
    char v[16];
    snprintf(v, sizeof v, "%d", s->values[k]);
    CreateDirectoryA("build", NULL);
    WritePrivateProfileStringA("recomp", s->key, v, g_rini);
}

/* ---------------- drawing ---------------- */

static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dw;
static ID2D1HwndRenderTarget *g_rt;
static IDWriteTextFormat *g_f_head, *g_f_sub, *g_f_body, *g_f_btn, *g_f_btn_big, *g_f_small, *g_f_logo, *g_f_logo2,
    *g_f_pct, *g_f_label;
static float g_dpi = 96;
static int g_page;      /* 0 main, 1 settings */
static int g_details;   /* the build's log shown */
static int g_hover = -1, g_press = -1;
static ULONGLONG g_anim; /* GetTickCount64 at start, for the progress shimmer */

/* clickable things: ids; the rectangles of this frame (DIPs) */
enum {
    B_ISO = 1, B_FOLDER, B_BUILD, B_PLAY, B_SETTINGS, B_DETAILS, B_CANCEL, B_DONE, B_SAVES, B_LOGS, B_GAME, B_REBUILD,
    B_CHANGE, B_SET0 = 100 /* + setting: its value; + 50: its left arrow */
};
typedef struct {
    int id;
    D2D1_RECT_F r;
} Hit;
static Hit g_hits[64];
static int g_nhits;

static D2D1_COLOR_F rgb(unsigned c, float a = 1) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a);
}
#define C_TEXT 0xF3EEEE
#define C_MUTED 0xAE9FA1
#define C_FAINT 0x75696A
#define C_RED 0xE3262E
#define C_RED_HI 0xF4434A
#define C_RED_LO 0xB71E25
#define C_AMBER 0xF2A33A

static ID2D1SolidColorBrush *g_brush; /* the target's one brush, recoloured for each use */
static ID2D1SolidColorBrush *brush(D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    return g_brush;
}

static IDWriteTextFormat *font(const wchar_t *family, float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextFormat *f = NULL;
    g_dw->CreateTextFormat(family, NULL, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    return f;
}

static void text(const char *s, IDWriteTextFormat *f, D2D1_RECT_F r, unsigned color, float alpha = 1,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    wchar_t w[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 1024) - 1;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_rt->DrawText(w, n > 0 ? n : 0, f, r, brush(rgb(color, alpha)));
}

static float text_width(const char *s, IDWriteTextFormat *f) {
    wchar_t w[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512) - 1;
    IDWriteTextLayout *l;
    float width = 0;
    if (SUCCEEDED(g_dw->CreateTextLayout(w, n, f, 4000, 200, &l))) {
        DWRITE_TEXT_METRICS m;
        l->GetMetrics(&m);
        width = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return width;
}

static void hit(int id, D2D1_RECT_F r) {
    if (g_nhits < 64) g_hits[g_nhits].id = id, g_hits[g_nhits++].r = r;
}

static void rounded(D2D1_RECT_F r, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F line, float width = 1) {
    D2D1_ROUNDED_RECT rr = {r, radius, radius};
    if (fill.a > 0) g_rt->FillRoundedRectangle(rr, brush(fill));
    if (line.a > 0) g_rt->DrawRoundedRectangle(rr, brush(line), width);
}

/* styles: 0 primary (red), 1 quiet (outlined), 2 link (text) */
static void button(int id, const char *label, D2D1_RECT_F r, int style, int enabled = 1, IDWriteTextFormat *f = NULL) {
    int hover = enabled && g_hover == id, press = enabled && g_press == id;
    if (style == 0) {
        unsigned c = !enabled ? 0x4A3335 : press ? C_RED_LO : hover ? C_RED_HI : C_RED;
        rounded(r, 10, rgb(c), rgb(0, 0));
        text(label, f ? f : g_f_btn, r, enabled ? 0xFFFFFF : C_FAINT, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (style == 1) {
        rounded(r, 10, rgb(0xFFFFFF, press ? 0.12f : hover ? 0.08f : 0.04f), rgb(0xFFFFFF, enabled ? 0.16f : 0.06f));
        text(label, f ? f : g_f_btn, r, enabled ? C_TEXT : C_FAINT, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        text(label, f ? f : g_f_small, r, hover ? 0xFFFFFF : C_MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (hover) g_rt->DrawLine(D2D1::Point2F(r.left + 2, r.bottom - 6), D2D1::Point2F(r.right - 2, r.bottom - 6),
                                  brush(rgb(0xFFFFFF, 0.6f)), 1);
    }
    if (enabled) hit(id, r);
}

/* an arc of the circle (cx, cy, r) from angle a to b (radians, clockwise on screen), stroked */
static void arc(float cx, float cy, float r, float a, float b, D2D1_COLOR_F c, float width) {
    ID2D1PathGeometry *path = NULL;
    ID2D1GeometrySink *sink = NULL;
    g_d2d->CreatePathGeometry(&path);
    path->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(cx + cosf(a) * r, cy + sinf(a) * r), D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i <= 96; i++) {
        float t = a + (b - a) * i / 96;
        sink->AddLine(D2D1::Point2F(cx + cosf(t) * r, cy + sinf(t) * r));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    g_rt->DrawGeometry(path, brush(c), width);
    sink->Release(), path->Release();
}

/* The Redline Recompiled emblem, as tools/make_icon.py draws it for the icons: a gauge's dial (135 to 405 degrees)
 * whose last fifth is the redline, ticks, a needle from behind a bold italic R into the redline (small: a red hub
 * instead of the R). (cx, cy) the dial's middle, r its radius, a the opacity. */
static void emblem(float cx, float cy, float r, float a, int letter) {
    const float pi = 3.14159265f, a0 = pi * 0.75f, sweep = pi * 1.5f, split = a0 + sweep * 0.8f;
    float rim = r * 0.139f, red = r * 0.278f;
    arc(cx, cy, r, a0, split, rgb(C_TEXT, a), rim);
    arc(cx, cy, r + (red - rim) / 2, split, a0 + sweep, rgb(C_RED, a), red);
    for (int i = 1; letter && i <= 6; i++) {
        float t = a0 + sweep * i / 8;
        g_rt->DrawLine(D2D1::Point2F(cx + cosf(t) * r * 0.78f, cy + sinf(t) * r * 0.78f),
                       D2D1::Point2F(cx + cosf(t) * r * 0.88f, cy + sinf(t) * r * 0.88f), brush(rgb(C_TEXT, a)), r * 0.067f);
    }
    float an = a0 + sweep * 0.86f, nx = -sinf(an), ny = cosf(an), half = r * 0.067f, from = letter ? r * 0.3f : 0;
    D2D1_POINT_2F p[3] = {{cx + cosf(an) * from + nx * half, cy + sinf(an) * from + ny * half},
                          {cx + cosf(an) * r * 1.02f, cy + sinf(an) * r * 1.02f},
                          {cx + cosf(an) * from - nx * half, cy + sinf(an) * from - ny * half}};
    ID2D1PathGeometry *path = NULL;
    ID2D1GeometrySink *sink = NULL;
    g_d2d->CreatePathGeometry(&path);
    path->Open(&sink);
    sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(p + 1, 2);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    g_rt->FillGeometry(path, brush(rgb(C_RED, a)));
    sink->Release(), path->Release();
    if (!letter) {
        g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 0.19f, r * 0.19f), brush(rgb(C_RED, a)));
        return;
    }
    static IDWriteTextFormat *f;
    static float f_size;
    if (!f || f_size != r * 1.39f) {
        if (f) f->Release();
        f_size = r * 1.39f;
        g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC,
                               DWRITE_FONT_STRETCH_NORMAL, f_size, L"en-us", &f);
    }
    if (f) text("R", f, D2D1::RectF(cx - r, cy - r + r * 0.02f, cx + r, cy + r + r * 0.02f), 0xFFFFFF, a,
                DWRITE_TEXT_ALIGNMENT_CENTER);
}

/* the backdrop: a dark warm gradient, the emblem large and faint on the right, speed streaks low left */
static void backdrop(float w, float h) {
    ID2D1LinearGradientBrush *g = NULL;
    ID2D1GradientStopCollection *stops = NULL;
    D2D1_GRADIENT_STOP s[3] = {{0, rgb(0x1A0B0D)}, {0.55f, rgb(0x0D0809)}, {1, rgb(0x060506)}};
    g_rt->CreateGradientStopCollection(s, 3, &stops);
    g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(w, h)), stops, &g);
    g_rt->FillRectangle(D2D1::RectF(0, 0, w, h), g);
    g->Release(), stops->Release();

    emblem(w * 0.72f, h * 0.55f, h * 0.36f, 0.09f, 1);
    for (int i = 0; i < 7; i++) { /* speed streaks */
        float y = h - 70 - i * 14.0f, x0 = -40.0f + i * 30, len = 260.0f + (i * 97 % 5) * 60;
        g_rt->DrawLine(D2D1::Point2F(x0, y), D2D1::Point2F(x0 + len, y), brush(rgb(i == 2 ? C_RED : 0xFFFFFF, i == 2 ? 0.20f : 0.035f)), 3);
    }
}

static void logo(float x, float y) {
    emblem(x + 12, y + 12, 10, 1, 0);
    text("REDLINE", g_f_logo, D2D1::RectF(x + 32, y - 4, x + 140, y + 26), C_TEXT);
    text("RECOMPILED", g_f_logo2, D2D1::RectF(x + 32 + text_width("REDLINE", g_f_logo) + 6, y - 4, x + 400, y + 26), C_MUTED);
}

/* the name of a setting's value, by the setting's key (rows move as settings are added) */
static const char *setting_name(const char *key) {
    for (int i = 0; i < NSETTINGS; i++)
        if (!strcmp(g_settings[i].key, key)) return g_settings[i].names[setting_index(i)];
    return "?";
}

static void settings_summary(char *out, size_t n) {
    snprintf(out, n, "Render %s  \xC2\xB7  %s  \xC2\xB7  %s fps  \xC2\xB7  Textures %s  \xC2\xB7  Audio %s",
             setting_name("renderscale"), setting_name("fsr"), setting_name("framerate"), setting_name("texpack"),
             strcmp(setting_name("enhaudio"), "On") ? "original" : "enhanced");
}

static void draw_main(float w, float h, float x0, float top) {
    float cw = w - x0 * 2 > 620 ? 620 : w - x0 * 2;
    if (g_busy) {
        text("Building\xE2\x80\xA6", g_f_head, D2D1::RectF(x0, top, x0 + 900, top + 64), C_TEXT);
        text("The first build takes about ten minutes; later ones only redo what changed.", g_f_sub,
             D2D1::RectF(x0, top + 66, x0 + 900, top + 96), C_MUTED);
        D2D1_RECT_F card = D2D1::RectF(x0, top + 124, x0 + cw, top + 124 + 150);
        rounded(card, 14, rgb(0xFFFFFF, 0.045f), rgb(0xFFFFFF, 0.08f));
        int s = g_step;
        char pct[16], what[128];
        snprintf(pct, sizeof pct, "%d%%", s * 100 / STEPS);
        snprintf(what, sizeof what, "%s  (step %d of %d)", g_step_names[s], s + 1, STEPS);
        text(pct, g_f_pct, D2D1::RectF(card.left + 24, card.top + 16, card.left + 200, card.top + 66), C_TEXT);
        text(what, g_f_body, D2D1::RectF(card.left + 24, card.top + 68, card.right - 24, card.top + 92), C_MUTED);
        D2D1_RECT_F bar = D2D1::RectF(card.left + 24, card.top + 104, card.right - 24, card.top + 110);
        rounded(bar, 3, rgb(0xFFFFFF, 0.08f), rgb(0, 0));
        float done = (bar.right - bar.left) * (s + 0.5f) / STEPS;
        rounded(D2D1::RectF(bar.left, bar.top, bar.left + done, bar.bottom), 3, rgb(C_RED), rgb(0, 0));
        float phase = (float)((GetTickCount64() - g_anim) % 1600) / 1600; /* a light running along the done part */
        float sx = bar.left + (done + 120) * phase - 120;
        rounded(D2D1::RectF(sx < bar.left ? bar.left : sx, bar.top, sx + 120 > bar.left + done ? bar.left + done : sx + 120, bar.bottom), 3,
                rgb(0xFFFFFF, 0.35f), rgb(0, 0));
        button(B_DETAILS, g_details ? "Hide details" : "Show details", D2D1::RectF(card.left + 16, card.bottom - 34, card.left + 130, card.bottom - 6), 2);
        button(B_CANCEL, "Cancel", D2D1::RectF(card.right - 120, card.bottom - 36, card.right - 20, card.bottom - 8), 2);
        return;
    }
    if (built() && !g_failed) {
        text("Ready to race", g_f_head, D2D1::RectF(x0, top, x0 + 900, top + 64), C_TEXT);
        char sum[256];
        settings_summary(sum, sizeof sum);
        text(sum, g_f_sub, D2D1::RectF(x0, top + 66, x0 + 900, top + 96), C_MUTED);
        D2D1_RECT_F play_r = D2D1::RectF(x0, top + 132, x0 + 250, top + 196);
        button(B_PLAY, "      Play", play_r, 0, 1, g_f_btn_big);
        { /* the play triangle */
            float tx = play_r.left + 84, ty = (play_r.top + play_r.bottom) / 2;
            ID2D1PathGeometry *path = NULL;
            ID2D1GeometrySink *sink = NULL;
            g_d2d->CreatePathGeometry(&path);
            path->Open(&sink);
            D2D1_POINT_2F p[3] = {{tx, ty - 11}, {tx + 18, ty}, {tx, ty + 11}};
            sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(p + 1, 2);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            g_rt->FillGeometry(path, brush(rgb(0xFFFFFF)));
            sink->Release(), path->Release();
        }
        button(B_SETTINGS, "Settings", D2D1::RectF(x0 + 266, top + 132, x0 + 406, top + 196), 1);
        char src[MAX_PATH + 32];
        snprintf(src, sizeof src, "Game: %s", g_source);
        text(src, g_f_small, D2D1::RectF(x0, top + 214, x0 + 700, top + 236), C_FAINT);
        button(B_CHANGE, "Change", D2D1::RectF(x0 + 8 + text_width(src, g_f_small), top + 212, x0 + 70 + text_width(src, g_f_small), top + 238), 2);
        return;
    }
    if (g_failed) { /* what went wrong, the way on, and the log under it (place_log) */
        text("The build stopped", g_f_head, D2D1::RectF(x0, top, x0 + 900, top + 64), C_TEXT);
        text(g_fail_line, g_f_sub, D2D1::RectF(x0, top + 66, w - x0, top + 96), C_AMBER);
        button(B_BUILD, "Try again", D2D1::RectF(x0, top + 120, x0 + 200, top + 170), 0, 1, g_f_btn_big);
        button(B_DETAILS, g_details ? "Hide details" : "Show details", D2D1::RectF(x0 + 216, top + 132, x0 + 336, top + 160), 2);
        button(B_CHANGE, "Choose another copy", D2D1::RectF(x0 + 346, top + 132, x0 + 506, top + 160), 2);
        return;
    }
    text("Build your copy", g_f_head, D2D1::RectF(x0, top, x0 + 900, top + 64), C_TEXT);
    text(g_failed ? g_fail_line : "Forza Motorsport is recompiled on this PC from your own disc. Nothing is uploaded.",
         g_f_sub, D2D1::RectF(x0, top + 66, x0 + 1000, top + 96), g_failed ? C_AMBER : C_MUTED);
    D2D1_RECT_F card = D2D1::RectF(x0, top + 124, x0 + cw, top + 124 + 176);
    rounded(card, 14, rgb(0xFFFFFF, 0.045f), rgb(0xFFFFFF, 0.08f));
    D2D1_RECT_F drop = D2D1::RectF(card.left + 16, card.top + 16, card.right - 16, card.top + 88);
    ID2D1StrokeStyle *dash = NULL;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
    sp.dashStyle = D2D1_DASH_STYLE_DASH;
    g_d2d->CreateStrokeStyle(sp, NULL, 0, &dash);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(drop, 10, 10), brush(rgb(0xFFFFFF, 0.18f)), 1.5f, dash);
    if (dash) dash->Release();
    text("Drop your disc image (.iso) or extracted game folder here", g_f_body, D2D1::RectF(drop.left, drop.top, drop.right, drop.bottom),
         C_MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    button(B_ISO, "Choose disc image", D2D1::RectF(card.left + 16, card.top + 104, card.left + 196, card.top + 144), 1);
    button(B_FOLDER, "Choose game folder", D2D1::RectF(card.left + 206, card.top + 104, card.left + 396, card.top + 144), 1);
    text(g_source[0] ? g_source : "Forza Motorsport (Xbox, USA). Nothing chosen yet.", g_f_small,
         D2D1::RectF(card.left + 18, card.top + 148, card.right - 16, card.top + 172), g_source[0] ? C_TEXT : C_FAINT);
    button(B_BUILD, g_failed ? "Try again" : "Build", D2D1::RectF(x0, card.bottom + 24, x0 + 220, card.bottom + 80), 0,
           g_source[0] != 0, g_f_btn_big);
    if (g_failed) button(B_DETAILS, g_details ? "Hide details" : "Show details", D2D1::RectF(x0 + 236, card.bottom + 38, x0 + 360, card.bottom + 66), 2);
    else text("Needs Python 3 and the Visual Studio C++ build tools; the launcher checks and says where to get them.",
              g_f_small, D2D1::RectF(x0 + 240, card.bottom + 24, x0 + 900, card.bottom + 80), C_FAINT);
}

static void draw_settings(float w, float h, float x0, float top) {
    top = 96; /* a long page: from just under the logo */
    float cw = w - x0 * 2 > 760 ? 760 : w - x0 * 2, rowh = 34, y = top + 96; /* 13 rows above the footer */
    text("Settings", g_f_head, D2D1::RectF(x0, top, x0 + 600, top + 64), C_TEXT);
    text("The same as Recomp Settings in the game's main menu. Click a value to change it.", g_f_sub,
         D2D1::RectF(x0, top + 62, x0 + 900, top + 88), C_MUTED);
    button(B_DONE, "Done", D2D1::RectF(x0 + cw - 140, top + 12, x0 + cw, top + 56), 0);
    D2D1_RECT_F card = D2D1::RectF(x0, y, x0 + cw, y + rowh * NSETTINGS + 12);
    rounded(card, 14, rgb(0x0E0A0B, 0.92f), rgb(0xFFFFFF, 0.08f)); /* opaque enough that the dial stays behind */
    for (int i = 0; i < NSETTINGS; i++) {
        float ry = y + 6 + rowh * i;
        if (i) g_rt->DrawLine(D2D1::Point2F(card.left + 16, ry), D2D1::Point2F(card.right - 16, ry), brush(rgb(0xFFFFFF, 0.05f)), 1);
        text(g_settings[i].label, g_f_label, D2D1::RectF(card.left + 20, ry, card.left + 190, ry + rowh), C_TEXT);
        text(g_settings[i].desc, g_f_small, D2D1::RectF(card.left + 190, ry, card.right - 200, ry + rowh), C_FAINT);
        D2D1_RECT_F val = D2D1::RectF(card.right - 180, ry + 8, card.right - 20, ry + rowh - 8);
        int id = B_SET0 + i, hov = g_hover == id || g_hover == id + 50;
        rounded(val, 8, rgb(0xFFFFFF, hov ? 0.10f : 0.05f), rgb(0xFFFFFF, 0.10f));
        text(g_settings[i].names[setting_index(i)], g_f_label, val, C_TEXT, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        text("\xE2\x80\xB9", g_f_label, D2D1::RectF(val.left + 6, val.top, val.left + 26, val.bottom), C_MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        text("\xE2\x80\xBA", g_f_label, D2D1::RectF(val.right - 26, val.top, val.right - 6, val.bottom), C_MUTED, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(id + 50, D2D1::RectF(val.left, val.top, val.left + 40, val.bottom)); /* the left arrow goes back */
        hit(id, val);
    }
}

static void draw(void) {
    if (!g_rt || !g_brush) return;
    g_nhits = 0;
    g_rt->BeginDraw();
    D2D1_SIZE_F sz = g_rt->GetSize();
    float w = sz.width, h = sz.height, x0 = w > 1000 ? 72.0f : 40.0f;
    backdrop(w, h);
    logo(x0, 28);
    float top = h * 0.24f < 150 ? 150 : h * 0.24f;
    if (g_page == 1) draw_settings(w, h, x0, top);
    else draw_main(w, h, x0, top);
    /* the footer */
    g_rt->FillRectangle(D2D1::RectF(0, h - 48, w, h), brush(rgb(0x070506, 0.92f)));
    g_rt->DrawLine(D2D1::Point2F(0, h - 48), D2D1::Point2F(w, h - 48), brush(rgb(0xFFFFFF, 0.06f)), 1);
    text(RECOMP_NAME " " RECOMP_VERSION "  \xC2\xB7  Not affiliated with Microsoft or Turn 10 Studios", g_f_small,
         D2D1::RectF(x0, h - 48, x0 + 600, h), C_FAINT);
    static const struct {
        int id;
        const char *label;
    } links[] = {{B_REBUILD, "Rebuild"}, {B_GAME, "Game folder"}, {B_LOGS, "Logs"}, {B_SAVES, "Saves"}};
    float x = w - x0;
    for (int i = 0; i < 4; i++) {
        if (links[i].id == B_REBUILD && (!built() || g_busy)) continue;
        float tw = text_width(links[i].label, g_f_small) + 20;
        button(links[i].id, links[i].label, D2D1::RectF(x - tw, h - 46, x, h - 2), 2);
        x -= tw + 8;
    }
    if (g_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) g_brush->Release(), g_rt->Release(), g_brush = NULL, g_rt = NULL;
}

/* the build log, a native edit control (selectable, copyable) under the progress card */
static void place_log(void) {
    RECT cr;
    GetClientRect(g_wnd, &cr);
    float s = g_dpi / 96, w = cr.right / s, h = cr.bottom / s, x0 = w > 1000 ? 72.0f : 40.0f;
    float top = (h * 0.24f < 150 ? 150 : h * 0.24f) + (g_busy ? 124 + 150 + 16 : 190);
    int show = g_details && g_page == 0 && (g_busy || g_failed) && h - 64 - top > 60;
    SetWindowPos(g_log, NULL, (int)(x0 * s), (int)(top * s), (int)((w - 2 * x0) * s), (int)((h - 64 - top) * s),
                 SWP_NOZORDER | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

static void create_target(void) {
    if (g_rt) return;
    RECT cr;
    GetClientRect(g_wnd, &cr);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
    p.dpiX = p.dpiY = g_dpi;
    g_d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(cr.right, cr.bottom)), &g_rt);
    if (g_rt) g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_brush);
}

static void click(int id) {
    switch (id) {
    case B_ISO: choose(1); break;
    case B_FOLDER: choose(0); break;
    case B_CHANGE: g_failed = 0; g_source[0] = 0; break;
    case B_BUILD:
    case B_REBUILD: g_page = 0; start_build(); break;
    case B_PLAY: play(); break;
    case B_SETTINGS: g_page = 1; break;
    case B_DONE: g_page = 0; break;
    case B_DETAILS: g_details ^= 1; break;
    case B_CANCEL: cancel_build(); break;
    case B_SAVES: {
        char game[MAX_PATH], p[MAX_PATH * 2];
        game_dir(game);
        snprintf(p, sizeof p, "%s\\..\\hdd\\Partition1\\UDATA", game);
        open_path(g_source[0] ? p : "hdd", 0);
        break;
    }
    case B_LOGS: open_path("build\\redline.log", 1); break;
    case B_GAME: {
        char game[MAX_PATH];
        game_dir(game);
        open_path(g_source[0] ? game : "game", 0);
        break;
    }
    default:
        if (id >= B_SET0 + 50) setting_cycle(id - B_SET0 - 50, -1);
        else if (id >= B_SET0) setting_cycle(id - B_SET0, 1);
    }
    place_log();
    InvalidateRect(g_wnd, NULL, FALSE);
}

static int hit_test(LPARAM lp) {
    float s = g_dpi / 96, x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
    for (int i = g_nhits - 1; i >= 0; i--)
        if (x >= g_hits[i].r.left && x < g_hits[i].r.right && y >= g_hits[i].r.top && y < g_hits[i].r.bottom) return g_hits[i].id;
    return -1;
}

static HBRUSH g_log_brush;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        create_target();
        draw();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        place_log();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = (float)HIWORD(wp);
        if (g_rt) g_rt->SetDpi(g_dpi, g_dpi);
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        m->ptMinTrackSize.x = (LONG)(940 * g_dpi / 96), m->ptMinTrackSize.y = (LONG)(700 * g_dpi / 96);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int id = hit_test(lp);
        if (id != g_hover) g_hover = id, InvalidateRect(h, NULL, FALSE);
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover >= 0) g_hover = -1, InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, g_hover >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        g_press = hit_test(lp);
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int id = hit_test(lp), was = g_press;
        g_press = -1;
        ReleaseCapture();
        if (id >= 0 && id == was) click(id);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_DROPFILES: {
        char path[MAX_PATH];
        if (!g_busy && DragQueryFileA((HDROP)wp, 0, path, MAX_PATH)) set_source(path), g_failed = 0;
        DragFinish((HDROP)wp);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (g_busy) InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LOG: {
        char *t = (char *)lp;
        int len = GetWindowTextLengthA(g_log);
        if (len > 400000) SendMessageA(g_log, EM_SETSEL, 0, 100000), SendMessageA(g_log, EM_REPLACESEL, 0, (LPARAM)""), len = GetWindowTextLengthA(g_log);
        SendMessageA(g_log, EM_SETSEL, len, len);
        SendMessageA(g_log, EM_REPLACESEL, 0, (LPARAM)t);
        for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) /* the last real line, for a failure */
            if (strlen(line) > 3 && line[0] != '>' && strncmp(line, "The build stopped", 17) && strncmp(line, "Cancelled", 9))
                snprintf(g_fail_line, sizeof g_fail_line, "%s", line);
        free(t);
        return 0;
    }
    case WM_STEP: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_DONE:
        InterlockedExchange(&g_busy, 0);
        g_failed = !wp && !g_cancel;
        if (g_failed) g_details = 1;
        if (wp) FlashWindow(h, TRUE);
        place_log();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, RGB(0xC9, 0xBC, 0xBD));
        SetBkColor((HDC)wp, RGB(0x10, 0x0C, 0x0D));
        return (LRESULT)g_log_brush;
    case WM_CLOSE:
        if (g_busy && MessageBoxA(h, "A build is running. Close anyway?", RECOMP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        cancel_build();
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev, (void)cmdline;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameA(NULL, g_root, MAX_PATH);
    *strrchr(g_root, '\\') = 0;
    SetCurrentDirectoryA(g_root);
    snprintf(g_ini, sizeof g_ini, "%s\\launcher.ini", g_root);
    snprintf(g_rini, sizeof g_rini, "%s\\build\\recomp.ini", g_root);
    GetPrivateProfileStringA("launcher", "game", "", g_source, MAX_PATH, g_ini);
    if (!g_source[0] && exists("game\\Forza.xbe")) GetFullPathNameA("game", MAX_PATH, g_source, NULL);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw);
    const wchar_t *ui = L"Segoe UI Variable Display", *ui_text = L"Segoe UI";
    g_f_head = font(ui, 46, DWRITE_FONT_WEIGHT_BOLD);
    g_f_sub = font(ui_text, 16, DWRITE_FONT_WEIGHT_NORMAL);
    g_f_body = font(ui_text, 15, DWRITE_FONT_WEIGHT_NORMAL);
    g_f_btn = font(ui_text, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_f_btn_big = font(ui_text, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_f_small = font(ui_text, 13, DWRITE_FONT_WEIGHT_NORMAL);
    g_f_label = font(ui_text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_f_logo = font(ui_text, 17, DWRITE_FONT_WEIGHT_BLACK);
    g_f_logo2 = font(ui_text, 17, DWRITE_FONT_WEIGHT_LIGHT);
    g_f_pct = font(ui, 40, DWRITE_FONT_WEIGHT_BOLD);
    g_anim = GetTickCount64();

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(wc.hInstance ? wc.hInstance : GetModuleHandle(NULL), MAKEINTRESOURCE(1)); /* redline.rc */
    wc.lpszClassName = "RedlineLauncher";
    RegisterClassA(&wc);
    g_dpi = (float)GetDpiForSystem();
    int ww = (int)(1180 * g_dpi / 96), wh = (int)(740 * g_dpi / 96);
    g_wnd = CreateWindowExA(WS_EX_ACCEPTFILES, "RedlineLauncher", RECOMP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, ww, wh, NULL, NULL, inst, NULL);
    g_dpi = (float)GetDpiForWindow(g_wnd);
    BOOL dark = TRUE; /* a dark title bar, coloured like the page (Windows 11 takes the caption colour) */
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF cap = RGB(0x14, 0x0A, 0x0C);
    DwmSetWindowAttribute(g_wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
    g_log_brush = CreateSolidBrush(RGB(0x10, 0x0C, 0x0D));
    g_log = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10,
                            g_wnd, NULL, inst, NULL);
    SendMessageA(g_log, WM_SETFONT,
                 (WPARAM)CreateFontA(-(int)(13 * g_dpi / 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                     CLEARTYPE_QUALITY, 0, "Cascadia Mono"),
                 TRUE);
    SendMessageA(g_log, EM_SETLIMITTEXT, 0, 0);
    SetWindowTheme(g_log, L"DarkMode_Explorer", NULL); /* its scroll bar dark too */
    SetTimer(g_wnd, 1, 33, NULL);
    place_log();
    ShowWindow(g_wnd, show);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
