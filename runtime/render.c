/* Host renderer (Direct3D 11) for the NV2A interpreter.
 *
 * Every NV2A colour surface (by guest address) has a host render target RENDER_SCALE times its native size; that
 * scale is the resolution enhancement. A texture whose address is a render target samples the host target
 * directly (render-to-texture never round-trips through guest memory). Shaders are generated as HLSL
 * (shaders.c) only for GPU state not seen before, compiled at runtime and cached by source hash. All calls happen with the guest lock held, so the
 * immediate context is never used from two threads at once.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>

#include "pgraph.h"
#include "icons.h"
#include "texpack.h"
#include "video.h"
#include "version.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static int g_scale = DEF_RENDERSCALE; /* RENDER SCALE (recomp.ini renderscale, RENDERSCALE), read at startup */
#define RENDER_SCALE g_scale          /* host pixels per NV2A pixel in each direction */
#define MAX_TARGETS 64
#define MAX_SHADERS 4096
#define TEX_SETS 1024 /* the decoded-texture cache: a texture's address picks a set of TEX_WAYS slots */
#define TEX_WAYS 8
#define MAX_TEXTURES (TEX_SETS * TEX_WAYS)
static uint32_t g_compiles_made, g_tex_uploads, g_targets_made; /* totals, for the hitch lines (present) */
static int64_t g_tex_ticks;                                     /* QPC time decoding and uploading textures */
#define MAX_STATES 256

static HWND g_hwnd;
static ID3D11Device *g_dev;
static ID3D11DeviceContext1 *g_ctx;
static IDXGISwapChain1 *g_swap;
static int g_wide;                       /* the picture is 16:9 (widescreen or an HD mode), else 4:3 */
static float g_widen = 1;                /* ULTRAWIDE: the picture is this much wider than 16:9 (1: off) */
static int g_texwait_ms = 100;           /* resolve(): a frame's wait for replacements (TEXWAIT=<ms>, testing) */
static int g_win_w = 1280, g_win_h = 960; /* first client size: as saved, else 960 lines at the picture's aspect */

static void fail(const char *what, HRESULT hr) {
    fprintf(stderr, "[render] %s failed (%08lX)\n", what, (unsigned long)hr);
    fatal_exit(7);
}

static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define FNV0 0xCBF29CE484222325ull

/* ---------------- window and device ---------------- */

static int g_full; /* the window's thread keeps it */
#define FRAMES (WS_OVERLAPPEDWINDOW | WS_POPUP)

/* borderless fullscreen on the window's monitor, or back to the window as it was (on the window's thread) */
static void set_fullscreen(HWND h, int on) {
    static WINDOWPLACEMENT was = {sizeof was};
    LONG style = GetWindowLongA(h, GWL_STYLE) & ~FRAMES;
    if (on && !g_full) {
        MONITORINFO mi = {sizeof mi};
        GetWindowPlacement(h, &was);
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        g_full = 1;
        SetWindowLongA(h, GWL_STYLE, style | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!on && g_full) {
        g_full = 0;
        SetWindowLongA(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &was);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void render_fullscreen(int on) { PostMessageA(g_hwnd, WM_APP, on, 0); }
float render_widen(void) { return g_widen; }

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) ExitProcess(0);
    if (msg == WM_APP) set_fullscreen(h, (int)wp);
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN) { /* Alt+Enter, remembered like the menu's FULLSCREEN */
        int on = !g_full;
        set_fullscreen(h, on);
        cfg_save("fullscreen", on);
        return 0;
    }
    if (msg == WM_SYSCHAR && wp == '\r') return 0; /* no beep for Alt+Enter */
    if (msg == WM_EXITSIZEMOVE && !g_full && !IsZoomed(h)) {
        RECT r; /* the size and place the player left the window at, for the next start */
        POINT p = {0, 0};
        GetClientRect(h, &r), ClientToScreen(h, &p);
        cfg_save("window_w", r.right), cfg_save("window_h", r.bottom);
        cfg_save("window_x", p.x), cfg_save("window_y", p.y);
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* the window lives on its own thread so its message loop never waits on the guest */
static DWORD WINAPI window_thread(void *ready) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(1)); /* the emblem (launcher/redline.rc) */
    wc.lpszClassName = "RedlineRecompiled";
    RegisterClassA(&wc);
    int test = getenv("TESTKEYS") != NULL; /* driven by a test script: shown without taking the focus */
    /* where the picture was left, if that is still on a screen; else Windows' place */
    RECT r = {cfg_saved("window_x", 0), cfg_saved("window_y", 0)};
    r.right = r.left + g_win_w, r.bottom = r.top + g_win_h;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (cfg_saved("window_x", -99999) != -99999 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = r.left, y = r.top;
    OffsetRect(&r, -r.left, -r.top);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    if (x != CW_USEDEFAULT) x += r.left, y += r.top; /* the frame around that place */
    g_hwnd = CreateWindowA("RedlineRecompiled", RECOMP_NAME " " RECOMP_VERSION, WS_OVERLAPPEDWINDOW | (test ? 0 : WS_VISIBLE),
                           x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (g_hwnd && test) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    if (g_hwnd && !test && cfg_get("FULLSCREEN", "fullscreen", 0)) set_fullscreen(g_hwnd, 1);
    SetEvent((HANDLE)ready);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

static int g_wbuf = 1; /* w-buffered depth (WBUFFER=0: z/w as before; creating build/wbuf.now switches it) */
static ID3D11Buffer *g_cb;
/* A draw's vertices, indices and constants are appended to rings, mapped NO_OVERWRITE; only a full ring is
 * discarded. A buffer discarded every draw had the driver find it new memory each time, much of a draw's cost. */
typedef struct {
    ID3D11Buffer *buf;
    UINT size, used;
    ID3D11ShaderResourceView *srv; /* g_vring: the vertex shader reads it as bytes */
} Ring;
static Ring g_vring, g_iring, g_kring;
static int g_vring_ok; /* the driver takes NO_OVERWRITE on a buffer shaders read (D3D 11.1); else each draw discards */
static int g_kring_ok; /* the driver takes constant buffer offsets and NO_OVERWRITE on them (D3D 11.1); else g_cb */
static ID3D11ShaderResourceView *g_dummy_srv, *g_dummy_cube, *g_dummy_3d;

static ID3DBlob *compile(const char *src, const char *profile) {
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                            &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "[render] %s compile failed:\n%s\n--- source ---\n%s\n", profile,
                errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?", src);
        fprintf(stderr, "[render] a %s shader did not compile (its errors and source are above)\n", profile);
        fatal_exit(7);
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

void render_init(void) {
    vsh_selftest();
    { /* hash_bytes sees a one-byte change in its word part and in its tail */
        uint8_t b[40] = {0};
        uint64_t h = hash_bytes(b, sizeof b, 0), h1;
        b[17] = 1, h1 = hash_bytes(b, sizeof b, 0), b[17] = 0, b[38] = 1;
        if (h == h1 || h == hash_bytes(b, sizeof b, 0)) fail("hash_bytes self-test", 0);
    }
    texture_selftest();
    g_scale = cfg_get("RENDERSCALE", "renderscale", DEF_RENDERSCALE);
    g_scale = g_scale < 1 ? 1 : g_scale > 4 ? 4 : g_scale;
    render_anisotropy(cfg_get("ANISOTROPY", "anisotropy", DEF_ANISOTROPY));
    if (getenv("TEXWAIT")) g_texwait_ms = atoi(getenv("TEXWAIT"));
    render_fsr(cfg_get("FSR", "fsr", DEF_FSR));
    if (getenv("MVTEST")) render_motion(1); /* testing: motion vectors with no upscaler to use them */
    if (getenv("WBUFFER")) g_wbuf = atoi(getenv("WBUFFER")) != 0;
    g_wide = cfg_widescreen() || cfg_video_lines() >= 720; /* the HD modes are 16:9 */
    /* ULTRAWIDE (a 16:9 picture): 1 the primary monitor's aspect, n >= 100 an aspect of n / 100 (testing).
     * shortcut: the primary monitor's, not the one the window opens on; a window elsewhere gets black bars. */
    int uw = cfg_get("ULTRAWIDE", "ultrawide", 0);
    if (uw && g_wide) {
        float a = uw >= 100 ? uw / 100.0f : (float)GetSystemMetrics(SM_CXSCREEN) / GetSystemMetrics(SM_CYSCREEN);
        if (a > 16.0f / 9) g_widen = a * 9 / 16;
    }
    if (g_wide) g_win_w = (int)(g_win_h * 16 / 9 * g_widen);
    if (cfg_saved("window_w", 0) >= 160 && cfg_saved("window_h", 0) >= 120)
        g_win_w = cfg_saved("window_w", 0), g_win_h = cfg_saved("window_h", 0);
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, ready, 0, NULL);
    WaitForSingleObject(ready, INFINITE);
    if (!g_hwnd) fail("CreateWindow", GetLastError());

    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_1;
    /* the fast GPU, not whichever is adapter 0: on a PC with integrated graphics too (a laptop, or a Ryzen's iGPU
     * driving a second screen) the default can be the slow one */
    IDXGIFactory6 *f6 = NULL;
    IDXGIAdapter1 *fast = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&f6))) {
        const char *gpu = getenv("RENDER_GPU"); /* RENDER_GPU=low: the power-saving one (testing other GPUs) */
        IDXGIFactory6_EnumAdapterByGpuPreference(
            f6, 0, gpu && !strcmp(gpu, "low") ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            &IID_IDXGIAdapter1, (void **)&fast);
        IDXGIFactory6_Release(f6);
    }
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)fast, fast ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   0, &want, 1, D3D11_SDK_VERSION, &g_dev, NULL, &ctx);
    if (fast) IDXGIAdapter1_Release(fast);
    if (FAILED(hr)) fail("D3D11CreateDevice", hr);
    hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&g_ctx);
    if (FAILED(hr)) fail("ID3D11DeviceContext1", hr);
    ID3D11DeviceContext_Release(ctx);

    IDXGIDevice *dxgi_dev;
    IDXGIAdapter *adapter;
    IDXGIFactory2 *factory;
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxgi_dev);
    IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    DXGI_ADAPTER_DESC ad;
    if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad))) fprintf(stderr, "[render] GPU: %ls\n", ad.Description);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = g_win_w;
    sd.Height = g_win_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3; /* room for a frame queued while another waits for the display */
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, g_hwnd, &sd, NULL, NULL, &g_swap);
    if (FAILED(hr)) fail("CreateSwapChainForHwnd", hr);
    IDXGIFactory2_MakeWindowAssociation(factory, g_hwnd, DXGI_MWA_NO_ALT_ENTER); /* wndproc's own Alt+Enter */
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);


    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = 4096; /* >= sizeof(Constants), rounded to 16 */
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb);
    if (FAILED(hr)) fail("constant buffer", hr);
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt = {0};
    g_kring_ok = SUCCEEDED(ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof opt)) &&
                 opt.ConstantBufferOffsetting && opt.MapNoOverwriteOnDynamicConstantBuffer;
    g_vring_ok = opt.MapNoOverwriteOnDynamicBufferSRV;

    /* 1x1 black textures for unbound units and ones that cannot be decoded, one per kind the shaders declare */
    uint32_t black[6] = {0};
    D3D11_SUBRESOURCE_DATA init[6] = {{black, 4, 4}, {black, 4, 4}, {black, 4, 4}, {black, 4, 4}, {black, 4, 4}, {black, 4, 4}};
    D3D11_TEXTURE2D_DESC td = {1, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                               D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *t;
    ID3D11Device_CreateTexture2D(g_dev, &td, init, &t);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &g_dummy_srv);
    ID3D11Texture2D_Release(t);
    td.ArraySize = 6;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    ID3D11Device_CreateTexture2D(g_dev, &td, init, &t);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURECUBE};
    vd.TextureCube.MipLevels = 1;
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, &vd, &g_dummy_cube);
    ID3D11Texture2D_Release(t);
    D3D11_TEXTURE3D_DESC td3 = {1, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture3D *t3;
    ID3D11Device_CreateTexture3D(g_dev, &td3, init, &t3);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t3, NULL, &g_dummy_3d);
    ID3D11Texture3D_Release(t3);
    texpack_init(g_dev, cfg_get("TEXPACK", "texpack", 1), cfg_get("TEXDUMP", "texdump", 0));
    video_init(g_dev, (ID3D11DeviceContext *)g_ctx, cfg_get("VIDEOS", "videos", 1));
    fprintf(stderr, "[render] D3D11 window %dx%d (scale %d)\n", g_win_w, g_win_h, RENDER_SCALE);
}

/* ---------------- surfaces ---------------- */

typedef struct {
    uint32_t addr, w, h; /* guest address and size in guest pixels (AA included) */
    UINT hw, hh;         /* host size: RENDER_SCALE times (an AA'd screen half as wide), a screen's g_widen times as wide */
    int depth, wide;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
    uint32_t used; /* g_frame it was last drawn to or read */
    ID3D11Texture2D *mv; /* the scene's motion (screen UV, a frame ago minus now; R16G16F), made when first wanted */
    ID3D11RenderTargetView *mv_rtv;
    ID3D11ShaderResourceView *mv_srv;
} Target;
static Target g_targets[MAX_TARGETS];
static uint32_t g_frame; /* frames presented (defined with the textures) */

static Target *find_target(uint32_t addr, int depth) {
    for (int i = 0; i < MAX_TARGETS; i++)
        if (g_targets[i].tex && g_targets[i].addr == addr && g_targets[i].depth == depth) {
            g_targets[i].used = g_frame;
            return &g_targets[i];
        }
    return NULL;
}

/* The game locked a surface it rendered to read it: copy the host target back into guest memory (only the host
 * target was ever drawn). Forza saves a car's paint by rendering the livery into 512x512 and 1024x1024 A8R8G8B8
 * targets and compressing them to DXT5 itself, and the car's icon by copying 128x64 of the screen: they were saved
 * as zeros, a near-black car in races and My Cars and a black icon. fmt and size are the D3D resource's Format and
 * Size; 32-bit colour only. Each guest pixel is the average of the host pixels over it. */
static void render_readback_(uint32_t addr, uint32_t fmt, uint32_t size) {
    Target *t = find_target(addr, 0);
    uint32_t color = (fmt >> 8) & 0xFF, lin = color == 0x12 || color == 0x1E; /* LIN_A8R8G8B8, LIN_X8R8G8B8 */
    if (!t || !(lin || color == 0x06 || color == 0x07)) return;
    uint32_t w = lin ? (size & 0xFFF) + 1 : 1u << ((fmt >> 20) & 15), h = lin ? ((size >> 12) & 0xFFF) + 1 : 1u << ((fmt >> 24) & 15);
    uint32_t pitch = ((size >> 24) + 1) * 64;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(t->tex, &td);
    /* host pixels per guest pixel (render scale, AA); a wide screen target (ULTRAWIDE) gives its middle 16:9, the
     * picture the game drew its 640 columns for: the car's icon is copied off the screen */
    float fx = (float)td.Width / (t->wide ? g_widen : 1) / w, x_off = (td.Width - fx * w) / 2;
    uint32_t sy = td.Height / h;
    if (fx < 1 || !sy) return;
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
    ID3D11Texture2D *st;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) return;
    ID3D11DeviceContext1_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)t->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext1_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t sum[4] = {0}, i0 = (uint32_t)(x_off + x * fx), sx = (uint32_t)(x_off + (x + 1) * fx) - i0;
                for (uint32_t j = 0; j < sy; j++) {
                    const uint8_t *p = (const uint8_t *)m.pData + (size_t)(y * sy + j) * m.RowPitch + i0 * 4;
                    for (uint32_t i = 0; i < sx * 4; i++) sum[i & 3] += p[i];
                }
                uint8_t *d = g_mem + addr + (lin ? y * pitch + x * 4 : swizzle(x, y, 0, w, h, 1) * 4);
                for (int k = 0; k < 4; k++) d[k] = (uint8_t)(sum[k] / (sx * sy)); /* B8G8R8A8: A8R8G8B8's bytes */
            }
        ID3D11DeviceContext1_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        fprintf(stderr, "[render] target %08X read back (%ux%u%s)\n", addr, w, h, lin ? "" : ", swizzled");
        achieve_readback(w, h); /* a paint job saved draws its livery at 1024 */
    }
    ID3D11Texture2D_Release(st);
}
void render_readback(uint32_t addr, uint32_t fmt, uint32_t size) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    render_readback_(addr, fmt, size);
    gpu_leave();
}

static void release_target(Target *t) {
    if (t->mv_rtv) ID3D11RenderTargetView_Release(t->mv_rtv);
    if (t->mv_srv) ID3D11ShaderResourceView_Release(t->mv_srv);
    if (t->mv) ID3D11Texture2D_Release(t->mv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->dsv) ID3D11DepthStencilView_Release(t->dsv);
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    memset(t, 0, sizeof *t);
}

/* host target for a colour (depth = 0) or depth surface, created or resized on demand */
static Target *target(uint32_t addr, uint32_t w, uint32_t h, int depth) {
    Target *t = find_target(addr, depth);
    if (t && t->w >= w && t->h >= h) return t;
    if (t) {
        if (t->w > w) w = t->w;
        if (t->h > h) h = t->h;
        release_target(t);
    } else {
        /* a free slot, else the one unused longest: each race puts its surfaces somewhere new, and nothing tells us
         * when the game lets one go */
        for (int i = 0; i < MAX_TARGETS && !(t && !t->tex); i++)
            if (!g_targets[i].tex || !t || g_targets[i].used < t->used) t = &g_targets[i];
        if (t->tex && g_frame - t->used < 2) fail("too many render targets", 0);
        if (t->tex) fprintf(stderr, "[render] target %08X unused for %u frames goes\n", t->addr, g_frame - t->used);
        release_target(t);
    }
    /* ULTRAWIDE: the screen's targets (480 lines, 640 wide or 1280 with AA; in VIDEOMODE 720 / 1080 the HD back
     * buffers) hold the game's wider view, which it squeezes into their columns; everything else (shadow and cube
     * maps, bloom, liveries) keeps its shape */
    uint32_t hd_w = pgraph_hd_width();
    int screen = (h == 480 && w >= 640) || (hd_w && h == hd_w * 9 / 16 && w >= hd_w), wide = g_widen > 1 && screen;
    /* The game's anti-aliasing draws the screen twice as wide (1280 or 3840 columns) and averages pairs; from render
     * scale 2 up, which samples every pixel several times already, that half of the width is left out: 1080i at
     * scale 4 drew 15360x4320, and an RTX 4090 at 99% gave a race ~80 frames a second */
    int aa = screen && RENDER_SCALE >= 2 && w >= 2 * (h == 480 ? 640 : hd_w) ? 2 : 1;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)(w * RENDER_SCALE / aa * (wide ? g_widen : 1) + 0.5f);
    if (td.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) /* 16384: 720p's 2560 (AA) at 4x, 32:9 would be 20480 */
        td.Width = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    td.Height = h * RENDER_SCALE;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = depth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM; /* depth: readable as a shadow map */
    td.SampleDesc.Count = 1;
    td.BindFlags = (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET) | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &t->tex);
    if (FAILED(hr)) fail("CreateTexture2D (target)", hr);
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D};
        sv.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)t->tex, &dv, &t->dsv);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, &sv, &t->srv);
    } else {
        hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
    }
    if (FAILED(hr)) fail("target views", hr);
    t->addr = addr;
    t->w = w;
    t->h = h;
    t->hw = td.Width, t->hh = td.Height, t->wide = wide;
    t->depth = depth;
    g_targets_made++;
    t->used = g_frame;
    if (ktrace()) fprintf(stderr, "[render] %s target %08X %ux%u\n", depth ? "depth" : "colour", addr, w, h);
    return t;
}

static void upload(ID3D11Buffer **buf, UINT *size, UINT bind, const void *data, UINT bytes);

/* the scissored clears below: a triangle that covers the target (at z 0), drawn with a scissor rect */
static ID3D11VertexShader *g_fill_vs;
static ID3D11RasterizerState *g_scissor_rs;
static void fill_init(void) {
    if (g_fill_vs) return;
    ID3DBlob *v = compile("float4 main(uint id : SV_VertexID) : SV_Position {\n"
                          "  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }\n",
                          "vs_5_0");
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &g_fill_vs);
    ID3D10Blob_Release(v);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE};
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_scissor_rs);
}

/* A clear of only some colour channels, drawn as a target-filling triangle through a write mask (ClearView cannot
 * mask). Forza's car select clears just alpha (and depth) between the car's reflection and the car; clearing all
 * of RGBA there blacked out the reflection and the floor. mask: D3D11 write-enable bits, the order of NV2A's. */
static void masked_clear(Target *t, const D3D11_RECT *r, const float c[4], UINT8 mask) {
    static ID3D11PixelShader *ps;
    static ID3D11Buffer *cb;
    static UINT cb_size;
    static ID3D11BlendState *blend[16];
    static ID3D11DepthStencilState *ds;
    fill_init();
    ID3D11VertexShader *vs = g_fill_vs;
    ID3D11RasterizerState *rs = g_scissor_rs;
    if (!ps) {
        ID3DBlob *p = compile("cbuffer C : register(b0) { float4 color; };\n"
                              "float4 main() : SV_Target { return color; }\n",
                              "ps_5_0");
        ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &ps);
        ID3D10Blob_Release(p);
        D3D11_DEPTH_STENCIL_DESC dd = {0};
        ID3D11Device_CreateDepthStencilState(g_dev, &dd, &ds);
    }
    if (!blend[mask]) {
        D3D11_BLEND_DESC bd = {0};
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = mask;
        ID3D11Device_CreateBlendState(g_dev, &bd, &blend[mask]);
    }
    upload(&cb, &cb_size, D3D11_BIND_CONSTANT_BUFFER, c, 16);
    D3D11_VIEWPORT vp = {0, 0, (float)t->hw, (float)t->hh, 0, 1};
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 1, &t->rtv, NULL);
    ID3D11DeviceContext1_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext1_RSSetScissorRects(g_ctx, 1, r);
    ID3D11DeviceContext1_RSSetState(g_ctx, rs);
    ID3D11DeviceContext1_OMSetBlendState(g_ctx, blend[mask], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext1_OMSetDepthStencilState(g_ctx, ds, 0);
    ID3D11DeviceContext1_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext1_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_VSSetShader(g_ctx, vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(g_ctx, ps, NULL, 0);
    ID3D11DeviceContext1_PSSetConstantBuffers(g_ctx, 0, 1, &cb);
    ID3D11DeviceContext1_Draw(g_ctx, 3, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 0, NULL, NULL); /* draws rebind everything they use */
}

/* A depth/stencil clear of part of the target (ClearDepthStencilView clears all of it): the triangle through the
 * scissor rect, its depth set by the viewport's range, the stencil replaced. Forza's bumper and hood views clear
 * depth to 0 under the rear-view mirror only, so the scene leaves the mirror alone; clearing the whole target to 0
 * left those views black but for the mirror. which: D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL. */
static void depth_rect_clear(Target *z, const D3D11_RECT *r, UINT which, float depth, UINT8 stencil) {
    static ID3D11DepthStencilState *ds[4];
    fill_init();
    if (!ds[which]) {
        D3D11_DEPTH_STENCIL_DESC dd = {TRUE, which & D3D11_CLEAR_DEPTH ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO,
                                       D3D11_COMPARISON_ALWAYS, (which & D3D11_CLEAR_STENCIL) != 0, 0xFF, 0xFF};
        D3D11_DEPTH_STENCILOP_DESC op = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                                         D3D11_COMPARISON_ALWAYS};
        dd.FrontFace = dd.BackFace = op;
        ID3D11Device_CreateDepthStencilState(g_dev, &dd, &ds[which]);
    }
    D3D11_VIEWPORT vp = {0, 0, (float)z->hw, (float)z->hh, depth, depth};
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 0, NULL, z->dsv);
    ID3D11DeviceContext1_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext1_RSSetScissorRects(g_ctx, 1, r);
    ID3D11DeviceContext1_RSSetState(g_ctx, g_scissor_rs);
    ID3D11DeviceContext1_OMSetDepthStencilState(g_ctx, ds[which], stencil);
    ID3D11DeviceContext1_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext1_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_VSSetShader(g_ctx, g_fill_vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(g_ctx, NULL, NULL, 0);
    ID3D11DeviceContext1_Draw(g_ctx, 3, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
}

/* a clear rect (guest pixels, inclusive) in the target's host pixels */
static D3D11_RECT clear_rect(const Target *t, const Surface *s, int x0, int y0, int x1, int y1) {
    float xs = (float)t->hw / t->w; /* host pixels per guest pixel across */
    D3D11_RECT r = {(LONG)(x0 * (int)s->aa_x * xs), y0 * (int)s->aa_y * RENDER_SCALE,
                    (LONG)((x1 + 1) * (int)s->aa_x * xs), (y1 + 1) * (int)s->aa_y * RENDER_SCALE};
    return r;
}

void render_clear(const Surface *s, uint32_t flags, uint32_t argb, uint32_t zstencil, int x0, int y0, int x1, int y1) {
    if (s->ui_w) { /* a front-end rect: stretch it as pgraph_surface stretches the draws */
        int w = (int)(s->width / s->aa_x), h = (int)(s->height / s->aa_y);
        x0 = x0 * w / (int)s->ui_w, x1 = (x1 + 1) * w / (int)s->ui_w - 1;
        y0 = y0 * h / (int)s->ui_h, y1 = (y1 + 1) * h / (int)s->ui_h - 1;
    }
    if ((flags & 0xF0) && s->color_addr) { /* NV097_CLEAR_SURFACE: [4] R, [5] G, [6] B, [7] A */
        Target *t = target(s->color_addr, s->width, s->height, 0);
        D3D11_RECT r = clear_rect(t, s, x0, y0, x1, y1);
        float c[4] = {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                      (argb >> 24) / 255.0f};
        if ((flags & 0xF0) == 0xF0) ID3D11DeviceContext1_ClearView(g_ctx, (ID3D11View *)t->rtv, c, &r, 1);
        else masked_clear(t, &r, c, (UINT8)((flags >> 4) & 15));
    }
    if ((flags & 3) && s->zeta_addr) {
        Target *t = target(s->zeta_addr, s->width, s->height, 1);
        /* Z24S8: depth in the top 24 bits; Z16 uses the low 16 */
        float z = s->zeta_format == 1 ? (zstencil & 0xFFFF) / 65535.0f : (zstencil >> 8) / 16777215.0f;
        UINT which = (flags & 1 ? D3D11_CLEAR_DEPTH : 0) | (flags & 2 ? D3D11_CLEAR_STENCIL : 0);
        /* part of the surface, not of the target: a depth buffer the AA scene shares is twice a 640-wide pass's
         * width, and its whole-surface clears must still clear all of it (the first-person views' light pass) */
        if (x0 > 0 || y0 > 0 || x1 + 1 < (int)(s->width / s->aa_x) || y1 + 1 < (int)(s->height / s->aa_y)) {
            D3D11_RECT r = clear_rect(t, s, x0, y0, x1, y1);
            depth_rect_clear(t, &r, which, z, (UINT8)zstencil);
            return; /* part of the scene only: its motion vectors stay */
        }
        ID3D11DeviceContext1_ClearDepthStencilView(g_ctx, t->dsv, which, z, (UINT8)zstencil);
        Target *c = s->color_addr && (flags & 1) ? find_target(s->color_addr, 0) : NULL;
        if (c && c->mv_rtv) { /* the scene starts over: nothing has moved yet */
            static const float zero[4] = {0};
            ID3D11DeviceContext1_ClearRenderTargetView(g_ctx, c->mv_rtv, zero);
        }
    }
}

/* ---------------- shaders ---------------- */

static struct {
    uint64_t hash;
    void *shader; /* ID3D11VertexShader / ID3D11PixelShader */
} g_shaders[MAX_SHADERS];
static int64_t g_compile_ticks; /* FPSLOG */
static int g_compiles;

static void *shader(const char *src, int pixel) {
    uint64_t h = fnv(src, strlen(src), FNV0 + pixel);
    int i, free_slot = -1;
    for (i = 0; i < MAX_SHADERS; i++) {
        if (g_shaders[i].hash == h && g_shaders[i].shader) return g_shaders[i].shader;
        if (!g_shaders[i].shader && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) fail("shader cache full", 0);
    /* shadercache/<hash>.vs/.ps next to the exe: compiled once, ever (a compile took ~35 ms, a stutter each time a
     * new car, track or effect came on screen); the name is the source's hash, so changed generators miss */
    static char dir[MAX_PATH];
    if (!dir[0]) {
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *slash = strrchr(dir, '\\');
        strcpy(slash ? slash + 1 : dir, "shadercache");
        CreateDirectoryA(dir, NULL);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/%016llX.%s", dir, (unsigned long long)h, pixel ? "ps" : "vs");
    void *sh = NULL;
    HRESULT hr = E_FAIL;
    FILE *f = fopen(path, "rb");
    if (f) { /* a cached file that will not load (cut short by a crash) is compiled again */
        fseek(f, 0, SEEK_END);
        size_t size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        void *bytes = size ? malloc(size) : NULL;
        if (bytes && fread(bytes, 1, size, f) == size)
            hr = pixel ? ID3D11Device_CreatePixelShader(g_dev, bytes, size, NULL, (ID3D11PixelShader **)&sh)
                       : ID3D11Device_CreateVertexShader(g_dev, bytes, size, NULL, (ID3D11VertexShader **)&sh);
        free(bytes);
        fclose(f);
    }
    if (FAILED(hr)) {
        LARGE_INTEGER c0, c1; /* FPSLOG */
        QueryPerformanceCounter(&c0);
        ID3DBlob *code = compile(src, pixel ? "ps_5_0" : "vs_5_0");
        QueryPerformanceCounter(&c1);
        g_compile_ticks += c1.QuadPart - c0.QuadPart, g_compiles++, g_compiles_made++;
        void *bytes = ID3D10Blob_GetBufferPointer(code);
        size_t size = ID3D10Blob_GetBufferSize(code);
        hr = pixel ? ID3D11Device_CreatePixelShader(g_dev, bytes, size, NULL, (ID3D11PixelShader **)&sh)
                   : ID3D11Device_CreateVertexShader(g_dev, bytes, size, NULL, (ID3D11VertexShader **)&sh);
        if (SUCCEEDED(hr) && (f = fopen(path, "wb"))) fwrite(bytes, 1, size, f), fclose(f);
        ID3D10Blob_Release(code);
    }
    if (FAILED(hr)) fail("create shader", hr);
    g_shaders[free_slot].hash = h;
    g_shaders[free_slot].shader = sh;
    if (ktrace()) fprintf(stderr, "[render] compiled %s shader %016llX\n", pixel ? "pixel" : "vertex", (unsigned long long)h);
    if (getenv("SHADER_DUMP")) fprintf(stderr, "---- %s ----\n%s\n", pixel ? "pixel" : "vertex", src);
    return sh;
}

/* generator inputs (vsh_key/psh_key) -> shader, so a draw with known state skips generating source */
#define MAX_KEYED 4096
static struct {
    uint64_t key;
    void *shader;
} g_keyed[2][MAX_KEYED];
static int g_nkeyed[2];

static void **keyed(int pixel, uint64_t key) {
    for (uint32_t i = (uint32_t)(key % MAX_KEYED);; i = (i + 1) % MAX_KEYED) {
        if (g_keyed[pixel][i].shader && g_keyed[pixel][i].key == key) return &g_keyed[pixel][i].shader;
        if (!g_keyed[pixel][i].shader) {
            if (++g_nkeyed[pixel] == MAX_KEYED) fail("shader key cache full", 0);
            g_keyed[pixel][i].key = key;
            return &g_keyed[pixel][i].shader;
        }
    }
}

/* ---------------- textures ---------------- */

static struct {
    uint32_t addr, format, rect, ctrl1;
    uint64_t hash, key; /* key: the texture pack's name for the contents (texpack.c) */
    uint32_t checked;   /* g_frame when the guest data last matched hash */
    uint32_t decoded;   /* g_frame it was decoded */
    uint32_t w, h;
    int is_rect;
    int pending, icon, device; /* replacement loading; replaced per input device (prompts), input_icon_state() then */
    uint32_t shown;            /* g_frame its original first showed while the replacement loaded (TEXTIME), 0 none */
    int dynamic, shape;        /* made updatable (its contents change often), and the shape it was made in */
    ID3D11ShaderResourceView *srv, *rep; /* the guest's texture, its replacement */
} g_textures[MAX_TEXTURES];
static int64_t g_hash_ticks, g_hash_bytes; /* FPSLOG */
static uint32_t g_frame = 1; /* frames presented */

/* slot i's replacement: a button prompt drawn for the input device last used (icons.c), else the texture pack's. In
 * the first second after the screen on top changes (a race's first frame, its grid, the countdown), a frame waits up
 * to g_texwait_ms in all for replacements still loading, so that a new scene's (a race's ~200 at once) show from
 * their first frame rather than the originals for a moment (the "pop"); otherwise, and past that, the original
 * until the replacement is ready: waiting mid-race made the first lap stutter (frames of 50-115 ms as new scenery
 * came in), where the few new textures a frame pop for a frame or two, far away. */
static ULONGLONG g_texwait_until; /* the frame's waiting ends (set at its first wait) */
static uint32_t g_texwait_frame, g_scene_frame; /* g_frame the screen on top last changed */
static void resolve(int i) {
    int state = input_icon_state(), r = 2;
    ID3D11ShaderResourceView *rep = g_textures[i].icon ? icons_get(g_dev, g_textures[i].key, state) : NULL;
    if (g_texwait_frame != g_frame)
        g_texwait_frame = g_frame, g_texwait_until = GetTickCount64() + (g_frame - g_scene_frame < 60 ? g_texwait_ms : 0);
    ULONGLONG now = GetTickCount64();
    if (rep) ID3D11ShaderResourceView_AddRef(rep);
    else r = texpack_get(g_textures[i].key, state & 15, &rep, now < g_texwait_until ? (DWORD)(g_texwait_until - now) : 0);
    g_textures[i].pending = r == 1;
    g_textures[i].device = state;
    if (r == 1 && !g_textures[i].shown) g_textures[i].shown = g_frame;
    if (r == 1) return; /* the old one, if any, until this one is ready */
    static int log = -1; /* TEXTIME=1: each replacement that came after its original showed (a pop) */
    if (log < 0) log = getenv("TEXTIME") != NULL;
    if (log && g_textures[i].shown && r == 2) fprintf(stderr, "[texpack] %llu ms: replaced %u frames late\n", GetTickCount64(), g_frame - g_textures[i].shown);
    g_textures[i].shown = 0;
    if (g_textures[i].rep) ID3D11ShaderResourceView_Release(g_textures[i].rep);
    g_textures[i].rep = rep;
}

/* A texture not used for 1200 frames (20 s at 60) gives up its slot, with the texture pack replacement it holds (a
 * reference the pack's own budget cannot drop): left alone, a session kept every texture every track ever showed,
 * ~1.1 GB more a track. A few slots a frame, all of them every ~2 s. */
static void texture_age(void) {
    static int at;
    for (int n = 0; n < MAX_TEXTURES / 128; n++, at = (at + 1) % MAX_TEXTURES)
        if (g_textures[at].srv && g_frame - g_textures[at].checked > 1200) {
            ID3D11ShaderResourceView_Release(g_textures[at].srv), g_textures[at].srv = NULL;
            if (g_textures[at].rep) ID3D11ShaderResourceView_Release(g_textures[at].rep), g_textures[at].rep = NULL;
        }
}

/* TEXTURE PACK, TEXTURE DUMP in the settings menu: every texture looks for its replacement again */
static void render_texpack_(int on) {
    texpack_enable(on);
    for (int i = 0; i < MAX_TEXTURES; i++) g_textures[i].pending = g_textures[i].srv != NULL;
}
void render_texpack(int on) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    render_texpack_(on);
    gpu_leave();
}
void render_texdump(int on) { texpack_set_dump(on); }

static ID3D11ShaderResourceView *dummy(int kind) { return kind == TK_CUBE ? g_dummy_cube : kind == TK_3D ? g_dummy_3d : g_dummy_srv; }

/* A cube map the game renders into (one face per surface, faces `face` bytes apart): a host cube whose faces are
 * copied from the face targets once per frame. Faces not rendered yet stay black. */
static struct {
    uint32_t addr, checked;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} g_cubes[8];

static ID3D11ShaderResourceView *target_cube(uint32_t addr, uint32_t face, Target *any) {
    int i, lru = 0;
    for (i = 0; i < 8 && g_cubes[i].tex && g_cubes[i].addr != addr; i++)
        if (g_cubes[i].checked < g_cubes[lru].checked) lru = i;
    uint32_t size = any->w * RENDER_SCALE;
    if (i == 8) { /* a full table gives up the cube used longest ago: each track renders its own (the 9th one stopped
                   * the game) */
        i = lru;
        ID3D11ShaderResourceView_Release(g_cubes[i].srv), ID3D11Texture2D_Release(g_cubes[i].tex);
        g_cubes[i].srv = NULL, g_cubes[i].tex = NULL;
    }
    if (g_cubes[i].tex && g_cubes[i].addr == addr) { /* the same cube at another size (render scale): made again */
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(g_cubes[i].tex, &d);
        if (d.Width != size) {
            ID3D11ShaderResourceView_Release(g_cubes[i].srv), ID3D11Texture2D_Release(g_cubes[i].tex);
            g_cubes[i].srv = NULL, g_cubes[i].tex = NULL;
        }
    }
    if (!g_cubes[i].tex) {
        D3D11_TEXTURE2D_DESC td = {size, size, 1, 6, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE};
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURECUBE};
        vd.TextureCube.MipLevels = 1;
        HRESULT hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_cubes[i].tex);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_cubes[i].tex, &vd, &g_cubes[i].srv);
        if (FAILED(hr)) fail("cube map target", hr);
        g_cubes[i].addr = addr;
    }
    if (g_cubes[i].checked != g_frame) {
        g_cubes[i].checked = g_frame;
        for (uint32_t f = 0; f < 6; f++) {
            Target *ft = find_target(addr + f * face, 0);
            D3D11_BOX box = {0, 0, 0, size, size, 1};
            if (ft && ft->w * RENDER_SCALE >= size && ft->h * RENDER_SCALE >= size)
                ID3D11DeviceContext1_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_cubes[i].tex, f, 0, 0, 0,
                                                           (ID3D11Resource *)ft->tex, 0, &box);
        }
    }
    return g_cubes[i].srv;
}

/* host view for texture unit `t`; *scale gets the coordinate scale for unnormalised (rect) textures */
static ID3D11ShaderResourceView *texture(const TexDesc *t, float scale[2], int ui) {
    scale[0] = scale[1] = 1;
    int kind = tex_kind(t->format);
    ID3D11ShaderResourceView *video = kind == TK_2D ? video_for(t->addr) : NULL;
    if (video) { /* an upscaled cutscene frame in place of the one Bink copied here (video.c) */
        uint32_t color = (t->format >> 8) & 0xFF;
        if (color >= 0x10 && color != 0x19 && color != 0x1A) /* linear: unnormalised coordinates over the guest's size */
            scale[0] = 1.0f / (t->rect >> 16), scale[1] = 1.0f / (t->rect & 0xFFFF);
        return video;
    }
    if (kind == TK_SHADOW) { /* a depth surface rendered earlier */
        Target *rt = find_target(t->addr, 1);
        if (!rt) return g_dummy_srv;
        if (((t->format >> 8) & 0xFF) >= 0x2E) { /* LIN_DEPTH_*: unnormalised coordinates */
            scale[0] = 1.0f / rt->w;
            scale[1] = 1.0f / rt->h;
        }
        return rt->srv;
    }
    if (kind == TK_CUBE) {
        uint32_t face = texture_guest_size(t) / 6;
        for (uint32_t f = 0; face && f < 6; f++) {
            Target *ft = find_target(t->addr + f * face, 0);
            if (ft) return target_cube(t->addr, face, ft);
        }
    }
    Target *rt = kind == TK_2D ? find_target(t->addr, 0) : NULL;
    if (rt) { /* render-to-texture */
        uint32_t color = (t->format >> 8) & 0xFF;
        if (color >= 0x10 && color != 0x19 && color != 0x1A) { /* linear: unnormalised coordinates */
            /* a front-end draw (ui) is 480-line code: it addresses a target the HD-aware post-processing sized for
             * the HD mode in 480-line texels, as the Assists dialog samples its blurred backdrop (1280x360) as 640x240 */
            uint32_t hd_w = pgraph_hd_width(), hd_h = hd_w * 9 / 16, w = rt->w, h = rt->h;
            if (ui && hd_part(w, hd_w) && hd_part(h, hd_h)) w = w * 640 / hd_w, h = h * 480 / hd_h;
            scale[0] = 1.0f / w;
            scale[1] = 1.0f / h;
        }
        return rt->srv;
    }
    uint32_t size = texture_guest_size(t);
    if (!size) {
        static uint32_t warned[64];
        uint32_t color = (t->format >> 8) & 0xFF;
        if (color < 64 && !warned[color]++) fprintf(stderr, "[render] texture format %02X not supported\n", color);
        return dummy(kind);
    }
    /* the texture's set (by address): it is in one of its slots, or the slot used longest ago gives way. 512 slots
     * searched in turn were too few for a track and eight cars: textures in use were dropped and decoded again,
     * 150-230 a second, and an Arcade race at Silverstone ran at 35 frames a second */
    int set = (int)(((t->addr >> 6) * 2654435761u) >> 22) * TEX_WAYS, i = -1, lru = set;
    for (int w = set; w < set + TEX_WAYS && i < 0; w++) {
        if (g_textures[w].srv && g_textures[w].addr == t->addr && g_textures[w].format == t->format &&
            g_textures[w].rect == t->rect && g_textures[w].ctrl1 == t->ctrl1)
            i = w;
        else if (g_textures[lru].srv && (!g_textures[w].srv || g_textures[w].checked < g_textures[lru].checked)) lru = w;
    }
    /* shortcut: the guest data is re-hashed once per frame (hashing at every use took 60% of the race's time), so
     * a texture the CPU rewrites between two draws of one frame shows its old contents until the next frame;
     * switch to write tracking if that shows */
    if (i >= 0 && g_textures[i].checked == g_frame) goto done;
    LARGE_INTEGER h0, h1; /* FPSLOG: time and bytes spent re-hashing */
    QueryPerformanceCounter(&h0);
    uint64_t h = hash_bytes(g_mem + t->addr, size, 0);
    QueryPerformanceCounter(&h1);
    g_hash_ticks += h1.QuadPart - h0.QuadPart, g_hash_bytes += size;
    if (((t->format >> 8) & 0xFF) == 0x0B) h = hash_bytes(g_mem + t->palette, t->palette_len * 4, h); /* palette edits too */
    if (i >= 0 && g_textures[i].hash == h) {
        g_textures[i].checked = g_frame;
        goto done;
    }
    /* a texture whose contents change within a second is animated: only its first picture goes to the dump */
    int animated = i >= 0 && g_frame - g_textures[i].decoded < 60;
    if (i < 0) i = lru;
    ID3D11ShaderResourceView *old = g_textures[i].srv; /* the same texture's, updated in place if it can be */
    g_textures[i].srv = NULL;
    if (g_textures[i].rep) ID3D11ShaderResourceView_Release(g_textures[i].rep), g_textures[i].rep = NULL;

    LARGE_INTEGER d0; /* the hitch lines: time spent decoding and uploading */
    QueryPerformanceCounter(&d0);
    TexImage img;
    if (!texture_decode(t, &img)) {
        if (old) ID3D11ShaderResourceView_Release(old);
        return dummy(kind);
    }
    if (ktrace()) {
        fprintf(stderr, "[render] texture %08X fmt %08X %ux%u levels %d, first bytes %08X %08X %08X %08X\n", t->addr,
                t->format, img.width, img.height, img.levels, M32(t->addr), M32(t->addr + 4), M32(t->addr + 8),
                M32(t->addr + 12));
        if (t_ctx) { /* who submitted it: guest return addresses on the submitting thread's stack */
            fprintf(stderr, "[render]   submitted by thread with stack:");
            for (uint32_t k = 0, n = 0; k < 512 && n < 24; k++) {
                uint32_t v = M32(t_ctx->esp + 4 * k);
                if (v > 0x11000 && v < 0x3B0000) fprintf(stderr, " %08X", v), n++; /* game/XAPI code only */
            }
            fprintf(stderr, "\n");
        }
        char path[64]; /* raw guest bytes, to trace where a texture came from */
        snprintf(path, sizeof path, "build/tex_%08X.bin", t->addr);
        FILE *f = fopen(path, "wb");
        if (f) fwrite(g_mem + t->addr, 1, size, f), fclose(f);
    }
    uint64_t key = hash_bytes((uint32_t[]){(t->format >> 8) & 0xFF, img.width, img.height}, 12, h);
    if (kind == TK_2D && !animated) texpack_dump(key, &img);
    if (icons_is_prompt(key)) icons_original(key, &img);
    static const DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM,
                                       DXGI_FORMAT_BC3_UNORM};
    D3D11_SUBRESOURCE_DATA init[6 * 13];
    for (int s = 0; s < img.faces * img.levels; s++) init[s] = (D3D11_SUBRESOURCE_DATA){img.level[s], img.pitch[s], img.slice[s]};
    ID3D11Resource *res = NULL;
    HRESULT hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {fmts[img.format], kind == TK_CUBE ? D3D11_SRV_DIMENSION_TEXTURECUBE
                                                            : kind == TK_3D ? D3D11_SRV_DIMENSION_TEXTURE3D
                                                                            : D3D11_SRV_DIMENSION_TEXTURE2D};
    vd.Texture2D.MipLevels = img.levels; /* same place in the Texture3D and TextureCube views */
    /* A texture whose contents change again within a second (animated: the game rewrites ~32 1x1 colour textures
     * each frame in a race) is made updatable once, then updated in place: a new texture each time was ~250 a second */
    int shape = (int)(hash_bytes((uint32_t[]){img.width, img.height, img.depth, img.levels, img.faces, img.format, kind},
                                 28, 0) & 0x7FFFFFFF);
    if (animated && old && g_textures[i].dynamic && g_textures[i].shape == shape) {
        ID3D11ShaderResourceView_GetResource(old, &res);
        int subs = kind == TK_3D ? img.levels : img.faces * img.levels;
        for (int s = 0; s < subs; s++)
            ID3D11DeviceContext1_UpdateSubresource(g_ctx, res, s, NULL, init[s].pSysMem, init[s].SysMemPitch,
                                                   init[s].SysMemSlicePitch);
        g_textures[i].srv = old, old = NULL, hr = S_OK;
    } else {
        D3D11_USAGE usage = animated ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
        if (kind == TK_3D) {
            D3D11_TEXTURE3D_DESC td = {img.width, img.height, img.depth, img.levels, fmts[img.format], usage,
                                       D3D11_BIND_SHADER_RESOURCE, 0, 0};
            hr = ID3D11Device_CreateTexture3D(g_dev, &td, init, (ID3D11Texture3D **)&res);
        } else {
            D3D11_TEXTURE2D_DESC td = {img.width, img.height, img.levels, img.faces, fmts[img.format], {1, 0}, usage,
                                       D3D11_BIND_SHADER_RESOURCE, 0, kind == TK_CUBE ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0};
            hr = ID3D11Device_CreateTexture2D(g_dev, &td, init, (ID3D11Texture2D **)&res);
        }
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_dev, res, &vd, &g_textures[i].srv);
        g_textures[i].dynamic = animated, g_textures[i].shape = shape;
    }
    if (old) ID3D11ShaderResourceView_Release(old);
    if (res) ID3D11Resource_Release(res);
    if (FAILED(hr)) {
        fprintf(stderr, "[render] texture %08X (%ux%ux%u fmt %08X) upload failed (%08lX)\n", t->addr, img.width,
                img.height, img.depth, t->format, (unsigned long)hr);
        texture_free(&img);
        return dummy(kind);
    }
    g_tex_uploads++;
    LARGE_INTEGER d1;
    QueryPerformanceCounter(&d1);
    g_tex_ticks += d1.QuadPart - d0.QuadPart;
    static int declog = -1; /* DECODELOG=1: every texture decoded, with whether it is the slot's same texture again */
    if (declog < 0) declog = getenv("DECODELOG") != NULL;
    if (declog)
        fprintf(stderr, "[decode] %08X fmt %08X %ux%ux%u kind %d%s\n", t->addr, t->format, img.width, img.height, img.depth,
                kind, g_textures[i].addr == t->addr && g_textures[i].format == t->format ? " (contents changed)" : "");
    g_textures[i].addr = t->addr;
    g_textures[i].format = t->format;
    g_textures[i].rect = t->rect;
    g_textures[i].ctrl1 = t->ctrl1;
    g_textures[i].hash = h;
    g_textures[i].checked = g_frame;
    g_textures[i].w = img.width;
    g_textures[i].h = img.height;
    g_textures[i].is_rect = img.rect;
    g_textures[i].decoded = g_frame;
    g_textures[i].key = key;
    g_textures[i].pending = kind == TK_2D; /* cube maps and volumes are never replaced */
    g_textures[i].shown = 0;
    g_textures[i].icon = kind == TK_2D && (icons_is_prompt(key) || texpack_has_device_icons(key));
    texture_free(&img);
done:
    if (g_textures[i].pending || (g_textures[i].icon && g_textures[i].device != input_icon_state())) resolve(i);
    if (g_textures[i].is_rect) { /* the guest's size: unnormalised coordinates address a replacement of any size */
        scale[0] = 1.0f / g_textures[i].w;
        scale[1] = 1.0f / g_textures[i].h;
    }
    return g_textures[i].rep ? g_textures[i].rep : g_textures[i].srv;
}

/* ---------------- fixed-function state ---------------- */

static struct {
    uint64_t key;
    void *state;
} g_states[3][MAX_STATES]; /* 0 blend, 1 depth, 2 raster */

static void *cached_state(int kind, uint64_t key) {
    for (int i = 0; i < MAX_STATES; i++)
        if (g_states[kind][i].state && g_states[kind][i].key == key) return g_states[kind][i].state;
    return NULL;
}

static void *store_state(int kind, uint64_t key, void *state) {
    for (int i = 0; i < MAX_STATES; i++)
        if (!g_states[kind][i].state) {
            g_states[kind][i].key = key;
            g_states[kind][i].state = state;
            return state;
        }
    fail("state cache full", 0);
    return NULL;
}

/* NV2A blend factors are GL enums */
static D3D11_BLEND blend_factor(uint32_t f, int alpha) {
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t e) {
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

/* NV097_SET_STENCIL_OP_* (GL's values) -> D3D11 */
static D3D11_STENCIL_OP stencil_op(uint32_t v) {
    switch (v) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP; /* 0x1E00 */
    }
}

static void apply_states(int mv) {
    uint32_t en = g_k[KM(0x0304)], sf = g_k[KM(0x0344)], df = g_k[KM(0x0348)], eq = g_k[KM(0x0350)];
    uint32_t mask = g_k[KM(0x0358)];
    uint64_t key = (uint64_t)en << 63 ^ (uint64_t)sf << 40 ^ (uint64_t)df << 20 ^ eq ^ (uint64_t)mask << 32 ^ (uint64_t)mv << 62;
    ID3D11BlendState *bs = cached_state(0, key);
    if (!bs) {
        D3D11_BLEND_DESC d = {0};
        d.IndependentBlendEnable = mv != 0; /* target 1, the motion: written as is */
        d.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RENDER_TARGET_BLEND_DESC *t = &d.RenderTarget[0];
        t->BlendEnable = en != 0;
        t->SrcBlend = blend_factor(sf, 0);
        t->DestBlend = blend_factor(df, 0);
        t->SrcBlendAlpha = blend_factor(sf, 1);
        t->DestBlendAlpha = blend_factor(df, 1);
        t->BlendOp = t->BlendOpAlpha = blend_op(eq);
        /* NV097_SET_COLOR_MASK: alpha [24], red [16], green [8], blue [0] */
        t->RenderTargetWriteMask = (mask & 0x01000000 ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0) |
                                   (mask & 0x00010000 ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
                                   (mask & 0x00000100 ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                                   (mask & 0x00000001 ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0);
        ID3D11Device_CreateBlendState(g_dev, &d, &bs);
        store_state(0, key, bs);
    }
    uint32_t bc = g_k[KM(0x034C)];
    float factor[4] = {((bc >> 16) & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f, (bc & 0xFF) / 255.0f, (bc >> 24) / 255.0f};
    ID3D11DeviceContext1_OMSetBlendState(g_ctx, bs, factor, 0xFFFFFFFF);

    /* depth, and the stencil test (NV097_SET_STENCIL_TEST_ENABLE 0x032C, write mask 0x0360, func 0x0364, ref 0x0368,
     * read mask 0x036C, ops on fail / depth fail / pass 0x0370-0x0378, GL's values): the race uses it for the cars'
     * shadows, which without it came out as hard black slabs reaching far from the car under a low sun */
    uint32_t zs[10] = {g_k[KM(0x030C)], g_k[KM(0x0354)], g_k[KM(0x035C)], g_k[KM(0x032C)], g_k[KM(0x0360)],
                       g_k[KM(0x0364)], g_k[KM(0x036C)], g_k[KM(0x0370)], g_k[KM(0x0374)], g_k[KM(0x0378)]};
    if (!zs[3]) memset(zs + 4, 0, 6 * 4); /* stencil off: its other state does not matter */
    key = hash_bytes(zs, sizeof zs, 0) & ~(1ull << 62);
    ID3D11DepthStencilState *ds = cached_state(1, key);
    if (!ds) {
        D3D11_DEPTH_STENCIL_DESC d = {0};
        d.DepthEnable = zs[0] != 0;
        d.DepthWriteMask = zs[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = (D3D11_COMPARISON_FUNC)(((zs[1] - 0x200) & 7) + 1); /* GL NEVER.. ALWAYS -> D3D11 1..8 */
        d.StencilEnable = zs[3] != 0;
        d.StencilWriteMask = (UINT8)zs[4], d.StencilReadMask = (UINT8)zs[6];
        d.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)(((zs[5] - 0x200) & 7) + 1);
        d.FrontFace.StencilFailOp = stencil_op(zs[7]);
        d.FrontFace.StencilDepthFailOp = stencil_op(zs[8]);
        d.FrontFace.StencilPassOp = stencil_op(zs[9]);
        d.BackFace = d.FrontFace; /* the NV2A has one set for both faces */
        ID3D11Device_CreateDepthStencilState(g_dev, &d, &ds);
        store_state(1, key, ds);
    }
    ID3D11DeviceContext1_OMSetDepthStencilState(g_ctx, ds, g_k[KM(0x0368)] & 0xFF);

    uint32_t cull = g_k[KM(0x0308)], face = g_k[KM(0x039C)], front = g_k[KM(0x03A0)];
    /* NV097_SET_POLY_OFFSET_FILL_ENABLE, POLYGON_OFFSET_SCALE_FACTOR / BIAS (floats, as glPolygonOffset) */
    uint32_t offset = g_k[KM(0x0338)], factor_bits = offset ? g_k[KM(0x0384)] : 0, bias_bits = offset ? g_k[KM(0x0388)] : 0;
    uint32_t wbuf = (g_k[KM(0x0290)] >> 16) & g_wbuf;
    key = hash_bytes((uint32_t[]){cull, face, front, factor_bits, bias_bits, wbuf}, 24, 0) & ~(1ull << 62); /* 62: samplers */
    ID3D11RasterizerState *rs = cached_state(2, key);
    if (!rs) {
        D3D11_RASTERIZER_DESC d = {0};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = !cull ? D3D11_CULL_NONE : face == 0x404 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        d.FrontCounterClockwise = front == 0x901; /* GL CCW */
        float factor, bias;
        memcpy(&factor, &factor_bits, 4);
        memcpy(&bias, &bias_bits, 4);
        /* the bias is in depth-buffer units, which a 24-bit host buffer shares; the slope is per guest pixel. The bias
         * counts away from the viewer: Forza's shadow pass sets -2000 with a GREATER shadow test, and only a stored
         * depth pushed 2000 units back lets the surfaces the light sees pass it (the other sign shadowed everything).
         * The w-buffered scene's draws take GL's sign: the car's badges and the shadow under it are pulled 2000 units
         * toward the viewer (the road is pushed 4000 back); with the other sign they vanished into the body and the
         * road. (So the shadow test's sense is likely inverted, and that pass's sign makes up for it.) */
        d.DepthBias = wbuf ? (INT)bias : -(INT)bias;
        d.SlopeScaledDepthBias = factor * RENDER_SCALE;
        d.DepthClipEnable = FALSE;
        d.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(g_dev, &d, &rs);
        store_state(2, key, rs);
    }
    ID3D11DeviceContext1_RSSetState(g_ctx, rs);
}

static int g_aniso = 1; /* TEXTURE FILTER: anisotropy for linear-filtered textures (1 = trilinear, as the NV2A) */

/* samplers are cached under bit 62 of the raster table's keys: drop them so they are made again at the new level */
static void render_anisotropy_(int n) {
    g_aniso = n < 1 ? 1 : n > 16 ? 16 : n;
    for (int i = 0; i < MAX_STATES; i++)
        if (g_states[2][i].state && g_states[2][i].key >> 62 & 1) {
            ID3D11SamplerState_Release((ID3D11SamplerState *)g_states[2][i].state);
            g_states[2][i].state = NULL;
        }
}
void render_anisotropy(int n) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    render_anisotropy_(n);
    gpu_leave();
}

static ID3D11SamplerState *sampler(const TexDesc *t) {
    /* ADDRESS: u [3:0], v [11:8], p [19:16] (1 wrap, 2 mirror, 3 clamp-to-edge, 4 border, 5 clamp); FILTER: min [23:16],
     * mag [27:24] */
    uint64_t key = (uint64_t)t->address << 32 ^ (t->filter & 0x0FFF0000);
    ID3D11SamplerState *s = cached_state(2, key | 1ull << 62); /* shares the raster table, distinct key space */
    if (s) return s;
    static const D3D11_TEXTURE_ADDRESS_MODE modes[8] = {D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                                                        D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP,
                                                        D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_CLAMP,
                                                        D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP};
    D3D11_SAMPLER_DESC d = {0};
    uint32_t min = (t->filter >> 16) & 0xFF, mag = (t->filter >> 24) & 15;
    int linear = mag == 2 || min >= 2;
    d.Filter = !linear ? D3D11_FILTER_MIN_MAG_MIP_POINT : g_aniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.MaxAnisotropy = g_aniso;
    d.AddressU = modes[t->address & 7];
    d.AddressV = modes[(t->address >> 8) & 7];
    d.AddressW = modes[(t->address >> 16) & 7];
    d.MaxLOD = D3D11_FLOAT32_MAX;
    uint32_t bc = t->border;
    d.BorderColor[0] = ((bc >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = ((bc >> 8) & 0xFF) / 255.0f;
    d.BorderColor[2] = (bc & 0xFF) / 255.0f;
    d.BorderColor[3] = (bc >> 24) / 255.0f;
    ID3D11Device_CreateSamplerState(g_dev, &d, &s);
    return store_state(2, key | 1ull << 62, s);
}

/* ---------------- draws ---------------- */

typedef struct {
    float c[192][4];
    float surf[4];
    float texscale[4][4];
    float factor0[8][4], factor1[8][4];
    float fogcolor[4], alpharef[4], final0[4], final1[4];
    float ui[4]; /* ULTRAWIDE's 2D: screen-space positions (w 1; z > 0: all) are squeezed across by x about y */
    uint32_t va[16][4]; /* vertex attributes in g_vring: NV2A type | size << 4 (0: vk), stride, vertex 0's byte */
    float vk[16][4];
} Constants;

static void argb4(uint32_t v, float out[4]) {
    out[0] = ((v >> 16) & 0xFF) / 255.0f;
    out[1] = ((v >> 8) & 0xFF) / 255.0f;
    out[2] = (v & 0xFF) / 255.0f;
    out[3] = (v >> 24) / 255.0f;
}

static void upload(ID3D11Buffer **buf, UINT *size, UINT bind, const void *data, UINT bytes) {
    if (*size < bytes) {
        if (*buf) ID3D11Buffer_Release(*buf);
        *size = bytes * 2 > 65536 ? bytes * 2 : 65536;
        D3D11_BUFFER_DESC bd = {*size, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, 0, 0};
        HRESULT hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, buf);
        if (FAILED(hr)) fail("CreateBuffer", hr);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext1_Map(g_ctx, (ID3D11Resource *)*buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext1_Unmap(g_ctx, (ID3D11Resource *)*buf, 0);
}

/* room for bytes in the ring at a multiple of align, mapped: *at gets its byte offset; ring_unmap after writing.
 * bind SHADER_RESOURCE makes the ring readable as bytes (r->srv). */
static uint8_t *ring_map(Ring *r, UINT bind, UINT bytes, UINT align, UINT *at) {
    *at = (r->used + align - 1) / align * align;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (*at + bytes > r->size || (r == &g_vring && !g_vring_ok)) {
        if (bytes > r->size / 4) { /* made (again) room for four of these at least */
            if (r->srv) ID3D11ShaderResourceView_Release(r->srv), r->srv = NULL;
            if (r->buf) ID3D11Buffer_Release(r->buf);
            r->size = bytes > (1u << 20) ? bytes * 4 : 4u << 20;
            int raw = (bind & D3D11_BIND_SHADER_RESOURCE) != 0;
            D3D11_BUFFER_DESC bd = {r->size, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE,
                                    raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0, 0};
            HRESULT hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &r->buf);
            if (SUCCEEDED(hr) && raw) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd = {DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX};
                vd.BufferEx.NumElements = r->size / 4, vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
                hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)r->buf, &vd, &r->srv);
            }
            if (FAILED(hr)) fail("CreateBuffer", hr);
        }
        *at = 0, how = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext1_Map(g_ctx, (ID3D11Resource *)r->buf, 0, how, 0, &m);
    r->used = *at + bytes;
    return (uint8_t *)m.pData + *at;
}
static void ring_unmap(Ring *r) { ID3D11DeviceContext1_Unmap(g_ctx, (ID3D11Resource *)r->buf, 0); }

/* appends data to the ring at a multiple of align; returns its byte offset */
static UINT ring_put(Ring *r, UINT bind, const void *data, UINT bytes, UINT align) {
    UINT at;
    memcpy(ring_map(r, bind, bytes, align, &at), data, bytes);
    ring_unmap(r);
    return at;
}

/* ---------------- motion vectors, for the temporal upscalers ---------------- */

/* While DLSS or FSR 3 runs, the scene's draws (depth-tested, into a target the size of the frame or larger: not the
 * shadow maps, reflections, post-processing or menus) also write where each pixel was a frame ago, from the same
 * vertex program run on the constants that draw had then, and draw with a sub-pixel camera jitter. A draw finds its
 * own of last frame by a key: its program, vertex and index counts, the first vertices' attributes, and how many
 * draws with that key came before it this frame. Skinned or CPU-animated vertices move without their constants
 * changing, so they report the camera's motion only. */
static int g_mv_on, g_jitter_on; /* motion vectors and jitter, for DLSS and FSR 3 (MVTEST: the vectors alone) */
static int g_supersampled;       /* the frame is twice the window each way: no upscaler, so no motion (render_present) */
#define MV_SLOTS 4096
typedef struct {
    uint64_t key; /* 0: free */
    float c[192][4];
} PrevConstants;
static PrevConstants *g_mvc[2]; /* this frame's and last frame's */
static int g_mvc_cur;
static float g_jitter[2];           /* this frame's camera offset, in pixels of the presented frame */
static Target *g_scene, *g_scene_z; /* the frame's scene target and its depth, for the upscaler */
static ID3D11Buffer *g_cbp;
static int g_mv_draws, g_mv_matched; /* MVLOG=1: per frame, the scene's draws and those that found last frame's */

void render_motion(int on) { g_mv_on = on; }

/* the constants the draw with this key had last frame (else its own now), and its slot this frame */
static const float *prev_constants(uint64_t base, const float (*now)[4]) {
    if (!g_mvc[0]) g_mvc[0] = calloc(MV_SLOTS, sizeof(PrevConstants)), g_mvc[1] = calloc(MV_SLOTS, sizeof(PrevConstants));
    PrevConstants *cur = g_mvc[g_mvc_cur], *old = g_mvc[g_mvc_cur ^ 1];
    uint64_t key = 0;
    uint32_t i = 0;
    for (uint32_t occ = 0; occ < 64; occ++) { /* the next free occurrence of the key this frame */
        key = hash_bytes(&occ, 4, base) | 1;
        for (i = (uint32_t)(key % MV_SLOTS); cur[i].key && cur[i].key != key; i = (i + 1) % MV_SLOTS) {}
        if (!cur[i].key) break;
    }
    cur[i].key = key;
    memcpy(cur[i].c, now, sizeof cur[i].c);
    g_mv_draws++;
    for (uint32_t j = (uint32_t)(key % MV_SLOTS), n = 0; old[j].key && n < MV_SLOTS; j = (j + 1) % MV_SLOTS, n++)
        if (old[j].key == key) return g_mv_matched++, &old[j].c[0][0];
    return &now[0][0];
}

/* a new frame: this frame's constants become last frame's; the next jitter (Halton 2, 3: 16 points in a pixel) */
static void motion_frame(void) {
    static uint32_t n;
    if (g_mvc[0]) g_mvc_cur ^= 1, memset(g_mvc[g_mvc_cur], 0, MV_SLOTS * sizeof(PrevConstants));
    uint32_t k = n++ % 16 + 1;
    float h2 = 0, h3 = 0, f;
    uint32_t i;
    for (i = k, f = 0.5f; i; i /= 2, f /= 2) h2 += f * (i % 2);
    for (i = k, f = 1.0f / 3; i; i /= 3, f /= 3) h3 += f * (i % 3);
    g_jitter[0] = g_jitter_on ? h2 - 0.5f : 0, g_jitter[1] = g_jitter_on ? h3 - 0.5f : 0;
}

static void motion_target(Target *t) {
    if (t->mv) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(t->tex, &td);
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &t->mv))) {
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->mv, NULL, &t->mv_rtv);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->mv, NULL, &t->mv_srv);
        static const float zero[4] = {0};
        ID3D11DeviceContext1_ClearRenderTargetView(g_ctx, t->mv_rtv, zero);
    }
}

static uint32_t g_present_w = 640, g_present_h = 480; /* the presented frame's guest size, last we saw */
static UINT g_present_hw = 640 * DEF_RENDERSCALE;    /* and its host width */

/* The game runs a frame or two ahead of the screen, so the scene's motion and depth are kept with the frame they
 * belong to: when a draw reads the scene target into another (the anti-aliasing resolve into the frame that will be
 * shown), both are copied off, under that target's address, for render_present to find */
typedef struct {
    uint32_t addr; /* the frame (colour target) they belong to; 0 free */
    uint32_t w, h; /* the scene's size, host pixels */
    ID3D11Texture2D *mv, *depth;
    ID3D11ShaderResourceView *mv_srv, *depth_srv;
    float jitter[2];
    uint32_t frame;
} Snapshot;
static Snapshot g_snaps[4];
static int g_snap_next;
static void snapshot_scene(uint32_t frame_addr) {
    if (!g_scene || !g_scene->mv || !g_scene_z || !g_mv_draws) return; /* later reads of the same scene (bloom): no */
    Snapshot *n = NULL;
    for (int i = 0; i < 4 && !n; i++)
        if (g_snaps[i].addr == frame_addr) n = &g_snaps[i];
    if (!n) n = &g_snaps[g_snap_next++ % 4];
    D3D11_TEXTURE2D_DESC md, dd;
    ID3D11Texture2D_GetDesc(g_scene->mv, &md);
    ID3D11Texture2D_GetDesc(g_scene_z->tex, &dd);
    if (n->mv && (n->w != md.Width || n->h != md.Height)) { /* resized: made again */
        ID3D11ShaderResourceView_Release(n->mv_srv), ID3D11Texture2D_Release(n->mv);
        ID3D11ShaderResourceView_Release(n->depth_srv), ID3D11Texture2D_Release(n->depth);
        n->mv = n->depth = NULL;
    }
    if (!n->mv) {
        md.BindFlags = dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC zv = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D};
        zv.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &md, NULL, &n->mv)) ||
            FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &n->depth)))
            return;
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)n->mv, NULL, &n->mv_srv);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)n->depth, &zv, &n->depth_srv);
        n->w = md.Width, n->h = md.Height;
    }
    if (dd.Width != md.Width || dd.Height != md.Height) return; /* a depth of another size: not this scene's */
    ID3D11DeviceContext1_CopyResource(g_ctx, (ID3D11Resource *)n->mv, (ID3D11Resource *)g_scene->mv);
    ID3D11DeviceContext1_CopyResource(g_ctx, (ID3D11Resource *)n->depth, (ID3D11Resource *)g_scene_z->tex);
    n->addr = frame_addr, n->jitter[0] = g_jitter[0], n->jitter[1] = g_jitter[1], n->frame = g_frame;
    static int log = -1;
    if (log < 0) log = getenv("MVLOG") != NULL;
    if (log) fprintf(stderr, "[mv] frame %u: %d scene draws, %d matched last frame's; scene %ux%u\n", g_frame, g_mv_draws,
                     g_mv_matched, md.Width, md.Height);
    g_mv_draws = g_mv_matched = 0;
    motion_frame(); /* the scene is done: the next draws are the next frame's */
}

/* the motion and depth of the frame in target addr, or NULL */
static const Snapshot *scene_of(uint32_t addr) {
    for (int i = 0; i < 4; i++)
        if (g_snaps[i].addr == addr && g_snaps[i].mv) return &g_snaps[i];
    return NULL;
}

/* ULTRAWIDE: the game draws its 2D (HUD, menus, dialogs, loading screens) in 640 columns meant for 16:9; on a wide
 * screen target it goes into the middle 16:9, as wide as it was meant. Except post-processing (a draw reading a render
 * target over all of its clip: the frame's composite, bloom, a dialog's blurred backdrop) and full-width fills (a draw
 * over the whole width whose vertices differ only in position: fades, darkening, the menus' bars), which span the
 * screen. The shader tells which positions are 2D: w 1, or 0 as the text's program leaves it (perspective draws with
 * the depth test off, the sky, are not); a draw clipped to part of the screen (the HUD's track map, a 3D view of its
 * own) is squeezed whole (*all).
 * Returns the factor positions are squeezed by across. */
static float ui_squeeze(const Draw *d, const Target *color, int rt_read, int *all) {
    const Surface *s = &d->surf;
    *all = 0;
    if (!color->wide || g_k[KM(0x030C)]) return 1; /* the depth test on: 3D */
    float p[4], q[4], x0 = 1e30f, x1 = -1e30f;
    int same = d->nindices <= 64; /* a fill is a quad or few; a bigger flat-coloured draw was compared vertex by vertex */
    for (uint32_t i = 1; i < d->nindices && same; i++)
        for (int a = 1; a < 16 && same; a++)
            pgraph_attr(d, d->indices[0], a, p), pgraph_attr(d, d->indices[i], a, q), same = !memcmp(p, q, sizeof p);
    if (rt_read || same) {
        for (uint32_t i = 0; i < d->nindices; i++) pgraph_attr(d, d->indices[i], 0, p), x0 = p[0] < x0 ? p[0] : x0, x1 = p[0] > x1 ? p[0] : x1;
        /* the clip in the draw's own coordinates: a front-end draw's (ui_w, VIDEOMODE 720 / 1080) are 480-line */
        float c0 = s->ui_w ? s->clip_x * 640.0f / pgraph_hd_width() : (float)s->clip_x, c1 = s->ui_w ? (float)s->ui_w : c0 + s->clip_w;
        if (x0 <= c0 + 1 && x1 >= c1 - 1) return 1; /* within a pixel: a popup's backdrop is -0.5..639.5 */
    }
    *all = s->clip_w * s->aa_x < color->w || s->clip_h * s->aa_y < color->h;
    return 1 / g_widen;
}

/* ULTRAWIDE in a race and its replay: the HUD goes to the screen's sides. A squeezed draw whose middle is in the left third of the
 * 640 columns keeps its distance from the left edge, in the right third from the right edge, else stays in the
 * middle 16:9 (so do menus, and the pause menu over a race). Where a draw lands is its program's position, run here
 * for its vertices; the track map (squeezed whole, *all) goes by its viewport. Returns 0 left, 0.5 middle, 1 right.
 * w: the screen's width in the draw's units. */
static float hud_side(const Draw *d, int all, float w) {
    const Surface *s = &d->surf;
    if (!forza_race_view()) return 0.5f;
    float x0 = 1e30f, x1 = -1e30f, p[4], v[16][4];
    if (all) {
        float k = s->ui_w ? 640.0f / pgraph_hd_width() : 1;
        x0 = s->clip_x * k, x1 = x0 + s->clip_w * k;
    } else { /* 8 vertices spread over the draw (a string's first and last glyphs among them): thirds are coarse,
              * and every vertex of every depth-test-off draw (the sky, glare, smoke) took ~20% of the GPU thread */
        uint32_t n = d->nindices < 8 ? d->nindices : 8;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t i = n > 1 ? (uint32_t)((uint64_t)k * (d->nindices - 1) / (n - 1)) : 0;
            for (int a = 0; a < 16; a++) pgraph_attr(d, d->indices[i], a, v[a]);
            if (d->program) vsh_position(d->program, d->program_start, d->constants, (const float(*)[4])v, p);
            else memcpy(p, v[0], sizeof p);
            if (!k && d->program && p[3] != 1 && p[3] != 0 && !isinf(p[3]))
                return 0.5f; /* not 2D: the shader leaves it be (w 1, or 0 as the text's program leaves it) */
            x0 = p[0] < x0 ? p[0] : x0, x1 = p[0] > x1 ? p[0] : x1;
        }
    }
    float m = (x0 + x1) / 2;
    return m < w / 3 ? 0 : m > w * 2 / 3 ? 1 : 0.5f;
}

/* A draw's vertices into g_vring, read by the vertex shader (shaders.c vfetch), and in k where each attribute is: the
 * arrays' own bytes over the vertices the indices span, ranges that overlap (an interleaved buffer) copied once, each
 * at its guest address's offset in a dword; or the decoded vertices, sixteen float4 attributes each. A race frame
 * decoded ~240,000 vertices into 61 MB this way replaces. */
static void vertex_upload(const Draw *d, Constants *k) {
    UINT at;
    if (d->verts) {
        UINT bytes = d->nverts * (UINT)sizeof(Vertex);
        memcpy(ring_map(&g_vring, D3D11_BIND_SHADER_RESOURCE, bytes, 16, &at), d->verts, bytes);
        ring_unmap(&g_vring);
        for (uint32_t a = 0; a < 16; a++)
            k->va[a][0] = 2 | 4 << 4, k->va[a][1] = sizeof(Vertex), k->va[a][2] = at + 16 * a; /* float, 4 */
        return;
    }
    uint32_t lo[16], hi[16], grp[16], at_of[16], ng = 0, total = 0, order[16], n = 0;
    for (uint32_t a = 0; a < 16; a++) {
        uint32_t fmt = d->arr[a].fmt, type = fmt & 15, size = (fmt >> 4) & 15, stride = fmt >> 8;
        k->va[a][0] = fmt & 0xFF, k->va[a][1] = stride;
        if (!size) {
            memcpy(k->vk[a], d->konst[a], sizeof k->vk[a]);
            continue;
        }
        uint32_t bytes = type == 2 ? 4 * size : type == 1 || type == 5 ? 2 * size : type == 6 ? 4 : size;
        lo[a] = d->arr[a].addr + d->vmin * stride, hi[a] = d->arr[a].addr + d->vmax * stride + bytes;
        uint32_t j = n++;
        for (; j && lo[order[j - 1]] > lo[a]; j--) order[j] = order[j - 1]; /* by where their bytes start */
        order[j] = a;
    }
    uint32_t glo[16], ghi[16];
    for (uint32_t i = 0; i < n; i++) { /* overlapping ranges merge */
        uint32_t a = order[i];
        if (ng && lo[a] <= ghi[ng - 1]) ghi[ng - 1] = hi[a] > ghi[ng - 1] ? hi[a] : ghi[ng - 1];
        else glo[ng] = lo[a], ghi[ng++] = hi[a];
        grp[a] = ng - 1;
    }
    for (uint32_t g = 0; g < ng; g++) total = ((total + 15) & ~15u) + (glo[g] & 3), at_of[g] = total, total += ghi[g] - glo[g];
    uint8_t *dst = ring_map(&g_vring, D3D11_BIND_SHADER_RESOURCE, total ? total : 4, 16, &at);
    for (uint32_t g = 0; g < ng; g++) memcpy(dst + at_of[g], g_mem + glo[g], ghi[g] - glo[g]);
    ring_unmap(&g_vring);
    for (uint32_t i = 0; i < n; i++) { /* vertex 0's byte; vmin's and up are in the ring */
        uint32_t a = order[i];
        k->va[a][2] = at + at_of[grp[a]] + d->arr[a].addr - glo[grp[a]];
    }
}

void render_draw(const Draw *d) {
    static char src[65536];
    const Surface *s = &d->surf;
    Target *color = target(s->color_addr, s->width, s->height, 0);
    Target *depth = s->zeta_addr ? target(s->zeta_addr, s->width, s->height, 1) : NULL;

    int mv = g_mv_on && !g_supersampled && depth && g_k[KM(0x030C)] && s->width / s->aa_x >= g_present_w && s->height / s->aa_y >= g_present_h;

    /* shaders */
    uint64_t vkey = d->program ? vsh_key(d->program, d->program_start) : ~0ull;
    void **vs = keyed(0, vkey ^ (mv ? 0x4D56000000000000ull : 0)); /* "MV": the variants with motion */
    if (!*vs) {
        if (d->program) vsh_generate(d->program, d->program_start, src, sizeof src, mv);
        else vsh_fixed(src, sizeof src, mv);
        *vs = shader(src, 0);
    }
    void **ps = keyed(1, psh_key(g_k) ^ (mv ? 0x4D56000000000000ull : 0));
    if (!*ps) {
        psh_generate(g_k, src, sizeof src, mv);
        *ps = shader(src, 1);
    }

    /* constants */
    static Constants k;
    memcpy(k.c, d->constants, sizeof k.c);
    uint32_t clip_max = g_k[KM(0x0398)];
    float zmax;
    memcpy(&zmax, &clip_max, 4);
    /* programs emit screen coordinates in non-anti-aliased pixels; the hardware applies the AA scale */
    k.surf[0] = s->ui_w ? (float)s->ui_w : (float)s->width / s->aa_x;
    k.surf[1] = s->ui_h ? (float)s->ui_h : (float)s->height / s->aa_y;
    k.surf[2] = zmax > 0 ? 1.0f / zmax : 1.0f / 16777215.0f;
    k.surf[3] = (float)((g_k[KM(0x0290)] >> 16) & g_wbuf); /* w-buffered: depth is w (shaders.c) */
    for (int i = 0; i < 8; i++) {
        argb4(g_k[KM(0x0A60) + i], k.factor0[i]);
        argb4(g_k[KM(0x0A80) + i], k.factor1[i]);
    }
    argb4(g_k[KM(0x02A8)], k.fogcolor);
    k.alpharef[0] = (float)(g_k[KM(0x0340)] & 0xFF);
    if (k.surf[3] > 0 && g_k[KM(0x0338)]) { /* w-buffered with polygon offset: the pixel shader adds it (it writes depth) */
        float factor, bias;
        memcpy(&factor, &g_k[KM(0x0384)], 4), memcpy(&bias, &g_k[KM(0x0388)], 4);
        k.alpharef[1] = factor * RENDER_SCALE, k.alpharef[2] = bias * k.surf[2]; /* GL's sign, as the rasterizer's */
    } else k.alpharef[1] = k.alpharef[2] = 0;
    argb4(g_k[KM(0x1E20)], k.final0);
    argb4(g_k[KM(0x1E24)], k.final1);

    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    int rt_read = 0; /* a unit samples a render target */
    for (int t = 0; t < 4; t++) {
        float sc[2] = {1, 1};
        srv[t] = d->tex[t].enabled ? texture(&d->tex[t], sc, s->ui_w != 0) : g_dummy_srv;
        if (d->tex[t].enabled && tex_kind(d->tex[t].format) == TK_2D && find_target(d->tex[t].addr, 0)) rt_read = 1;
        smp[t] = sampler(&d->tex[t]);
        k.texscale[t][0] = sc[0];
        k.texscale[t][1] = sc[1];
        k.texscale[t][2] = (float)RENDER_SCALE; /* host texels a guest texel of a rendered texture spans (shadow maps) */
    }
    int all;
    float sw = s->ui_w ? 640 : color->w / (float)s->aa_x; /* the screen's width in the draw's (non-AA) pixels */
    k.ui[0] = ui_squeeze(d, color, rt_read, &all);
    float side = k.ui[0] != 1 ? hud_side(d, all, sw) : 0.5f;
    k.ui[1] = side * sw; /* squeezed about the screen's middle, or the HUD's side */
    k.ui[2] = (float)all;
    if (all) k.surf[0] = sw; /* positions over the screen's viewport (below) */
    vertex_upload(d, &k);
    for (int t = 0; t < 4 && g_mv_on && g_scene && color != g_scene; t++)
        if (srv[t] == g_scene->srv) { /* the scene, resolved into a frame: its motion goes with that frame */
            snapshot_scene(color->addr);
            break;
        }
    for (int t = 0; t < 4; t++) /* a target cannot be bound for reading and writing at once */
        if (srv[t] == color->srv || (depth && srv[t] == depth->srv)) {
            static int warned;
            if (!warned++) fprintf(stderr, "[render] draw samples its own render target; unit unbound\n");
            srv[t] = dummy(tex_kind(d->tex[t].format));
        }
    ID3D11Buffer *cbs[2] = {g_cb, g_cbp};
    UINT cb_first[2] = {0, 0}, cb_num[2] = {(sizeof k + 255) / 256 * 16, 0}; /* in constants, multiples of 16 */
    if (g_kring_ok) cb_first[0] = ring_put(&g_kring, D3D11_BIND_CONSTANT_BUFFER, &k, sizeof k, 256) / 16, cbs[0] = g_kring.buf;
    else upload(&g_cb, &(UINT){4096}, D3D11_BIND_CONSTANT_BUFFER, &k, sizeof k);
    static struct {
        float cp[192][4], jitter[4];
    } p;
    cb_num[1] = (sizeof p + 255) / 256 * 16;
    if (mv) { /* last frame's constants for this draw, and the jitter in clip units */
        uint32_t first[8] = {d->nverts, d->nindices, d->topology};
        pgraph_attr(d, d->indices[0], 0, (float *)(first + 3)); /* the first vertex's position */
        uint64_t base = hash_bytes(first, sizeof first, vkey);
        memcpy(p.cp, prev_constants(base, (const float(*)[4])k.c), sizeof p.cp);
        p.jitter[0] = 2 * g_jitter[0] / g_present_hw;
        p.jitter[1] = -2 * g_jitter[1] / (g_present_h * RENDER_SCALE);
        static UINT cbp_size;
        upload(&g_cbp, &cbp_size, D3D11_BIND_CONSTANT_BUFFER, &p, sizeof p);
        cbs[1] = g_cbp;
        motion_target(color);
        g_scene = color, g_scene_z = depth;
    }

    /* geometry */
    UINT ioff = ring_put(&g_iring, D3D11_BIND_INDEX_BUFFER, d->indices, d->nindices * 4, 4);

    /* pipeline */
    ID3D11RenderTargetView *rtvs[2] = {color->rtv, mv ? color->mv_rtv : NULL};
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, mv && color->mv_rtv ? 2 : 1, rtvs, depth ? depth->dsv : NULL);
    float xs = (float)color->hw / color->w; /* host pixels per guest pixel across */
    /* a draw squeezed whole goes past its clip's viewport, which D3D would cut it to: it gets the screen's */
    D3D11_VIEWPORT vp = {0, 0, all ? (float)color->hw : s->width * xs, (float)(s->height * RENDER_SCALE), 0, 1};
    ID3D11DeviceContext1_RSSetViewports(g_ctx, 1, &vp);
    float cx = color->hw * side, sx0 = s->clip_x * s->aa_x * xs, sx1 = s->width * xs; /* a squeezed draw's clip is too */
    if (k.ui[0] != 1) sx0 = cx + (sx0 - cx) * k.ui[0], sx1 = cx + (sx1 - cx) * k.ui[0];
    D3D11_RECT sc = {(LONG)sx0, (LONG)(s->clip_y * s->aa_y * RENDER_SCALE), (LONG)sx1, (LONG)(s->height * RENDER_SCALE)};
    ID3D11DeviceContext1_RSSetScissorRects(g_ctx, 1, &sc);
    apply_states(mv);
    ID3D11DeviceContext1_IASetInputLayout(g_ctx, NULL); /* the vertex shader reads g_vring */
    ID3D11DeviceContext1_VSSetShaderResources(g_ctx, 0, 1, &g_vring.srv);
    ID3D11DeviceContext1_IASetIndexBuffer(g_ctx, g_iring.buf, DXGI_FORMAT_R32_UINT, ioff);
    static const D3D11_PRIMITIVE_TOPOLOGY topo[3] = {D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
                                                     D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    ID3D11DeviceContext1_IASetPrimitiveTopology(g_ctx, topo[d->topology]);
    ID3D11DeviceContext1_VSSetShader(g_ctx, *vs, NULL, 0);
    ID3D11DeviceContext1_VSSetConstantBuffers1(g_ctx, 0, mv ? 2 : 1, cbs, g_kring_ok ? cb_first : NULL, g_kring_ok ? cb_num : NULL);
    ID3D11DeviceContext1_PSSetShader(g_ctx, *ps, NULL, 0);
    ID3D11DeviceContext1_PSSetConstantBuffers1(g_ctx, 0, 1, cbs, g_kring_ok ? cb_first : NULL, g_kring_ok ? cb_num : NULL);
    ID3D11DeviceContext1_PSSetShaderResources(g_ctx, 0, 4, srv);
    ID3D11DeviceContext1_PSSetSamplers(g_ctx, 0, 4, smp);
    ID3D11DeviceContext1_DrawIndexed(g_ctx, d->nindices, 0, 0);
    /* unbind so the next draw may render into a target this one sampled */
    ID3D11ShaderResourceView *none[4] = {0};
    ID3D11DeviceContext1_PSSetShaderResources(g_ctx, 0, 4, none);
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
}

/* ---------------- video overlay ---------------- */

static struct {
    int on;
    uint32_t addr, w, h, pitch, format;
    int dst[4];
    int key_on;
    uint32_t key;
} g_ov;

static void render_overlay_(int enable, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, const int *dst,
                    int key_on, uint32_t key) {
    if (getenv("OVLOG") && (enable != g_ov.on || (addr && (addr != g_ov.addr || key_on != g_ov.key_on))))
        fprintf(stderr, "[overlay] %s addr %08X %ux%u key %d %08X dst %d,%d-%d,%d\n", enable ? "on" : "off", addr, w, h,
                key_on, key, dst ? dst[0] : -1, dst ? dst[1] : -1, dst ? dst[2] : -1, dst ? dst[3] : -1);
    g_ov.on = enable;
    if (!addr) return; /* EnableOverlay: keep the last surface */
    g_ov.addr = addr, g_ov.w = w, g_ov.h = h, g_ov.pitch = pitch, g_ov.format = format;
    if (dst) memcpy(g_ov.dst, dst, sizeof g_ov.dst);
    else memset(g_ov.dst, 0, sizeof g_ov.dst); /* none: the whole frame, 640x480 or the game's HD mode */
    g_ov.key_on = key_on, g_ov.key = key;
}
void render_overlay(int enable, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, const int *dst,
                    int key_on, uint32_t key) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    render_overlay_(enable, addr, w, h, pitch, format, dst, key_on, key);
    gpu_leave();
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* draw the overlay surface (YUY2, or UYVY for format 0x25) into its destination rectangle on the back buffer. With a
 * colour key, like the scanout hardware it shows only where the framebuffer `fb` holds the key colour. */
static void composite_overlay(ID3D11Texture2D *back, ID3D11ShaderResourceView *fb) {
    static ID3D11Texture2D *tex;
    static ID3D11ShaderResourceView *srv;
    static uint32_t tw, th;
    static ID3D11VertexShader *vs;
    static ID3D11PixelShader *ps;
    static ID3D11SamplerState *smp;
    if (!g_ov.on || !g_ov.addr || g_ov.w > 2048 || g_ov.h > 2048) return;
    ID3D11ShaderResourceView *video = video_for(g_ov.addr); /* an upscaled cutscene frame (video.c) */
    if (!vs) {
        ID3DBlob *b = compile("void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {"
                              " uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }",
                              "vs_5_0");
        ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &vs);
        ID3D10Blob_Release(b);
        b = compile("Texture2D t : register(t0); Texture2D fb : register(t1); SamplerState s : register(s0);"
                    " cbuffer K : register(b0) { float4 key; };" /* rgb; w > 0: keyed */
                    " float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                    "  if (key.w > 0 && any(abs(fb.Load(int3(pos.xy, 0)).rgb - key.rgb) > 0.5 / 255)) discard;"
                    "  return t.Sample(s, uv); }",
                    "ps_5_0");
        ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps);
        ID3D10Blob_Release(b);
        D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP};
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(g_dev, &sd, &smp);
    }
    if (!tex || tw != g_ov.w || th != g_ov.h) {
        if (tex) ID3D11ShaderResourceView_Release(srv), ID3D11Texture2D_Release(tex);
        D3D11_TEXTURE2D_DESC td = {g_ov.w, g_ov.h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DYNAMIC,
                                   D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0};
        ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &tex);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tex, NULL, &srv);
        tw = g_ov.w, th = g_ov.h;
    }
    if (video) goto draw;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext1_Map(g_ctx, (ID3D11Resource *)tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    int uyvy = g_ov.format == 0x25;
    for (uint32_t y = 0; y < g_ov.h; y++) {
        const uint8_t *src = g_mem + g_ov.addr + y * g_ov.pitch;
        uint8_t *dst = (uint8_t *)m.pData + y * m.RowPitch;
        for (uint32_t x = 0; x + 1 < g_ov.w; x += 2, src += 4) {
            int y0 = uyvy ? src[1] : src[0], u = uyvy ? src[0] : src[1], y1 = uyvy ? src[3] : src[2], v = uyvy ? src[2] : src[3];
            int d = u - 128, e = v - 128;
            for (int k = 0; k < 2; k++) { /* BT.601 */
                int cc = (k ? y1 : y0) - 16;
                uint8_t *p = dst + 4 * (x + k);
                p[2] = clamp8((298 * cc + 409 * e + 128) >> 8);
                p[1] = clamp8((298 * cc - 100 * d - 208 * e + 128) >> 8);
                p[0] = clamp8((298 * cc + 516 * d + 128) >> 8);
                p[3] = 255;
            }
        }
    }
    ID3D11DeviceContext1_Unmap(g_ctx, (ID3D11Resource *)tex, 0);

draw:;
    ID3D11RenderTargetView *rtv;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &rtv))) return;
    /* the movie player places the overlay in 640x480 screen space even in Forza's HD modes (VIDEOMODE): scale it to
     * the frame; no destination rectangle means the whole frame */
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(back, &bd);
    float sx = bd.Width / 640.0f / g_widen, sy = bd.Height / 480.0f, x0 = (bd.Width - 640 * sx) / 2; /* ULTRAWIDE: the middle 16:9 */
    D3D11_VIEWPORT vp = {x0 + g_ov.dst[0] * sx, g_ov.dst[1] * sy, (g_ov.dst[2] - g_ov.dst[0]) * sx,
                         (g_ov.dst[3] - g_ov.dst[1]) * sy, 0, 1};
    if (g_ov.dst[2] <= g_ov.dst[0]) vp.TopLeftX = x0, vp.TopLeftY = 0, vp.Width = 640 * sx, vp.Height = (float)bd.Height;
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext1_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext1_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext1_RSSetState(g_ctx, NULL);
    ID3D11DeviceContext1_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext1_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext1_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_VSSetShader(g_ctx, vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(g_ctx, ps, NULL, 0);
    static ID3D11Buffer *kb;
    float key[4] = {((g_ov.key >> 16) & 0xFF) / 255.0f, ((g_ov.key >> 8) & 0xFF) / 255.0f, (g_ov.key & 0xFF) / 255.0f,
                    g_ov.key_on && fb ? 1.0f : 0.0f};
    static UINT kb_size;
    upload(&kb, &kb_size, D3D11_BIND_CONSTANT_BUFFER, key, sizeof key);
    ID3D11DeviceContext1_PSSetConstantBuffers(g_ctx, 0, 1, &kb);
    ID3D11ShaderResourceView *srvs[2] = {video ? video : srv, fb};
    ID3D11DeviceContext1_PSSetShaderResources(g_ctx, 0, 2, srvs);
    ID3D11DeviceContext1_PSSetSamplers(g_ctx, 0, 1, &smp);
    ID3D11DeviceContext1_Draw(g_ctx, 3, 0);
    ID3D11ShaderResourceView *none[2] = {0};
    ID3D11DeviceContext1_PSSetShaderResources(g_ctx, 0, 2, none);
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(rtv);
}

/* SCREENSHOT=<n>: write the n-th presented frame to build/frame.bmp, and every colour target to build/rt_*.bmp */
static void screenshot(ID3D11Texture2D *tex, const char *path) {
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &staging))) return;
    ID3D11DeviceContext1_CopyResource(g_ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f = fopen(path, "wb");
    if (f && SUCCEEDED(ID3D11DeviceContext1_Map(g_ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        uint32_t w = td.Width, h = td.Height, size = w * h * 4;
        BITMAPFILEHEADER fh = {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
        BITMAPINFOHEADER ih = {sizeof ih, (LONG)w, -(LONG)h, 1, 32, BI_RGB};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        for (uint32_t y = 0; y < h; y++) fwrite((uint8_t *)m.pData + y * m.RowPitch, 4, w, f);
        ID3D11DeviceContext1_Unmap(g_ctx, (ID3D11Resource *)staging, 0);
        fprintf(stderr, "[render] saved %s (%ux%u)\n", path, w, h);
    }
    if (f) fclose(f);
    ID3D11Texture2D_Release(staging);
}

/* ---------------- presentation passes ---------------- */

static ID3D11PixelShader *pixel_shader(const char *src) {
    ID3DBlob *b = compile(src, "ps_5_0");
    ID3D11PixelShader *ps = NULL;
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps);
    ID3D10Blob_Release(b);
    return ps;
}

/* draw a triangle covering `vp` of `rtv` (uv 0..1 across it) with pixel shader `ps` reading `src`; `cb` (16 floats)
 * goes to b0; `over`: blend premultiplied alpha over what is there */
static void pass(ID3D11RenderTargetView *rtv, const D3D11_VIEWPORT *vp, ID3D11PixelShader *ps,
                 ID3D11ShaderResourceView *src, const float *cb, int over) {
    static ID3D11VertexShader *vs;
    static ID3D11SamplerState *smp;
    static ID3D11BlendState *premul;
    static ID3D11Buffer *cbuf;
    static UINT cbuf_size;
    if (!vs) {
        ID3DBlob *b = compile("void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {"
                              " uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }",
                              "vs_5_0");
        ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &vs);
        ID3D10Blob_Release(b);
        D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP};
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(g_dev, &sd, &smp);
        D3D11_BLEND_DESC d = {0};
        d.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                                                            D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                                                            D3D11_COLOR_WRITE_ENABLE_ALL};
        ID3D11Device_CreateBlendState(g_dev, &d, &premul);
    }
    if (cb) {
        upload(&cbuf, &cbuf_size, D3D11_BIND_CONSTANT_BUFFER, cb, 16 * sizeof(float));
        ID3D11DeviceContext1_PSSetConstantBuffers(g_ctx, 0, 1, &cbuf);
    }
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext1_OMSetBlendState(g_ctx, over ? premul : NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext1_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext1_RSSetState(g_ctx, NULL);
    ID3D11DeviceContext1_RSSetViewports(g_ctx, 1, vp);
    ID3D11DeviceContext1_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext1_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_VSSetShader(g_ctx, vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(g_ctx, ps, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(g_ctx, 0, 1, &src);
    ID3D11DeviceContext1_PSSetSamplers(g_ctx, 0, 1, &smp);
    ID3D11DeviceContext1_Draw(g_ctx, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext1_PSSetShaderResources(g_ctx, 0, 1, &none);
    ID3D11DeviceContext1_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
}

/* draw `src` over its rectangle `vp` of `dst`, bilinear; with `over` (premultiplied alpha), blended over it */
static void blit(ID3D11Texture2D *dst, const D3D11_VIEWPORT *vp, ID3D11ShaderResourceView *src, int over) {
    static ID3D11PixelShader *ps;
    if (!ps) ps = pixel_shader("Texture2D t : register(t0); SamplerState s : register(s0);"
                               " float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }");
    ID3D11RenderTargetView *rtv;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)dst, NULL, &rtv))) return;
    pass(rtv, vp, ps, src, NULL, over);
    ID3D11RenderTargetView_Release(rtv);
}

/* the frame `src` (sw x sh) shrunk into `vp` of `dst`, each pixel the average of the frame's pixels under it (an n x n
 * grid of bilinear taps over its footprint, n the frame's pixels it spans): blit's one tap used 4 of the 9 pixels
 * behind each at render scale 4 and threw the rest of the supersampling away. Not shrinking: blit's one tap. */
static void downsample(ID3D11Texture2D *dst, const D3D11_VIEWPORT *vp, ID3D11ShaderResourceView *src, UINT sw, UINT sh) {
    static ID3D11PixelShader *ps;
    if (!ps)
        ps = pixel_shader("Texture2D t : register(t0); SamplerState s : register(s0); cbuffer c : register(b0) { float4 size; };"
                          " float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                          " int2 n = clamp((int2)ceil(size.xy / size.zw - 0.01), 1, 8); float2 step = 1 / (size.zw * n);"
                          " float4 sum = 0; [loop] for (int y = 0; y < n.y; y++) [loop] for (int x = 0; x < n.x; x++)"
                          " sum += t.SampleLevel(s, uv + (float2(x, y) + 0.5 - n * 0.5) * step, 0); return sum / (n.x * n.y); }");
    float cb[16] = {(float)sw, (float)sh, vp->Width, vp->Height};
    ID3D11RenderTargetView *rtv;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)dst, NULL, &rtv))) return;
    pass(rtv, vp, ps, src, cb, 0);
    ID3D11RenderTargetView_Release(rtv);
}

/* ---------------- temporal upscaling (DLSS, FSR 3) ---------------- */

static int g_fsr; /* UPSCALING, below */

/* up12.cpp: DLSS (2) or FSR 3 (3) on the frame, its depth and motion; a view of the result, or NULL */
ID3D11ShaderResourceView *upscale_run(int which, ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *color,
                                      ID3D11Texture2D *depth, ID3D11Texture2D *motion, unsigned iw, unsigned ih,
                                      unsigned ow, unsigned oh, float jx, float jy, int reset, float ms);

typedef struct {
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11UnorderedAccessView *uav;
    UINT w, h;
} Surf;

static void surf(Surf *s, UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
    if (s->tex && s->w == w && s->h == h) return;
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->uav) ID3D11UnorderedAccessView_Release(s->uav);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    memset(s, 0, sizeof *s);
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &s->tex))) return;
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    if (bind & D3D11_BIND_RENDER_TARGET) ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
        ID3D11Device_CreateUnorderedAccessView(g_dev, (ID3D11Resource *)s->tex, NULL, &s->uav);
    s->w = w, s->h = h;
}

static void pass(ID3D11RenderTargetView *rtv, const D3D11_VIEWPORT *vp, ID3D11PixelShader *ps,
                 ID3D11ShaderResourceView *src, const float *cb, int over);
static void blit(ID3D11Texture2D *dst, const D3D11_VIEWPORT *vp, ID3D11ShaderResourceView *src, int over);

/* the frame `composed` (w x h) rebuilt by DLSS or FSR 3 from its scene's motion and depth (snap) into `back`'s
 * rectangle vp; 0 when it cannot (the caller scales it as before). Never smaller than the input: a frame larger than
 * the window is anti-aliased at its own size and then shrunk (twice the window or more: only shrunk, render_present).
 * Once per game frame; a frame shown again reuses it. */
/* the scene's motion and depth at the frame's size w x h (the scene is drawn at twice the width, for the game's
 * anti-aliasing): the nearest sample, by position; for the upscalers */
static Surf g_in_mv, g_in_z;
static const Snapshot *g_in_of;
static uint32_t g_in_frame;
static int frame_inputs(const Snapshot *snap, UINT w, UINT h) {
    if (g_in_of == snap && g_in_frame == snap->frame && g_in_mv.w == w && g_in_mv.h == h) return 1;
    surf(&g_in_mv, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET);
    surf(&g_in_z, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET);
    if (!g_in_mv.rtv || !g_in_z.rtv) return 0;
    static ID3D11PixelShader *take;
    if (!take)
        take = pixel_shader("Texture2D t : register(t0);"
                            " float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                            " uint sw, sh; t.GetDimensions(sw, sh); return t.Load(int3(uv * float2(sw, sh), 0)); }");
    D3D11_VIEWPORT ivp = {0, 0, (float)w, (float)h, 0, 1};
    pass(g_in_mv.rtv, &ivp, take, snap->mv_srv, NULL, 0);
    pass(g_in_z.rtv, &ivp, take, snap->depth_srv, NULL, 0);
    g_in_of = snap, g_in_frame = snap->frame;
    return 1;
}

static int temporal(ID3D11Texture2D *back, const D3D11_VIEWPORT *vp, ID3D11Texture2D *composed, UINT w, UINT h,
                    const Snapshot *snap) {
    static ID3D11ShaderResourceView *out;
    static uint32_t last = ~0u, last_addr;
    static int failed;
    UINT ow = (UINT)vp->Width, oh = (UINT)vp->Height;
    if (ow < w || oh < h) ow = w, oh = h;
    if (snap->frame != last || snap->addr != last_addr || !out) {
        if (!frame_inputs(snap, w, h)) return 0;
        int reset = last == ~0u || snap->frame - last > 4; /* no history: a cut, or the first frame */
        out = upscale_run(g_fsr, g_dev, (ID3D11DeviceContext *)g_ctx, composed, g_in_z.tex, g_in_mv.tex, w, h, ow, oh, snap->jitter[0],
                          snap->jitter[1], reset, 1000.0f / 60);
        if (!out) {
            if (!failed++) fprintf(stderr, "[render] temporal upscaling cannot run: FSR 1 instead, without jitter\n");
            render_fsr(1);
            return 0;
        }
        last = snap->frame, last_addr = snap->addr;
        if (DeleteFileA("build/up.now")) { /* creating build/up.now saves the upscaler's input and output (build/up_*.bmp) */
            ID3D11Resource *o;
            ID3D11ShaderResourceView_GetResource(out, &o);
            screenshot(composed, "build/up_color.bmp"), screenshot((ID3D11Texture2D *)o, "build/up_out.bmp");
            ID3D11Resource_Release(o);
        }
    }
    downsample(back, vp, out, ow, oh);
    return 1;
}

/* AMD FidelityFX Super Resolution 1 (MIT), as its ffx_fsr1.h does it: EASU, an edge-adaptive Lanczos-like upscale
 * over 12 taps, then RCAS, a contrast-adaptive sharpen, both in the frame's gamma space. Loads instead of gathers;
 * b0 = {input w, h, output w, h, output origin x, y, sharpness (2^-stops)}. */
static const char g_easu[] =
    "Texture2D t : register(t0); cbuffer C : register(b0) { float4 size; float4 vp; };\n"
    "float3 L(int2 p) { return t.Load(int3(clamp(p, int2(0, 0), int2(size.xy) - 1), 0)).rgb; }\n"
    "float Y(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "void Set(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
    "  float lenX = abs(lD - lB) / max(max(abs(lD - lC), abs(lC - lB)), 1e-5); dir.x += (lD - lB) * w;\n"
    "  lenX = saturate(lenX); len += lenX * lenX * w;\n"
    "  float lenY = abs(lE - lA) / max(max(abs(lE - lC), abs(lC - lA)), 1e-5); dir.y += (lE - lA) * w;\n"
    "  lenY = saturate(lenY); len += lenY * lenY * w; }\n"
    "void Tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {\n"
    "  float2 v = float2(dot(off, dir), dot(off, float2(-dir.y, dir.x))) * len;\n"
    "  float d2 = min(dot(v, v), clp), wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
    "  float w = (1.5625 * wB * wB - 0.5625) * (wA * wA); aC += c * w; aW += w; }\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 pp = pos.xy * size.xy / size.zw - 0.5, fp = floor(pp); pp -= fp; int2 f = int2(fp);\n"
    "  float3 b = L(f + int2(0, -1)), c = L(f + int2(1, -1)), e = L(f + int2(-1, 0)), F = L(f), g = L(f + int2(1, 0)),\n"
    "         h = L(f + int2(2, 0)), i = L(f + int2(-1, 1)), j = L(f + int2(0, 1)), k = L(f + int2(1, 1)),\n"
    "         l = L(f + int2(2, 1)), n = L(f + int2(0, 2)), o = L(f + int2(1, 2));\n"
    "  float bL = Y(b), cL = Y(c), eL = Y(e), fL = Y(F), gL = Y(g), hL = Y(h), iL = Y(i), jL = Y(j), kL = Y(k),\n"
    "        lL = Y(l), nL = Y(n), oL = Y(o);\n"
    "  float2 dir = 0; float len = 0;\n"
    "  Set(dir, len, (1 - pp.x) * (1 - pp.y), bL, eL, fL, gL, jL);\n"
    "  Set(dir, len, pp.x * (1 - pp.y), cL, fL, gL, hL, kL);\n"
    "  Set(dir, len, (1 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
    "  Set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
    "  float dirR = dot(dir, dir); bool zro = dirR < 1.0 / 32768.0;\n"
    "  dirR = zro ? 1.0 : rsqrt(dirR); dir.x = zro ? 1.0 : dir.x; dir *= dirR;\n"
    "  len = len * 0.5; len *= len;\n"
    "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len, clp = 1.0 / lob;\n"
    "  float3 mn = min(min(F, g), min(j, k)), mx = max(max(F, g), max(j, k)), aC = 0; float aW = 0;\n"
    "  Tap(aC, aW, float2(0, -1) - pp, dir, len2, lob, clp, b); Tap(aC, aW, float2(1, -1) - pp, dir, len2, lob, clp, c);\n"
    "  Tap(aC, aW, float2(-1, 1) - pp, dir, len2, lob, clp, i); Tap(aC, aW, float2(0, 1) - pp, dir, len2, lob, clp, j);\n"
    "  Tap(aC, aW, float2(0, 0) - pp, dir, len2, lob, clp, F); Tap(aC, aW, float2(-1, 0) - pp, dir, len2, lob, clp, e);\n"
    "  Tap(aC, aW, float2(1, 1) - pp, dir, len2, lob, clp, k); Tap(aC, aW, float2(2, 1) - pp, dir, len2, lob, clp, l);\n"
    "  Tap(aC, aW, float2(2, 0) - pp, dir, len2, lob, clp, h); Tap(aC, aW, float2(1, 0) - pp, dir, len2, lob, clp, g);\n"
    "  Tap(aC, aW, float2(1, 2) - pp, dir, len2, lob, clp, o); Tap(aC, aW, float2(0, 2) - pp, dir, len2, lob, clp, n);\n"
    "  return float4(min(mx, max(mn, aC / aW)), 1); }\n";
static const char g_rcas[] =
    "Texture2D t : register(t0); cbuffer C : register(b0) { float4 size; float4 vp; };\n"
    "float3 L(int2 p) { return t.Load(int3(clamp(p, int2(0, 0), int2(size.zw) - 1), 0)).rgb; }\n"
    "float Y(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 p = int2(pos.xy - vp.xy);\n"
    "  float3 b = L(p + int2(0, -1)), d = L(p + int2(-1, 0)), e = L(p), f = L(p + int2(1, 0)), h = L(p + int2(0, 1));\n"
    "  float bL = Y(b), dL = Y(d), eL = Y(e), fL = Y(f), hL = Y(h);\n"
    "  float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
    "  float nz = 1.0 - 0.5 * saturate(abs(0.25 * (bL + dL + fL + hL) - eL) / max(range, 1e-5));\n"
    "  float3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
    "  float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5), hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);\n"
    "  float3 l3 = max(-hitMin, hitMax);\n"
    "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(l3.r, max(l3.g, l3.b)), 0.0)) * vp.z * nz;\n"
    "  return float4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1); }\n";

/* draw `src` (w x h) upscaled over its rectangle `vp` of `dst` with FSR 1 */
static void fsr(ID3D11Texture2D *dst, const D3D11_VIEWPORT *vp, ID3D11ShaderResourceView *src, UINT w, UINT h) {
    static ID3D11PixelShader *easu, *rcas;
    static ID3D11Texture2D *mid;
    static ID3D11ShaderResourceView *mid_srv;
    static ID3D11RenderTargetView *mid_rtv;
    static UINT mw, mh;
    UINT ow = (UINT)vp->Width, oh = (UINT)vp->Height;
    if (!easu) easu = pixel_shader(g_easu), rcas = pixel_shader(g_rcas);
    if (!mid || mw != ow || mh != oh) { /* EASU's output, the picture's size */
        if (mid) ID3D11RenderTargetView_Release(mid_rtv), ID3D11ShaderResourceView_Release(mid_srv), ID3D11Texture2D_Release(mid);
        D3D11_TEXTURE2D_DESC td = {ow, oh, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &mid)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)mid, NULL, &mid_srv)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)mid, NULL, &mid_rtv)))
            fail("FSR target", 0);
        mw = ow, mh = oh;
    }
    float cb[16] = {(float)w, (float)h, (float)ow, (float)oh, vp->TopLeftX, vp->TopLeftY, 0.87f /* 0.2 stops */};
    D3D11_VIEWPORT whole = {0, 0, (float)ow, (float)oh, 0, 1};
    pass(mid_rtv, &whole, easu, src, cb, 0);
    ID3D11RenderTargetView *rtv;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)dst, NULL, &rtv))) return;
    pass(rtv, vp, rcas, mid_srv, cb, 0);
    ID3D11RenderTargetView_Release(rtv);
}

static int g_fsr = DEF_FSR;
/* UPSCALING: 0 bilinear, 1 FSR 1, 2 DLSS, 3 FSR 3; the temporal two need the scene's motion vectors and jitter */
static void render_fsr_(int mode) {
    g_fsr = mode;
    g_jitter_on = mode >= 2;
    render_motion(mode >= 2);
}
void render_fsr(int mode) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    render_fsr_(mode);
    gpu_leave();
}

/* the name and version in the bottom-right corner of the window, white over a soft shadow, drawn with GDI into a
 * texture whenever the window's height changes */
static void version_text(ID3D11Texture2D *back, UINT sw, UINT sh) {
    static ID3D11ShaderResourceView *srv;
    static UINT for_h, tw, th;
    static const char text[] = RECOMP_NAME " v" RECOMP_VERSION;
    if (!srv || for_h != sh) {
        if (srv) ID3D11ShaderResourceView_Release(srv), srv = NULL;
        for_h = sh;
        int px = sh / 60 < 12 ? 12 : sh / 60;
        HDC dc = CreateCompatibleDC(NULL);
        HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        HGDIOBJ old_font = SelectObject(dc, font);
        SIZE sz;
        GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
        tw = sz.cx + 2, th = sz.cy + 2;
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), (LONG)tw, -(LONG)th, 1, 32, BI_RGB}};
        uint32_t *bits;
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        HGDIOBJ old_bm = SelectObject(dc, bm);
        memset(bits, 0, tw * th * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, 0, 0, text, (int)strlen(text)); /* coverage only: GDI leaves alpha alone */
        GdiFlush();
        uint32_t *px_out = malloc(tw * th * 4);
        for (UINT y = 0; y < th; y++)
            for (UINT x = 0; x < tw; x++) { /* the text at (0,0), its shadow at (1,1), premultiplied, 80% opaque */
                float t = x < tw - 2 && y < th - 2 ? (bits[y * tw + x] & 0xFF) / 255.0f : 0;
                float sh_ = x && y ? (bits[(y - 1) * tw + x - 1] & 0xFF) / 255.0f * 0.7f : 0;
                float a = (t + sh_ * (1 - t)) * 0.8f, c = t * 0.8f;
                uint32_t ca = (uint32_t)(a * 255 + 0.5f), cc = (uint32_t)(c * 255 + 0.5f);
                px_out[y * tw + x] = ca << 24 | cc << 16 | cc << 8 | cc;
            }
        SelectObject(dc, old_bm), SelectObject(dc, old_font);
        DeleteObject(bm), DeleteObject(font), DeleteDC(dc);
        D3D11_TEXTURE2D_DESC td = {tw, th, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {px_out, tw * 4, 0};
        ID3D11Texture2D *t;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &t)))
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &srv), ID3D11Texture2D_Release(t);
        free(px_out);
    }
    float margin = (float)(th / 2);
    D3D11_VIEWPORT vp = {sw - tw - margin, sh - th - margin / 2, (float)tw, (float)th, 0, 1};
    if (srv) blit(back, &vp, srv, 1);
}

/* an achievement just earned (achieve.c): a dark panel at the top middle with the emblem (the exe's icon), ACHIEVEMENT
 * UNLOCKED, the name and how it was earned; it slides down in 0.3 s, stays 4 s and slides back up, one at a time.
 * Drawn with GDI into a texture (premultiplied), as version_text. */
static void toast_text(HDC dc, uint32_t *cov, int w, int h, float *px, int x, int y, int size, int weight, const char *s,
                       float r, float g, float b) {
    HFONT font = CreateFontA(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    memset(cov, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT), SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, x, y, s, (int)strlen(s));
    GdiFlush();
    for (int i = 0; i < w * h; i++) { /* the coverage over what is there, straight alpha */
        float c = (cov[i] & 0xFF) / 255.0f, *p = px + 4 * i;
        p[0] = p[0] * (1 - c) + r * c, p[1] = p[1] * (1 - c) + g * c, p[2] = p[2] * (1 - c) + b * c;
        p[3] = p[3] + c * (1 - p[3]);
    }
    SelectObject(dc, old), DeleteObject(font);
}

static void achieve_toast(ID3D11Texture2D *back, UINT sw, UINT sh) {
    static ID3D11ShaderResourceView *srv;
    static UINT for_h, tw, th;
    static int shown = -1;
    static ULONGLONG start;
    ULONGLONG now = GetTickCount64();
    if (shown >= 0 && now - start > 4600) shown = -1;
    if (shown < 0) {
        if (srv) ID3D11ShaderResourceView_Release(srv), srv = NULL;
        if ((shown = achieve_toast_next()) < 0) return;
        start = now, for_h = 0;
    }
    if (!srv || for_h != sh) {
        if (srv) ID3D11ShaderResourceView_Release(srv), srv = NULL;
        for_h = sh;
        int h = sh / 9 < 72 ? 72 : (int)sh / 9, w = h * 9 / 2, pad = h / 6, icon = h - 2 * pad;
        tw = w, th = h;
        float *px = calloc((size_t)w * h, 16);
        for (int y = 0; y < h; y++) /* the panel: near black, 90%, corners rounded by pad / 2; a red edge on the left */
            for (int x = 0; x < w; x++) {
                int rr = pad / 2, cx = x < rr ? rr - x : x >= w - rr ? x - (w - rr - 1) : 0,
                    cy = y < rr ? rr - y : y >= h - rr ? y - (h - rr - 1) : 0;
                if (cx * cx + cy * cy > rr * rr) continue;
                float *p = px + 4 * (y * w + x);
                int edge = x < h / 18 + 1;
                p[0] = edge ? 0.86f : 0.07f, p[1] = edge ? 0.12f : 0.07f, p[2] = edge ? 0.15f : 0.08f, p[3] = 0.9f;
            }
        HICON ico = (HICON)LoadImageA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1), IMAGE_ICON, icon, icon, 0);
        ICONINFO ii;
        if (ico && GetIconInfo(ico, &ii)) { /* the emblem's 32-bit colour, straight alpha, over the panel */
            BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), icon, -icon, 1, 32, BI_RGB}};
            uint32_t *ib = malloc((size_t)icon * icon * 4);
            HDC sdc = GetDC(NULL);
            if (ii.hbmColor && GetDIBits(sdc, ii.hbmColor, 0, icon, ib, &bi, DIB_RGB_COLORS))
                for (int y = 0; y < icon; y++)
                    for (int x = 0; x < icon; x++) {
                        uint32_t v = ib[y * icon + x];
                        float a = (v >> 24) / 255.0f, *p = px + 4 * ((y + pad) * w + x + pad + h / 18 + 2);
                        p[0] = p[0] * (1 - a) + ((v >> 16) & 0xFF) / 255.0f * a;
                        p[1] = p[1] * (1 - a) + ((v >> 8) & 0xFF) / 255.0f * a;
                        p[2] = p[2] * (1 - a) + (v & 0xFF) / 255.0f * a, p[3] = p[3] + a * (1 - p[3]);
                    }
            ReleaseDC(NULL, sdc), free(ib);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
        if (ico) DestroyIcon(ico);
        HDC dc = CreateCompatibleDC(NULL);
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
        uint32_t *cov;
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&cov, NULL, 0);
        HGDIOBJ old_bm = SelectObject(dc, bm);
        int tx = pad + h / 18 + 2 + icon + pad;
        toast_text(dc, cov, w, h, px, tx, pad * 3 / 4, h / 6, FW_BOLD, "ACHIEVEMENT UNLOCKED", 0.95f, 0.25f, 0.27f);
        toast_text(dc, cov, w, h, px, tx, pad * 3 / 4 + h / 5, h * 2 / 7, FW_BOLD, achieve_name(shown), 1, 1, 1);
        toast_text(dc, cov, w, h, px, tx, pad * 3 / 4 + h / 5 + h * 3 / 10, h / 6, FW_NORMAL, achieve_desc(shown), 0.75f,
                   0.75f, 0.78f);
        SelectObject(dc, old_bm), DeleteObject(bm), DeleteDC(dc);
        uint32_t *out = malloc((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) { /* premultiplied BGRA */
            float *p = px + 4 * i;
            uint32_t a = (uint32_t)(p[3] * 255 + 0.5f);
            out[i] = a << 24 | (uint32_t)(p[0] * p[3] * 255 + 0.5f) << 16 | (uint32_t)(p[1] * p[3] * 255 + 0.5f) << 8 |
                     (uint32_t)(p[2] * p[3] * 255 + 0.5f);
        }
        free(px);
        D3D11_TEXTURE2D_DESC td = {tw, th, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {out, tw * 4, 0};
        ID3D11Texture2D *t;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &t)))
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &srv), ID3D11Texture2D_Release(t);
        free(out);
    }
    float t = (now - start) / 1000.0f, in = t < 0.3f ? t / 0.3f : t > 4.3f ? (4.6f - t) / 0.3f : 1;
    in = in < 0 ? 0 : in * in * (3 - 2 * in); /* eased */
    D3D11_VIEWPORT vp = {(float)(sw - tw) / 2, -(float)th + in * (th + th / 5.0f), (float)tw, (float)th, 0, 1};
    if (srv) blit(back, &vp, srv, 1);
}

/* The CRTC now scans out the framebuffer at guest address `addr`: show its host target. The frame (640x480, or the
 * game's own 1280x720 / 1920x1080 with VIDEOMODE) and the video overlay are composed at RENDER_SCALE, then fitted
 * into the window at the picture's aspect, black bars around: 16:9 when the game renders anamorphic widescreen, as
 * it does with the widescreen video flag, and in the HD modes. The swap chain follows the window's size. */
static uint32_t g_shown, g_new; /* frames that reached the window, and new game frames among them (FPSLOG) */

/* A new game frame came more than 50 ms after the last (three frames late at 60): always logged, with what happened
 * in between, so the log of any session says what its stutters were. Not more than one line a second. */
static void hitch_check(void) {
    static LARGE_INTEGER hz, last, logged;
    static uint32_t compiles, uploads, targets, loads, evictions;
    static int64_t tex_ticks;
    LARGE_INTEGER now;
    if (!hz.QuadPart) QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&now);
    uint32_t tl, te;
    int queued;
    texpack_stats(&tl, &te, &queued);
    double gap = last.QuadPart ? (now.QuadPart - last.QuadPart) * 1000.0 / hz.QuadPart : 0;
    if (gap > 50 && (now.QuadPart - logged.QuadPart) > hz.QuadPart) {
        fprintf(stderr, "[hitch] %.0f ms between frames on %s: %u shaders compiled, %u textures decoded (%.1f ms), %u "
                "render targets made, texture pack %u loaded / %u dropped (%d waiting)\n", gap,
                *forza_screen() ? forza_screen() : "?", g_compiles_made - compiles, g_tex_uploads - uploads,
                (g_tex_ticks - tex_ticks) * 1000.0 / hz.QuadPart, g_targets_made - targets, tl - loads, te - evictions,
                queued);
        logged = now;
    }
    last = now, compiles = g_compiles_made, uploads = g_tex_uploads, targets = g_targets_made, loads = tl,
    evictions = te, tex_ticks = g_tex_ticks;
}
static void present(uint32_t addr) {
    static int frame, shot = -2;
    static char screen[64]; /* the screen on top, for resolve()'s waiting */
    if (strncmp(screen, forza_screen(), sizeof screen - 1)) strncpy(screen, forza_screen(), sizeof screen - 1), g_scene_frame = g_frame;
    static ID3D11Texture2D *composed;
    static ID3D11ShaderResourceView *composed_srv;
    static UINT cw, ch, sw, sh;
    if (shot == -2) shot = getenv("SCREENSHOT") ? atoi(getenv("SCREENSHOT")) : -1;
    if (frame % 15 == 0 && texpack_poll_keys()) render_texpack(1);
    texture_age();
    if (frame % 15 == 0 && DeleteFileA("build/wbuf.now")) fprintf(stderr, "[render] w-buffer %s\n", (g_wbuf ^= 1) ? "on" : "off");
    if (frame % 15 == 0 && DeleteFileA("build/rt.now")) shot = frame + 1; /* SCREENSHOT's pictures of the next frame, now */
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) { /* minimised */
        g_frame++;
        return;
    }
    if (!sw) sw = g_win_w, sh = g_win_h;
    if ((UINT)cr.right != sw || (UINT)cr.bottom != sh) {
        sw = cr.right, sh = cr.bottom;
        IDXGISwapChain1_ResizeBuffers(g_swap, 0, sw, sh, DXGI_FORMAT_UNKNOWN, 0);
    }
    float aspect = g_wide ? 16.0f / 9 * g_widen : 4.0f / 3;
    D3D11_VIEWPORT vp = {0, 0, (float)sw, (float)sh, 0, 1};
    if (vp.Width > vp.Height * aspect) vp.Width = floorf(vp.Height * aspect), vp.TopLeftX = floorf((sw - vp.Width) / 2);
    else vp.Height = floorf(vp.Width / aspect), vp.TopLeftY = floorf((sh - vp.Height) / 2); /* whole pixels, for FSR */
    Target *t = find_target(addr, 0);
    if (t) g_present_w = t->w, g_present_h = t->h, g_present_hw = t->hw; /* the scene is a target at least this size */
    UINT w = t ? t->hw : (UINT)(640 * RENDER_SCALE * g_widen), h = t ? t->hh : 480 * RENDER_SCALE;
    if (!composed || w != cw || h != ch) {
        if (composed) ID3D11ShaderResourceView_Release(composed_srv), ID3D11Texture2D_Release(composed);
        D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &composed)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)composed, NULL, &composed_srv)))
            fail("composed frame", 0);
        cw = w, ch = h;
        fprintf(stderr, "[render] presenting %ux%u frames\n", t ? t->w : 640, h / RENDER_SCALE);
    }
    ID3D11Texture2D *back;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return;
    if (t) {
        D3D11_BOX box = {0, 0, 0, w, h, 1};
        ID3D11DeviceContext1_CopySubresourceRegion(g_ctx, (ID3D11Resource *)composed, 0, 0, 0, 0,
                                                   (ID3D11Resource *)t->tex, 0, &box);
    }
    composite_overlay(composed, t ? t->srv : NULL);
    ID3D11RenderTargetView *back_rtv;
    if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &back_rtv))) {
        static const float black[4] = {0, 0, 0, 1}; /* the bars around the picture */
        ID3D11DeviceContext1_ClearRenderTargetView(g_ctx, back_rtv, black);
        ID3D11RenderTargetView_Release(back_rtv);
    }
    /* a frame at least twice the window's size each way is anti-aliased by shrinking it: DLSS or FSR 3 would run at
     * the frame's size (DLAA at 7680x4320 for 1080i at scale 4: ~99 new frames a second against 144 without) to be
     * shrunk anyway; they, their motion vectors and the jitter rest meanwhile */
    g_supersampled = vp.Width * 2 <= w && vp.Height * 2 <= h;
    const Snapshot *snap = t ? scene_of(t->addr) : NULL;
    static int show_mv = -1; /* MVSHOW=1: the scene's motion instead of the picture (red right, green down, x40) */
    if (show_mv < 0) show_mv = getenv("MVSHOW") != NULL;
    if (show_mv && snap) {
        static ID3D11PixelShader *mvps;
        if (!mvps)
            mvps = pixel_shader("Texture2D t : register(t0); SamplerState s : register(s0);"
                                " float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                                " float2 m = t.Sample(s, uv).xy * 40; return float4(max(m.x, 0) + max(-m.y, 0),"
                                " max(m.y, 0) + max(-m.y, 0) * 0.5, max(-m.x, 0), 1); }");
        ID3D11RenderTargetView *brtv;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &brtv)))
            pass(brtv, &vp, mvps, snap->mv_srv, NULL, 0), ID3D11RenderTargetView_Release(brtv);
    } else if (g_fsr >= 2 && !g_supersampled && snap && temporal(back, &vp, composed, w, h, snap)) {
    } else if (g_fsr && vp.Width >= w && vp.Height >= h && (vp.Width > w || vp.Height > h)) fsr(back, &vp, composed_srv, w, h);
    else downsample(back, &vp, composed_srv, w, h);
    version_text(back, sw, sh);
    achieve_toast(back, sw, sh);
    if (t && frame++ == shot) {
        screenshot(back, "build/frame.bmp");
        for (int i = 0; i < MAX_TARGETS; i++)
            if (g_targets[i].tex && !g_targets[i].depth) {
                char path[64];
                snprintf(path, sizeof path, "build/rt_%08X.bmp", g_targets[i].addr);
                screenshot(g_targets[i].tex, path);
            }
    }
    { /* SHOTEVERY=<n>: also save the screen every n frames to build/shot_<frame>.bmp */
        static int every = -2;
        if (every == -2) every = getenv("SHOTEVERY") ? atoi(getenv("SHOTEVERY")) : 0;
        if (every > 0 && frame % every == 0) {
            char path[64];
            snprintf(path, sizeof path, "build/shot_%d.bmp", frame);
            screenshot(back, path);
        }
    }
    ID3D11Texture2D_Release(back);
    static uint32_t last_addr;
    if (addr != last_addr) hitch_check(); /* the game flips between buffers: a new address is a new frame */
    g_new += addr != last_addr, last_addr = addr;
    g_frame++;
    /* pacing comes from the emulated vblank, and this thread holds the guest lock: never wait for the display (above
     * its refresh, a waiting present held the lock nearly always and the game stopped); a frame it has no room for is
     * dropped, the next vblank brings a newer one */
    if (IDXGISwapChain1_Present(g_swap, 0, DXGI_PRESENT_DO_NOT_WAIT) != DXGI_ERROR_WAS_STILL_DRAWING) g_shown++;
}

/* FPSLOG=1: every 2 s, the presents per second, how evenly they come (average and worst spacing, how many over 20 ms)
 * and the time spent presenting each */
void render_present(uint32_t addr) {
    static int log = -1, n, slow;
    static LARGE_INTEGER f, t0, last;
    static double worst, busy;
    if (log < 0) log = getenv("FPSLOG") != NULL, QueryPerformanceFrequency(&f);
    if (!log) {
        present(addr);
        return;
    }
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    present(addr);
    QueryPerformanceCounter(&end);
    busy += (end.QuadPart - start.QuadPart) * 1000.0 / f.QuadPart;
    if (last.QuadPart) {
        double ms = (start.QuadPart - last.QuadPart) * 1000.0 / f.QuadPart;
        if (ms > worst) worst = ms;
        slow += ms > 20, n++;
        static int all = -1; /* FPSLOG=2: every spacing too */
        if (all < 0) all = atoi(getenv("FPSLOG")) == 2;
        if (all) fprintf(stderr, "[fps] %.1f%s", ms, n % 20 ? "" : "\n");
    } else t0 = start;
    last = start;
    double span = (start.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    if (span >= 2000 && n) {
        static int64_t push0;
        static uint32_t uploads0;
        int64_t push = nv2a_push_ticks();
        fprintf(stderr, "[fps] %.1f presents/s (%.1f shown, %.1f new), %.1f ms apart (worst %.1f, %d over 20 ms), %.2f ms presenting, "
                "%.2f ms of push buffer (%.2f ms hashing %.1f MB of textures, %u decoded); %d shaders compiled in %.0f ms\n",
                n * 1000.0 / span, g_shown * 1000.0 / span, g_new * 1000.0 / span, span / n, worst, slow, busy / n,
                (push - push0) * 1000.0 / f.QuadPart / n, g_hash_ticks * 1000.0 / f.QuadPart / n, g_hash_bytes / 1e6 / n,
                g_tex_uploads - uploads0, g_compiles, g_compile_ticks * 1000.0 / f.QuadPart);
        n = slow = 0, worst = busy = 0, t0 = start, push0 = push, g_hash_ticks = g_hash_bytes = 0, g_shown = g_new = 0;
        uploads0 = g_tex_uploads;
        g_compiles = 0, g_compile_ticks = 0;
    }
}
