/* Texture packs: replacement images for the game's textures, found by content, and the dump they are made from.
 *
 * A texture's key is a hash of its guest bytes (palette included), colour format and size, so the same picture is the
 * same key wherever it is loaded. Every folder under textures\ next to the exe except dump\ is a pack: files named
 * <16 hex digits of the key>.png or .jpg, any size (tools/texpack.py upscales a dump 4x). They load on worker
 * threads through WIC with a box-filtered mip chain; the game shows the original until the replacement is ready,
 * which the renderer waits for a little (texpack_get's wait_ms) so that most never show at all.
 * A pack's aliases.txt lines "<key> <key>" give the first key the second's replacement: the lower mip levels the game
 * draws as textures of their own get the whole texture's picture (tools/texextract.py). Folders under
 * textures\icons\<device>\ hold replacements that apply only while that device was the last one used (the button
 * prompts). With the dump on, each texture's top level is written once to textures\dump\<key>.dds.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>

#include "pgraph.h"
#include "texpack.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define MAX_FILES 262144 /* open addressing over keys: at most half full */
typedef struct Entry {
    uint64_t key;
    int device;                       /* -1: every device, else the DEV_* its icon set is for */
    volatile LONG state;              /* 0 on disk, 1 queued, 2 ready, 3 failed */
    char *path;
    ID3D11ShaderResourceView *volatile srv;
    int logged;
    struct Entry *target; /* an alias: shows this one's picture */
    uint64_t used;        /* g_clock when last asked for */
    uint32_t bytes;       /* of its texture, mips included */
} Entry;
/* the budget for loaded replacements (a whole track's at 4x is ~1.7 GB): a third of the GPU's own memory, between
 * 512 MB and 4 GB (texpack_init); TEXBUDGET=<MB> sets it for testing */
static uint64_t g_budget = 4ull << 30;
static uint32_t g_loads, g_evictions;
static SRWLOCK g_lock = SRWLOCK_INIT; /* entries' state, srv and use, between the renderer and the worker */
static uint64_t g_loaded, g_clock;
static char *g_alias_files[64]; /* read once every picture is known */
static int g_alias_device[64], g_nalias_files;
static Entry *g_files; /* MAX_FILES * 2 slots */
static int g_nfiles;
static ID3D11Device *g_dev;
static char g_root[MAX_PATH];  /* ...\textures */
static int g_enabled, g_dump;
static SRWLOCK g_queue_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_queue_cv = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE g_done_cv = CONDITION_VARIABLE_INIT; /* a load finished (under g_lock) */
static Entry *g_queue[4096];
static int g_queue_head, g_queue_tail;

static Entry *slot(uint64_t key, int device) {
    for (uint32_t i = (uint32_t)((key ^ (key >> 32)) + device * 0x9E3779B1u) % (MAX_FILES * 2);; i = (i + 1) % (MAX_FILES * 2))
        if (!g_files[i].path || (g_files[i].key == key && g_files[i].device == device)) return &g_files[i];
}

/* the replacement files under dir and its folders; at the root of textures\ the dump and the icons are not packs */
static void scan(const char *dir, int device, int root) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] != '.' && !(root && (!_stricmp(fd.cFileName, "dump") || !_stricmp(fd.cFileName, "icons"))))
                scan(path, device, 0);
            continue;
        }
        if (!_stricmp(fd.cFileName, "aliases.txt") && g_nalias_files < 64) {
            g_alias_device[g_nalias_files] = device, g_alias_files[g_nalias_files++] = _strdup(path);
            continue;
        }
        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot || dot - fd.cFileName != 16 || (_stricmp(dot, ".png") && _stricmp(dot, ".jpg"))) continue;
        char *end;
        uint64_t key = _strtoui64(fd.cFileName, &end, 16);
        if (end != dot || g_nfiles == MAX_FILES) continue;
        Entry *e = slot(key, device);
        if (!e->path) g_nfiles++;
        free(e->path);
        e->key = key, e->device = device, e->path = _strdup(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* the image at path as BGRA with a full mip chain, as a shader resource */
static ID3D11ShaderResourceView *load(IWICImagingFactory *wic, const char *path, uint32_t *bytes) {
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    UINT w = 0, h = 0;
    uint32_t *mip[14] = {0};
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(wic, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(wic, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && w && h && w <= 16384 && h <= 16384 &&
        (mip[0] = malloc((size_t)w * h * 4)) &&
        SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)mip[0]))) {
        D3D11_SUBRESOURCE_DATA init[14];
        UINT n = 0, lw = w, lh = h;
        for (;; n++) {
            init[n] = (D3D11_SUBRESOURCE_DATA){mip[n], lw * 4, 0};
            if ((lw == 1 && lh == 1) || n == 13) break;
            UINT nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1; /* box filter; an odd edge drops its last row */
            if (!(mip[n + 1] = malloc((size_t)nw * nh * 4))) break;
            const uint8_t *s = (const uint8_t *)mip[n];
            uint8_t *d = (uint8_t *)mip[n + 1];
            for (UINT y = 0; y < nh; y++)
                for (UINT x = 0; x < nw; x++)
                    for (int ch = 0; ch < 4; ch++) {
                        UINT x0 = x * 2 < lw ? x * 2 : lw - 1, x1 = x * 2 + 1 < lw ? x * 2 + 1 : x0;
                        UINT y0 = y * 2 < lh ? y * 2 : lh - 1, y1 = y * 2 + 1 < lh ? y * 2 + 1 : y0;
                        d[(y * nw + x) * 4 + ch] = (uint8_t)((s[(y0 * lw + x0) * 4 + ch] + s[(y0 * lw + x1) * 4 + ch] +
                                                              s[(y1 * lw + x0) * 4 + ch] + s[(y1 * lw + x1) * 4 + ch] + 2) / 4);
                    }
            lw = nw, lh = nh;
        }
        D3D11_TEXTURE2D_DESC td = {w, h, n + 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D *tex;
        *bytes = w * h * 4 / 3 * 4;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, init, &tex))) { /* the device is free-threaded */
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tex, NULL, &srv);
            ID3D11Texture2D_Release(tex);
        }
    }
    for (int i = 0; i < 14; i++) free(mip[i]);
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (!srv) fprintf(stderr, "[texpack] cannot load %s\n", path);
    return srv;
}

static DWORD WINAPI worker(void *arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic))) {
        fprintf(stderr, "[texpack] no WIC; texture packs are off\n");
        g_enabled = 0;
        return 0;
    }
    for (;;) {
        AcquireSRWLockExclusive(&g_queue_lock);
        while (g_queue_head == g_queue_tail) SleepConditionVariableSRW(&g_queue_cv, &g_queue_lock, INFINITE, 0);
        Entry *e = g_queue[g_queue_head++ % 4096];
        ReleaseSRWLockExclusive(&g_queue_lock);
        uint32_t bytes = 0;
        LARGE_INTEGER t0, t1, hz;
        QueryPerformanceCounter(&t0);
        ID3D11ShaderResourceView *srv = load(wic, e->path, &bytes);
        QueryPerformanceCounter(&t1), QueryPerformanceFrequency(&hz);
        static int log = -1; /* TEXTIME=1: each load's time and what still waits */
        if (log < 0) log = getenv("TEXTIME") != NULL;
        if (log)
            fprintf(stderr, "[texpack] %llu ms: loaded %u KB in %.1f ms, %d waiting\n", GetTickCount64(), bytes >> 10,
                    (t1.QuadPart - t0.QuadPart) * 1000.0 / hz.QuadPart, g_queue_tail - g_queue_head);
        AcquireSRWLockExclusive(&g_lock);
        e->srv = srv, e->bytes = bytes, e->used = ++g_clock;
        e->state = srv ? 2 : 3;
        g_loaded += bytes;
        g_loads++;
        WakeAllConditionVariable(&g_done_cv);
        while (g_loaded > g_budget) { /* the least recently used goes; the renderer keeps what it still holds */
            Entry *lru = NULL;
            for (int i = 0; i < MAX_FILES * 2; i++)
                if (g_files[i].state == 2 && !g_files[i].target && (!lru || g_files[i].used < lru->used)) lru = &g_files[i];
            if (!lru || lru == e) break;
            ID3D11ShaderResourceView_Release(lru->srv);
            lru->srv = NULL, lru->state = 0;
            g_evictions++;
            g_loaded -= lru->bytes;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void texpack_init(ID3D11Device *dev, int enabled, int dump) {
    g_dev = dev, g_enabled = enabled, g_dump = dump;
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc))) {
        uint64_t third = desc.DedicatedVideoMemory / 3;
        g_budget = third < (512ull << 20) ? 512ull << 20 : third > (4ull << 30) ? 4ull << 30 : third;
    }
    if (ad) IDXGIAdapter_Release(ad);
    if (dx) IDXGIDevice_Release(dx);
    if (getenv("TEXBUDGET")) g_budget = (uint64_t)atoi(getenv("TEXBUDGET")) << 20;
    GetModuleFileNameA(NULL, g_root, MAX_PATH);
    char *slash = strrchr(g_root, '\\');
    strcpy(slash ? slash + 1 : g_root, "textures");
    g_files = calloc(MAX_FILES * 2, sizeof *g_files);
    scan(g_root, -1, 1);
    static const char *const devices[] = {"keyboard", "xbox", "playstation", "other"}; /* DEV_* order */
    for (int d = 0; d < 4; d++) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\icons\\%s", g_root, devices[d]);
        scan(dir, d, 0);
    }
    int aliases = 0;
    for (int i = 0; i < g_nalias_files; i++) {
        FILE *f = fopen(g_alias_files[i], "r");
        unsigned long long from, to;
        while (f && fscanf(f, "%llx %llx", &from, &to) == 2 && g_nfiles < MAX_FILES) {
            Entry *t = slot(to, g_alias_device[i]), *e;
            if (!t->path || t->target || (e = slot(from, g_alias_device[i]))->path) continue; /* a picture of its own wins */
            e->key = from, e->device = g_alias_device[i], e->path = t->path, e->target = t;
            g_nfiles++, aliases++;
        }
        if (f) fclose(f);
    }
    /* one worker loaded a race's ~290 replacements one after another, ~160 of them in its first second, so the
     * originals showed for up to half a second: one for every two cores, 2 to 8 */
    DWORD cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), n = cores / 2 < 2 ? 2 : cores / 2 > 8 ? 8 : cores / 2;
    if (getenv("TEXWORKERS") && atoi(getenv("TEXWORKERS")) > 0) n = (DWORD)atoi(getenv("TEXWORKERS")); /* testing */
    if (g_nfiles)
        fprintf(stderr, "[texpack] %d replacement textures (%d aliases) in %s, %llu MB of them at once, %lu loaders\n",
                g_nfiles, aliases, g_root, (unsigned long long)(g_budget >> 20), n);
    for (DWORD i = 0; i < n; i++) CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

void texpack_enable(int on) { g_enabled = on; }

/* replacements loaded and dropped so far, and how many wait to load (the renderer's hitch lines) */
void texpack_stats(uint32_t *loads, uint32_t *evictions, int *queued) {
    *loads = g_loads, *evictions = g_evictions, *queued = g_queue_tail - g_queue_head;
}
void texpack_set_dump(int on) { g_dump = on; }

/* Finding the texture behind a glitch: creating build/texkeys.now (one hex key a line, or "all") limits replacements to
 * those keys until the next one; the renderer looks for it four times a second and every texture asks again */
static uint64_t g_allow[4096];
static int g_nallow = -1;
int texpack_poll_keys(void) {
    FILE *f = fopen("build/texkeys.now", "r");
    if (!f) return 0;
    char line[64];
    unsigned long long k;
    g_nallow = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "all", 3)) g_nallow = -1;
        else if (g_nallow >= 0 && g_nallow < 4096 && sscanf(line, "%llx", &k) == 1) g_allow[g_nallow++] = k;
    fclose(f);
    DeleteFileA("build/texkeys.now");
    fprintf(stderr, "[texpack] texkeys.now: %d keys allowed\n", g_nallow);
    return 1;
}

/* 0: no replacement; 1: one is loading (asked again later); 2: *srv is it, a reference the caller now holds.
 * A loading one is waited for up to wait_ms. */
int texpack_get(uint64_t key, int device, ID3D11ShaderResourceView **srv, DWORD wait_ms) {
    if (!g_enabled || !g_nfiles) return 0;
    if (g_nallow >= 0) {
        int ok = 0;
        for (int i = 0; i < g_nallow && !ok; i++) ok = g_allow[i] == key;
        if (!ok) return 0;
    }
    Entry *e = slot(key, device);
    if (!e->path && device >= 0) return texpack_get(key, -1, srv, wait_ms); /* no icon for this device: the pack's */
    if (!e->path) return 0;
    if (e->target) e = e->target;
    AcquireSRWLockExclusive(&g_lock);
    if (e->state == 0) {
        AcquireSRWLockExclusive(&g_queue_lock);
        if (g_queue_tail - g_queue_head < 4096) {
            e->state = 1;
            g_queue[g_queue_tail++ % 4096] = e;
            WakeConditionVariable(&g_queue_cv);
        }
        ReleaseSRWLockExclusive(&g_queue_lock);
    }
    for (ULONGLONG until = GetTickCount64() + wait_ms, now; e->state == 1 && (now = GetTickCount64()) < until;)
        SleepConditionVariableSRW(&g_done_cv, &g_lock, (DWORD)(until - now), 0);
    int r = e->state == 2 ? 2 : e->state == 3 ? 0 : 1;
    if (r == 2) {
        *srv = e->srv;
        ID3D11ShaderResourceView_AddRef(e->srv);
        e->used = ++g_clock;
        static int log = -1; /* TEXLOG=1: name each replacement the first time it is used */
        if (log < 0) log = getenv("TEXLOG") != NULL;
        if (log && !e->logged) e->logged = 1, fprintf(stderr, "[texpack] %016llX -> %s\n", (unsigned long long)key, e->path);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

int texpack_has_device_icons(uint64_t key) {
    if (!g_nfiles) return 0;
    for (int d = 0; d < 4; d++)
        if (slot(key, d)->path) return 1;
    return 0;
}

/* textures\dump\<key>.dds: the top level as decoded, block-compressed (DXT1/3/5) or BGRA */
void texpack_dump(uint64_t key, const TexImage *img) {
    if (!g_dump || img->faces != 1 || img->depth != 1) return;
    static int made;
    char path[MAX_PATH];
    if (!made) {
        CreateDirectoryA(g_root, NULL);
        snprintf(path, sizeof path, "%s\\dump", g_root);
        CreateDirectoryA(path, NULL);
        made = 1;
    }
    snprintf(path, sizeof path, "%s\\dump\\%016llX.dds", g_root, (unsigned long long)key);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t hd[32] = {0x20534444, 124, 0x1007 /* caps, height, width, pixel format */, img->height, img->width};
    uint32_t bc = img->format != TEX_BGRA8, size = img->slice[0];
    hd[2] |= bc ? 0x80000 : 0x8; /* linear size, or pitch */
    hd[5] = bc ? size : img->pitch[0];
    hd[19] = 32; /* DDS_PIXELFORMAT */
    if (bc) hd[20] = 4, hd[21] = img->format == TEX_BC1 ? 0x31545844 : img->format == TEX_BC2 ? 0x33545844 : 0x35545844;
    else hd[20] = 0x41, hd[22] = 32, hd[23] = 0xFF0000, hd[24] = 0xFF00, hd[25] = 0xFF, hd[26] = 0xFF000000u;
    hd[27] = 0x1000; /* DDSCAPS_TEXTURE */
    fwrite(hd, 4, 32, f);
    fwrite(img->level[0], 1, size, f);
    fclose(f);
}
