/* Redline Recompiled launcher: point it at your own copy of the game (an Xbox ISO or an extracted game folder) and it
 * builds the PC version from it, then starts it.
 *
 * The recompiled game is made on the player's PC from their files; nothing of the game ships with the port. Build runs
 * the pipeline NOTES.md describes, each step a command whose output fills the details log:
 *   first:     the build tools (g_tools: Python, capstone, zig), fetched and checked, into build\tools
 *   ISO only:  python tools\xiso_extract.py <iso> game
 *   always:    the game's Forza.xbe must be the one this port was made for (SHA-1 below)
 *   once:      python tools\find_functions.py, tools\oovpa_pp.py, tools\find_symbols.py (analysis\)
 *   always:    python tools\lift.py (recomp\), python tools\build.py --zig (build\redline.exe, the runtime, and
 *              build\forza_recomp.dll, the game code it loads)
 * Nothing needs installing: the tools run from build\tools. launcher.ini next to it remembers the game. Everything is
 * relative to the launcher's own folder, the root of the port.
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
#include <urlmon.h>
#include <xinput.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../runtime/version.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define XBE_SHA1 "e60a968e7ea73a1065b87dc1120f0b02e2eb1b58" /* Forza Motorsport (USA) (Rev 1), Forza.xbe */

#define WM_LOG (WM_APP + 1)  /* lParam: malloc'd text to append */
#define WM_DONE (WM_APP + 2) /* wParam: 1 built */
#define WM_STEP (WM_APP + 3) /* the build moved to another step */
#define WM_UPDATE (WM_APP + 4) /* the update check found a newer release (g_update_*) */
#define REPO "DivxPlays/redline-recompiled"

/* The build's tools, fetched on the first Build from their makers' own downloads, checked against the SHA-256 pinned
 * here and unpacked under build\tools: nothing is installed. capstone is the one package the Python tools need (the
 * same version as the port was made with, so the lifted code is the same); zig is clang, lld and mingw-w64's headers,
 * building with Windows' own C runtime (tools/build.py --zig). */
static const struct {
    const char *what, *url, *sha256, *dir, *check; /* check: a file the unpacked tool has */
    int mb;
} g_tools[] = {
    {"Python 3.12.10 (embeddable)", "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
     "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", "build\\tools\\python-3.12.10",
     "build\\tools\\python-3.12.10\\python.exe", 11},
    {"capstone 5.0.7",
     "https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/"
     "capstone-5.0.7-py3-none-win_amd64.whl",
     "4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1", "build\\tools\\python-3.12.10",
     "build\\tools\\python-3.12.10\\capstone\\__init__.py", 2},
    {"zig 0.14.1 (the C compiler)", "https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip",
     "554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c", "build\\tools\\zig-0.14.1",
     "build\\tools\\zig-0.14.1\\zig-x86_64-windows-0.14.1\\zig.exe", 82},
};
#define PYTHON "build\\tools\\python-3.12.10\\python.exe" /* run() starts commands in the root */

static HWND g_wnd, g_log;
static char g_root[MAX_PATH], g_ini[MAX_PATH], g_rini[MAX_PATH], g_source[MAX_PATH];
static volatile LONG g_busy, g_cancel;
static volatile HANDLE g_child; /* the step's job (its process tree), for Cancel */
static int g_failed;            /* the last build stopped */
static char g_fail_line[256];   /* its last log line that says why */
static volatile LONG g_step;
static const char *const g_step_names[] = {"Getting the build tools", "Extracting the disc image",
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

/* what the step is doing now, under its name in the build card ("zig 0.14.1, 34 of 82 MB"); the build thread writes
 * it, the window reads it */
static SRWLOCK g_detail_lock = SRWLOCK_INIT;
static char g_detail[96];
static void set_detail(const char *s) {
    AcquireSRWLockExclusive(&g_detail_lock);
    snprintf(g_detail, sizeof g_detail, "%s", s);
    ReleaseSRWLockExclusive(&g_detail_lock);
    PostMessageA(g_wnd, WM_STEP, 0, 0);
}
static void get_detail(char *out, size_t n) {
    AcquireSRWLockShared(&g_detail_lock);
    snprintf(out, n, "%s", g_detail);
    ReleaseSRWLockShared(&g_detail_lock);
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

/* ---------------- updates and what's new ---------------- */

/* a whole file, 0-terminated (NULL if it cannot be read); the caller frees it */
static char *read_all(const char *path, size_t limit) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = (char *)malloc(limit + 1);
    size_t n = buf ? fread(buf, 1, limit, f) : 0;
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

/* "v1.2.3" or "1.2.3", comparable */
static unsigned long long version_key(const char *v) {
    unsigned a = 0, b = 0, c = 0;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%u.%u.%u", &a, &b, &c);
    return (unsigned long long)a << 40 | (unsigned long long)b << 20 | c;
}

/* the first string member `key` in some JSON, unescaped to UTF-8; NULL if there is none. The caller frees it. */
static char *json_string(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    for (p += strlen(pat); *p == ' '; p++) {}
    if (*p++ != '"') return NULL;
    char *out = (char *)malloc(strlen(p) + 1), *o = out;
    while (out && *p && *p != '"') {
        if (*p != '\\' || !p[1]) {
            *o++ = *p++;
            continue;
        }
        switch (*++p) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': break;
        case 'u': {
            unsigned u = 0;
            if (sscanf(p + 1, "%4x", &u) == 1) p += 4;
            if (u < 0x80) *o++ = (char)u;
            else if (u < 0x800) *o++ = (char)(0xC0 | u >> 6), *o++ = (char)(0x80 | (u & 63));
            else *o++ = (char)(0xE0 | u >> 12), *o++ = (char)(0x80 | (u >> 6 & 63)), *o++ = (char)(0x80 | (u & 63));
            break;
        }
        default: *o++ = *p; /* \" \\ \/ */
        }
        p++;
    }
    if (out) *o = 0;
    return out;
}

/* a newer release than this one, from GitHub (build\latest_release.json): shown at the top until the player updates */
static char g_update_tag[32], *g_update_notes;
static void update_from(const char *path) {
    char *json = read_all(path, 1 << 20), *tag = json ? json_string(json, "tag_name") : NULL;
    if (tag && version_key(tag) > version_key(RECOMP_VERSION) && !g_update_tag[0]) {
        g_update_notes = json_string(json, "body");
        snprintf(g_update_tag, sizeof g_update_tag, "%s", tag[0] == 'v' ? tag + 1 : tag); /* before the message */
        PostMessageA(g_wnd, WM_UPDATE, 0, 0);
    }
    free(tag), free(json);
}

/* at most once a day, in the background: ask GitHub for the latest release. Offline, or anything wrong, it says
 * nothing. Nothing is downloaded but the answer, and nothing installed: the player follows the link or not. */
static DWORD WINAPI update_thread(void *arg) {
    (void)arg;
    char path[MAX_PATH], tmp[MAX_PATH], url[160], day[16];
    snprintf(day, sizeof day, "%u", (unsigned)(time(NULL) / 86400));
    snprintf(path, sizeof path, "%s\\build\\latest_release.json", g_root);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    /* the day in the address: not yesterday's answer from Windows' download cache */
    snprintf(url, sizeof url, "https://api.github.com/repos/" REPO "/releases/latest?day=%s", day);
    CreateDirectoryA("build", NULL);
    if (FAILED(URLDownloadToFileA(NULL, url, tmp, 0, NULL))) return 0;
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
    WritePrivateProfileStringA("launcher", "update_check_day", day, g_ini);
    update_from(path);
    return 0;
}

static void update_start(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\build\\latest_release.json", g_root);
    update_from(path); /* what the last check found */
    if (GetPrivateProfileIntA("launcher", "update_check_day", 0, g_ini) != (int)(time(NULL) / 86400))
        CloseHandle(CreateThread(NULL, 0, update_thread, NULL, 0, NULL));
}

static char *md_text(const char *md);

/* the notes, and the release's page if the player wants it */
static void update_offer(void) {
    char head[256], *plain = g_update_notes ? md_text(g_update_notes) : NULL, *notes = plain ? plain : (char *)"";
    snprintf(head, sizeof head, RECOMP_NAME " %s is out (you have " RECOMP_VERSION ").\n\n", g_update_tag);
    size_t n = strlen(notes) > 1800 ? 1800 : strlen(notes);
    char *msg = (char *)malloc(strlen(head) + n + 64);
    if (!msg) {
        free(plain);
        return;
    }
    sprintf(msg, "%s%.*s%s\n\nOpen its download page?", head, (int)n, notes, n < strlen(notes) ? "\n..." : "");
    int wn = MultiByteToWideChar(CP_UTF8, 0, msg, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(wn * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, wn) &&
        MessageBoxW(g_wnd, w, L"" RECOMP_NAME, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        char url[160];
        snprintf(url, sizeof url, "https://github.com/" REPO "/releases/tag/v%s", g_update_tag);
        ShellExecuteA(g_wnd, "open", url, NULL, NULL, SW_SHOWNORMAL);
    }
    free(w), free(msg), free(plain);
}

/* Markdown notes as plain lines, up to the first "## " heading after some text: "### Fixed" headings stand alone,
 * "- " items get a bullet and their wrapped lines are joined; the changelog's "For the README" part (notes for the
 * release itself) is left out. NULL if nothing is left. The caller frees it. */
static char *md_text(const char *md) {
    char *copy = _strdup(md), *out = (char *)malloc(strlen(md) * 2 + 1), *o = out;
    int skip = 0;
    for (char *line = copy; copy && out && line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!strncmp(line, "## ", 3)) {
            if (o > out) break;
        } else if (!strncmp(line, "### ", 4)) {
            skip = !strcmp(line + 4, "For the README");
            if (!skip) o += sprintf(o, "%s%s\n", o > out ? "\n" : "", line + 4);
        } else if (skip || !len) {
        } else if (!strncmp(line, "- ", 2)) {
            o += sprintf(o, "\xE2\x80\xA2  %s\n", line + 2);
        } else if (o > out && line[0] == ' ') { /* an item's wrapped line */
            while (*line == ' ') line++;
            o[-1] = ' ';
            o += sprintf(o, "%s\n", line);
        } else {
            o += sprintf(o, "%s\n", line);
        }
        line = next;
    }
    free(copy);
    if (out && o == out) free(out), out = NULL;
    else if (out) *o = 0;
    return out;
}

/* What's new: this version's part of CHANGELOG.md ("## 0.2.0 ..." to the next "## "); NULL if it has none */
static char *whats_new(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\CHANGELOG.md", g_root);
    char *md = read_all(path, 1 << 20), *out = NULL;
    for (char *p = md; p && (p = strstr(p, "\n## ")) != NULL; p++)
        if (!strncmp(p + 4, RECOMP_VERSION, strlen(RECOMP_VERSION)) && (unsigned char)p[4 + strlen(RECOMP_VERSION)] <= ' ') {
            char *body = strchr(p + 1, '\n'); /* past the "## 0.2.0 ..." line */
            out = body ? md_text(body) : NULL;
            break;
        }
    free(md);
    return out;
}
static char *g_news; /* shown on page 2 the first start after an update */

static int built(void) { /* the game code: the runtime (build\redline.exe) can come built, without it */
    char dll[MAX_PATH];
    snprintf(dll, sizeof dll, "%s\\build\\forza_recomp.dll", g_root);
    return exists(dll) && g_source[0];
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
    /* the step's whole process tree in a job, so Cancel ends what cmd started too (Python, tar, the compilers), not
     * just cmd, while they ran on and the pipe kept the step waiting for them */
    HANDLE job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, g_root, &si, &pi)) {
        CloseHandle(rd), CloseHandle(wr);
        if (job) CloseHandle(job);
        post_log("cannot start: error %lu\r\n", GetLastError());
        return 1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    g_child = job;
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
    if (job) CloseHandle(job);
    return g_cancel ? 1 : code;
}

/* a file's hash in lowercase hex (alg: BCRYPT_SHA1_ALGORITHM, 20 bytes; BCRYPT_SHA256_ALGORITHM, 32) */
static int hash_file(const char *path, LPCWSTR alg_id, ULONG bytes, char *hex) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    UCHAR digest[32];
    FILE *f = fopen(path, "rb");
    if (!f || BCryptOpenAlgorithmProvider(&alg, alg_id, NULL, 0)) {
        if (f) fclose(f);
        return 0;
    }
    BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    static UCHAR buf[1 << 20];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) BCryptHashData(h, buf, (ULONG)n, 0);
    fclose(f);
    BCryptFinishHash(h, digest, bytes, 0);
    BCryptDestroyHash(h), BCryptCloseAlgorithmProvider(alg, 0);
    for (ULONG i = 0; i < bytes; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    return 1;
}
static int sha1_file(const char *path, char hex[41]) { return hash_file(path, BCRYPT_SHA1_ALGORITHM, 20, hex); }

/* URLDownloadToFile's callback: the download's progress in the build card, and Cancel stops it */
struct DownloadProgress : IBindStatusCallback {
    const char *what;
    ULONG last_mb = ~0u;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **p) {
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *p = this;
            return S_OK;
        }
        *p = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() { return 1; } /* on fetch_tools' stack, for the call */
    ULONG STDMETHODCALLTYPE Release() { return 1; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) {
        if (g_cancel) return E_ABORT;
        if (done >> 20 != last_mb && total) {
            char s[96];
            last_mb = done >> 20;
            snprintf(s, sizeof s, "%s, %lu of %lu MB", what, done >> 20, (total + (1 << 19)) >> 20);
            set_detail(s);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG *) { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD *, BINDINFO *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC *, STGMEDIUM *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown *) { return S_OK; }
};

/* g_tools, each fetched once: download, check, unpack with Windows' tar. A tool counts as there by its .ok file,
 * written only once it unpacked whole (Cancel during the unpacking leaves its files but no .ok, so the next Build
 * fetches it again) */
static int fetch_tools(void) {
    char check[MAX_PATH], ok[MAX_PATH + 4], dir[MAX_PATH], zip[MAX_PATH], tar[MAX_PATH], hex[65], cmd[MAX_PATH * 3];
    ExpandEnvironmentStringsA("%SystemRoot%\\System32\\tar.exe", tar, MAX_PATH); /* not a tar from Git, which cannot unzip */
    for (size_t i = 0; i < sizeof g_tools / sizeof g_tools[0]; i++) {
        snprintf(check, sizeof check, "%s\\%s", g_root, g_tools[i].check);
        snprintf(ok, sizeof ok, "%s.ok", check);
        if (exists(ok)) continue;
        if (strlen(g_root) > 120) { /* zig's deepest file is 114 characters below build\tools\zig-0.14.1 */
            post_log("This folder's path is too long for the build tools (%u characters, 120 at most). Move the port to a "
                     "shorter one, such as C:\\Games\\RedlineRecompiled, then press Build again.\r\n", (unsigned)strlen(g_root));
            return 0;
        }
        snprintf(dir, sizeof dir, "%s\\%s", g_root, g_tools[i].dir);
        SHCreateDirectoryExA(NULL, dir, NULL);
        snprintf(zip, sizeof zip, "%s\\download.zip", dir);
        post_log("Downloading %s, %d MB: %s\r\n", g_tools[i].what, g_tools[i].mb, g_tools[i].url);
        DownloadProgress progress;
        progress.what = g_tools[i].what;
        HRESULT hr = URLDownloadToFileA(NULL, g_tools[i].url, zip, 0, &progress);
        set_detail("");
        if (g_cancel) {
            DeleteFileA(zip);
            return 0;
        }
        if (FAILED(hr)) {
            post_log("The download failed (error %08lX). Check the internet connection, then press Build again.\r\n", hr);
            return 0;
        }
        if (!hash_file(zip, BCRYPT_SHA256_ALGORITHM, 32, hex) || strcmp(hex, g_tools[i].sha256)) {
            DeleteFileA(zip);
            post_log("%s did not match its checksum, so it was not used (a broken download, or not the file it should "
                     "be). Press Build to try again.\r\n", g_tools[i].what);
            return 0;
        }
        snprintf(cmd, sizeof cmd, "\"%s\" -xf \"%s\" -C \"%s\"", tar, zip, dir);
        set_detail("unpacking");
        DWORD code = run(cmd);
        set_detail("");
        DeleteFileA(zip);
        if (code || !exists(check)) {
            if (!g_cancel) post_log("Unpacking %s failed.\r\n", g_tools[i].what);
            return 0;
        }
        FILE *mark = fopen(ok, "w");
        if (mark) fclose(mark);
    }
    /* the embeddable Python finds modules only where its ._pth says: its own folder (capstone) and tools\ */
    snprintf(check, sizeof check, "%s\\build\\tools\\python-3.12.10\\python312._pth", g_root);
    FILE *f = fopen(check, "w");
    if (!f) return 0;
    fputs("python312.zip\n.\n../../../tools\n", f);
    fclose(f);
    post_log("The build tools are ready (build\\tools).\r\n");
    return 1;
}

static DWORD WINAPI build_thread(void *arg) {
    (void)arg;
    char game[MAX_PATH], xbe[MAX_PATH], cmd[2048], hex[41];
    int ok = 0;
    game_dir(game);
    snprintf(xbe, sizeof xbe, "%s\\Forza.xbe", game);
    step(0);
    if (!fetch_tools()) goto done;
    if (is_iso(g_source)) {
        step(1);
        char have[41] = "";
        if (!exists(xbe) || !sha1_file(xbe, have) || strcmp(have, XBE_SHA1)) {
            post_log("Extracting the ISO (3.6 GB)...\r\n");
            snprintf(cmd, sizeof cmd, "%s tools\\xiso_extract.py \"%s\" game", PYTHON, g_source);
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
        snprintf(cmd, sizeof cmd, "%s tools\\find_functions.py \"%s\" analysis\\Forza.functions.json", PYTHON, xbe);
        if (run(cmd)) goto done;
    }
    if (!exists("analysis\\Forza.symbols.json")) {
        if (run(PYTHON " tools\\oovpa_pp.py")) goto done;
        snprintf(cmd, sizeof cmd, "%s tools\\find_symbols.py \"%s\" build\\oovpa third_party\\nxdk\\xboxkrnl.exe.def "
                 "analysis\\Forza.symbols.json", PYTHON, xbe);
        if (run(cmd)) goto done;
    }
    step(4);
    snprintf(cmd, sizeof cmd, "%s tools\\lift.py \"%s\" analysis\\Forza.functions.json recomp --hle runtime\\hle.txt "
             "--symbols analysis\\Forza.symbols.json", PYTHON, xbe);
    if (run(cmd)) goto done;
    step(5);
    snprintf(cmd, sizeof cmd, "%s tools\\build.py --zig", PYTHON);
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
    if (h) TerminateJobObject(h, 1);
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
        /* NOCHANGEDIR: the dialog would make the ISO's folder our current one, and the build's analysis\ went there
         * while its steps run in the root (GitHub #5, #9) */
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
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
static int g_page;      /* 0 main, 1 settings, 2 what's new */
static int g_details;   /* the build's log shown */
static int g_hover = -1, g_press = -1;
static int g_focus = -1, g_pad_mode; /* the gamepad's or keyboard's item, and whether its ring shows (pad_poll) */
static ULONGLONG g_anim; /* GetTickCount64 at start, for the progress shimmer */

/* clickable things: ids; the rectangles of this frame (DIPs) */
enum {
    B_ISO = 1, B_FOLDER, B_BUILD, B_PLAY, B_SETTINGS, B_DETAILS, B_CANCEL, B_DONE, B_SAVES, B_LOGS, B_GAME, B_REBUILD,
    B_CHANGE, B_UPDATE, B_CHANGELOG, B_SET0 = 100 /* + setting: its value; + 50: its left arrow */
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
        char pct[16], what[256], detail[96];
        snprintf(pct, sizeof pct, "%d%%", s * 100 / STEPS);
        get_detail(detail, sizeof detail);
        snprintf(what, sizeof what, "%s%s%s  (step %d of %d)", g_step_names[s], detail[0] ? ": " : "", detail, s + 1, STEPS);
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
    else text("Nothing to install. The first build downloads its tools (about 95 MB) and the upscalers.",
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
        hit(id, val);
        hit(id + 50, D2D1::RectF(val.left, val.top, val.left + 40, val.bottom)); /* the left arrow goes back: after val,
                                                                                    since hit_test takes the last */
    }
}

static const Hit *hit_of(int id); /* the gamepad's focus (below) */
static void focus_default(void);

/* page 2: this version's changes, once, the first start after an update (g_news) */
static void draw_news(float w, float h, float x0) {
    float top = 96, cw = w - x0 * 2 > 760 ? 760 : w - x0 * 2;
    text("What's new in " RECOMP_VERSION, g_f_head, D2D1::RectF(x0, top, x0 + cw - 160, top + 64), C_TEXT);
    button(B_DONE, "Done", D2D1::RectF(x0 + cw - 140, top + 12, x0 + cw, top + 56), 0);
    D2D1_RECT_F card = D2D1::RectF(x0, top + 84, x0 + cw, h - 104);
    rounded(card, 14, rgb(0x0E0A0B, 0.92f), rgb(0xFFFFFF, 0.08f));
    int n = MultiByteToWideChar(CP_UTF8, 0, g_news, -1, NULL, 0);
    wchar_t *wt = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (wt && MultiByteToWideChar(CP_UTF8, 0, g_news, -1, wt, n)) {
        g_f_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_f_body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); /* text() sets it back for the rest */
        g_rt->DrawText(wt, n - 1, g_f_body, D2D1::RectF(card.left + 24, card.top + 18, card.right - 24, card.bottom - 14),
                       brush(rgb(C_TEXT)), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    free(wt);
    button(B_CHANGELOG, "Every change: CHANGELOG.md", D2D1::RectF(x0, h - 100, x0 + 230, h - 60), 2);
}

static void draw(void) {
    if (!g_rt || !g_brush) return;
    g_nhits = 0;
    g_rt->BeginDraw();
    D2D1_SIZE_F sz = g_rt->GetSize();
    float w = sz.width, h = sz.height, x0 = w > 1000 ? 72.0f : 40.0f;
    backdrop(w, h);
    logo(x0, 28);
    if (g_update_tag[0]) { /* a newer release: the notes and its page on a click (update_offer) */
        char msg[96];
        snprintf(msg, sizeof msg, "Version %s is out: see what's new", g_update_tag);
        float tw = text_width(msg, g_f_label) + 24;
        D2D1_RECT_F r = D2D1::RectF(w - x0 - tw, 22, w - x0, 54);
        rounded(r, 8, rgb(C_AMBER, g_hover == B_UPDATE ? 0.22f : 0.12f), rgb(C_AMBER, 0.5f));
        text(msg, g_f_label, r, C_AMBER, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(B_UPDATE, r);
    }
    float top = h * 0.24f < 150 ? 150 : h * 0.24f;
    if (g_page == 1) draw_settings(w, h, x0, top);
    else if (g_page == 2 && g_news) draw_news(w, h, x0);
    else draw_main(w, h, x0, top);
    /* the footer */
    g_rt->FillRectangle(D2D1::RectF(0, h - 48, w, h), brush(rgb(0x070506, 0.92f)));
    g_rt->DrawLine(D2D1::Point2F(0, h - 48), D2D1::Point2F(w, h - 48), brush(rgb(0xFFFFFF, 0.06f)), 1);
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
    /* up to the links (it wraps in a narrow window rather than run under them) */
    text(RECOMP_NAME " " RECOMP_VERSION "  \xC2\xB7  Made with the help of AI  \xC2\xB7  Not affiliated with Microsoft or "
         "Turn 10 Studios", g_f_small, D2D1::RectF(x0, h - 48, x - 8, h), C_FAINT);
    if (g_pad_mode) { /* the gamepad's or keyboard's item: a ring round it (a page change moves it to the main action) */
        if (!hit_of(g_focus)) focus_default();
        const Hit *f = hit_of(g_focus);
        if (f) rounded(D2D1::RectF(f->r.left - 4, f->r.top - 4, f->r.right + 4, f->r.bottom + 4), 12, rgb(0, 0),
                       rgb(0xFFFFFF, 0.92f), 2.5f);
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
    case B_UPDATE: update_offer(); break;
    case B_CHANGELOG: {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\CHANGELOG.md", g_root);
        ShellExecuteA(g_wnd, "open", p, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
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

/* ---------------- a gamepad or the keyboard (#13: the launcher with a controller alone) ----------------
 * A focus ring moves over what can be clicked (the d-pad, the left stick, the arrow keys); A or Enter clicks it,
 * B or Esc goes back. On a setting, left and right step its value. The mouse hides the ring. */
static const Hit *hit_of(int id) {
    for (int i = g_nhits - 1; i >= 0; i--)
        if (g_hits[i].id == id) return &g_hits[i];
    return NULL;
}
static int navigable(int id) { return id < B_SET0 + 50; } /* not a setting's left arrow: left and right step it */

static void focus_default(void) { /* the page's main action, else its first item */
    static const int first[] = {B_PLAY, B_BUILD, B_DONE};
    for (int i = 0; i < 3; i++)
        if (hit_of(first[i])) {
            g_focus = first[i];
            return;
        }
    for (int i = 0; i < g_nhits; i++)
        if (navigable(g_hits[i].id)) {
            g_focus = g_hits[i].id;
            return;
        }
}

/* to the nearest item whose middle lies that way from the focused one's, sideways distance counting double */
static void focus_move(int dx, int dy) {
    const Hit *f = hit_of(g_focus);
    if (!f) {
        focus_default();
        return;
    }
    float fx = (f->r.left + f->r.right) / 2, fy = (f->r.top + f->r.bottom) / 2, best_score = 1e30f;
    int best = -1;
    for (int i = 0; i < g_nhits; i++) {
        const Hit *c = &g_hits[i];
        if (c->id == g_focus || !navigable(c->id)) continue;
        float ox = (c->r.left + c->r.right) / 2 - fx, oy = (c->r.top + c->r.bottom) / 2 - fy;
        float along = ox * dx + oy * dy, side = fabsf(ox * dy) + fabsf(oy * dx);
        if (along > 1 && along + 2 * side < best_score) best_score = along + 2 * side, best = c->id;
    }
    if (best >= 0) g_focus = best;
}

static void pad_nav(int dx, int dy) {
    int shown = g_pad_mode && hit_of(g_focus);
    g_pad_mode = 1;
    if (!shown) focus_default(); /* the first press shows where the ring is */
    else if (dx && g_page == 1 && g_focus >= B_SET0 && g_focus < B_SET0 + 50) setting_cycle(g_focus - B_SET0, dx);
    else focus_move(dx, dy);
    InvalidateRect(g_wnd, NULL, FALSE);
}
static void pad_choose(void) {
    int shown = g_pad_mode && hit_of(g_focus);
    g_pad_mode = 1;
    if (!shown) focus_default();
    else click(g_focus);
    InvalidateRect(g_wnd, NULL, FALSE);
}
static void pad_back(void) { /* never Cancel: a build stops only on purpose */
    g_pad_mode = 1;
    if (g_page) g_page = 0;
    else if (g_details && !g_busy) g_details = 0, place_log();
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* the first connected pad, each timer tick: presses, and a held direction repeating after 0.4 s every 0.12 s */
static void pad_poll(void) {
    static DWORD slot, next_scan;
    static WORD last;
    static ULONGLONG repeat_at;
    XINPUT_STATE st;
    WORD b = 0;
    if (XInputGetState(slot, &st) != ERROR_SUCCESS) {
        if (GetTickCount() < next_scan) return; /* an empty slot is slow to ask: look for a pad twice a second */
        next_scan = GetTickCount() + 500;
        for (slot = 0; slot < 4 && XInputGetState(slot, &st) != ERROR_SUCCESS; slot++) {}
        if (slot == 4) {
            slot = 0, last = 0;
            return;
        }
    }
    b = st.Gamepad.wButtons;
    const SHORT dead = 16000; /* the stick as a d-pad */
    if (st.Gamepad.sThumbLX < -dead) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX > dead) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (st.Gamepad.sThumbLY > dead) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -dead) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    WORD down = b & ~last;
    ULONGLONG now = GetTickCount64();
    if (down & dirs) repeat_at = now + 400;
    else if ((b & dirs) && (b & dirs) == (last & dirs) && now >= repeat_at) down |= b & dirs, repeat_at = now + 120;
    last = b;
    if (down & XINPUT_GAMEPAD_DPAD_UP) pad_nav(0, -1);
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) pad_nav(0, 1);
    if (down & XINPUT_GAMEPAD_DPAD_LEFT) pad_nav(-1, 0);
    if (down & XINPUT_GAMEPAD_DPAD_RIGHT) pad_nav(1, 0);
    if (down & XINPUT_GAMEPAD_A) pad_choose();
    if (down & XINPUT_GAMEPAD_B) pad_back();
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
        static LPARAM last_pos = -1;
        if (lp != last_pos && last_pos != -1 && g_pad_mode) g_pad_mode = 0, InvalidateRect(h, NULL, FALSE); /* the mouse again */
        last_pos = lp;
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
        if (GetForegroundWindow() == h) pad_poll(); /* not a pad meant for the game or another window */
        return 0;
    case WM_KEYDOWN: /* the keyboard moves the same focus */
        switch (wp) {
        case VK_UP: pad_nav(0, -1); return 0;
        case VK_DOWN: pad_nav(0, 1); return 0;
        case VK_LEFT: pad_nav(-1, 0); return 0;
        case VK_RIGHT: pad_nav(1, 0); return 0;
        case VK_RETURN:
        case VK_SPACE: pad_choose(); return 0;
        case VK_ESCAPE:
        case VK_BACK: pad_back(); return 0;
        }
        break;
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
    case WM_STEP:
    case WM_UPDATE: InvalidateRect(h, NULL, FALSE); return 0;
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
    { /* the first start of this version: what's new, if it came over an older one (launcher.ini there already) */
        char seen[32];
        GetPrivateProfileStringA("launcher", "seen_version", "", seen, sizeof seen, g_ini);
        if (strcmp(seen, RECOMP_VERSION)) {
            if (exists(g_ini) && (g_news = whats_new())) g_page = 2;
            WritePrivateProfileStringA("launcher", "seen_version", RECOMP_VERSION, g_ini);
        }
    }

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
    update_start();
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
