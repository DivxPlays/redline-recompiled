/* The temporal upscalers, DLSS (NVIDIA DLSS SDK 310.9.16, NGX) and FSR 3 (AMD FSR SDK 2.4.0, FSR 3.1 upscaling), run
 * on a D3D12 device next to the renderer's D3D11 one: FSR ships for DirectX 12 and Vulkan only, and NGX's D3D11 path
 * shares textures with CUDA, which here read and wrote the wrong ones (a tile of the frame, or a texture from the
 * pack). C++ because the vendors' headers are; render.c calls the C functions at the end.
 *
 * The two devices share four textures (the frame, its depth and motion at the frame's size, and the output) and a
 * fence. Each frame D3D11 copies the inputs and signals; the D3D12 queue waits for that, runs the upscaler and
 * signals back; D3D11's GPU waits for it and draws the output into the window. Command allocators alternate, each
 * reused only once the queue is past the frame that last used it.
 *
 * Input is the frame the game composed (its HUD included: there is no picture without it), its depth (0 near, 1 far)
 * and the scene's motion (render.c: screen UV, a frame ago minus now, unjittered), with the camera jitter the scene
 * was drawn with, in input pixels. DLSS's quality mode follows the actual ratio, from DLAA at 1x to Ultra Performance
 * at 3x; FSR 3 takes any ratio. Either is made again when a size changes. The DLLs (nvngx_dlss.dll and AMD's signed
 * amd_fidelityfx_*_dx12.dll) sit next to the exe: tools/build.py copies them from third_party (tools/fetch_sdks.py).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "nvsdk_ngx_helpers_d3d.h" /* not nvsdk_ngx_helpers.h, which pulls in the CUDA and Vulkan ones */
#define _WINDOWS /* ffx_api_loader.h loads the functions only for it */
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static ID3D11Device5 *g_d11;
static ID3D11DeviceContext4 *g_c11;
static ID3D12Device *g_d12;
static ID3D12CommandQueue *g_queue;
static ID3D12CommandAllocator *g_alloc[2];
static UINT64 g_alloc_used[2]; /* the fence value of the frame that last recorded with each */
static unsigned g_turn;        /* which allocator records next: they take turns */
static ID3D12GraphicsCommandList *g_list;
static ID3D11Fence *g_f11;
static ID3D12Fence *g_f12;
static UINT64 g_fence;
static int g_ready = -1; /* -1 not tried, 0 unavailable, 1 ready */

typedef struct {
    ID3D11Texture2D *t11;
    ID3D12Resource *t12;
    ID3D11ShaderResourceView *srv;
    UINT w, h;
} Shared;
static Shared g_color, g_depth, g_motion, g_out;

static void release(Shared *s) {
    if (s->srv) s->srv->Release();
    if (s->t12) s->t12->Release();
    if (s->t11) s->t11->Release();
    *s = Shared{};
}

/* a texture both devices see: made on D3D11 with an NT handle, opened on D3D12; bind: D3D11 bindings beyond reading */
static int shared(Shared *s, UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
    if (s->t11 && s->w == w && s->h == h) return 1;
    release(s);
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | bind, 0,
                               D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
    IDXGIResource1 *r = NULL;
    HANDLE h12 = NULL;
    if (FAILED(g_d11->CreateTexture2D(&td, NULL, &s->t11)) || FAILED(s->t11->QueryInterface(IID_PPV_ARGS(&r))) ||
        FAILED(r->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h12)) ||
        FAILED(g_d12->OpenSharedHandle(h12, IID_PPV_ARGS(&s->t12)))) {
        if (r) r->Release();
        if (h12) CloseHandle(h12);
        release(s);
        fprintf(stderr, "[upscale] cannot share a %ux%u texture with D3D12\n", w, h);
        return 0;
    }
    r->Release(), CloseHandle(h12);
    g_d11->CreateShaderResourceView(s->t11, NULL, &s->srv);
    s->w = w, s->h = h;
    return 1;
}

static double ms_since(LARGE_INTEGER t0) { /* the start-up lines say how long each step took */
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f), QueryPerformanceCounter(&t);
    return (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

static int init12(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if (g_ready >= 0) return g_ready;
    g_ready = 0;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&g_d11))) || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&g_c11))) ||
        FAILED(dev->QueryInterface(IID_PPV_ARGS(&xd))) || FAILED(xd->GetAdapter(&ad)) || FAILED(ad->GetDesc(&desc))) {
        fprintf(stderr, "[upscale] needs D3D11.4 (Windows 10 1703 or later)\n");
        return 0;
    }
    IDXGIFactory4 *f = NULL; /* the same GPU, by its LUID */
    IDXGIAdapter1 *same = NULL;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    if (f) f->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&same)), f->Release();
    xd->Release(), ad->Release();
    HRESULT hr = D3D12CreateDevice(same, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d12));
    if (same) same->Release();
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    HANDLE fh = NULL;
    if (FAILED(hr) || FAILED(g_d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))) ||
        FAILED(g_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[0]))) ||
        FAILED(g_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[1]))) ||
        FAILED(g_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], NULL, IID_PPV_ARGS(&g_list))) ||
        FAILED(g_d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_f11))) ||
        FAILED(g_f11->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &fh)) ||
        FAILED(g_d12->OpenSharedHandle(fh, IID_PPV_ARGS(&g_f12)))) {
        if (fh) CloseHandle(fh);
        fprintf(stderr, "[upscale] no D3D12 device to run the upscalers on (%08lX)\n", (unsigned long)hr);
        return 0;
    }
    CloseHandle(fh);
    g_list->Close();
    fprintf(stderr, "[upscale] D3D12 device ready (%.0f ms)\n", ms_since(t0));
    return g_ready = 1;
}

/* ---------------- DLSS ---------------- */

static NVSDK_NGX_Parameter *g_ngx;
static NVSDK_NGX_Handle *g_dlss;
static UINT g_dlss_in[2], g_dlss_out[2];
static int g_dlss_ok = -1;

static int dlss_ready(void) {
    if (g_dlss_ok >= 0) return g_dlss_ok;
    g_dlss_ok = 0;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (wchar_t *slash = wcsrchr(dir, L'\\')) *slash = 0;
    static NVSDK_NGX_FeatureCommonInfo info = {}; /* DLSSLOG=1: NGX's own log */
    if (getenv("DLSSLOG"))
        info.LoggingInfo.LoggingCallback = [](const char *m, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) { fprintf(stderr, "[ngx] %s", m); },
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    /* a project id of our own (any GUID string, for engines NGX does not know) */
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("8d6f2b3c-5e1a-4c7e-9b2d-7a41f0c3e6b9", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              "0.1.0", dir, g_d12, &info, NVSDK_NGX_Version_API);
    int available = 0;
    if (NVSDK_NGX_FAILED(r) || NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&g_ngx)) ||
        NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(g_ngx, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
        fprintf(stderr, "[dlss] not available (%08X): it needs an NVIDIA RTX GPU and a recent driver\n", (unsigned)r);
        return 0;
    }
    fprintf(stderr, "[dlss] ready (%.0f ms)\n", ms_since(t0));
    return g_dlss_ok = 1;
}

static int dlss_make(UINT iw, UINT ih, UINT ow, UINT oh) {
    if (g_dlss) NVSDK_NGX_D3D12_ReleaseFeature(g_dlss), g_dlss = NULL;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    double ratio = (double)ow / iw;
    NVSDK_NGX_PerfQuality_Value q = ratio <= 1.01   ? NVSDK_NGX_PerfQuality_Value_DLAA
                                    : ratio <= 1.55 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                    : ratio <= 1.8  ? NVSDK_NGX_PerfQuality_Value_Balanced
                                    : ratio <= 2.2  ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                    : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    NVSDK_NGX_DLSS_Create_Params p = {};
    p.Feature.InWidth = iw, p.Feature.InHeight = ih, p.Feature.InTargetWidth = ow, p.Feature.InTargetHeight = oh;
    p.Feature.InPerfQualityValue = q;
    p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; /* LDR, motion at the input's size, unjittered */
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(g_list, 1, 1, &g_dlss, g_ngx, &p);
    if (NVSDK_NGX_FAILED(r)) {
        fprintf(stderr, "[dlss] cannot create for %ux%u -> %ux%u (%08X)\n", iw, ih, ow, oh, (unsigned)r);
        g_dlss = NULL;
        return 0;
    }
    static const char *names[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
    fprintf(stderr, "[dlss] %ux%u -> %ux%u (%s, %.0f ms)\n", iw, ih, ow, oh, names[q], ms_since(t0));
    g_dlss_in[0] = iw, g_dlss_in[1] = ih, g_dlss_out[0] = ow, g_dlss_out[1] = oh;
    return 1;
}

static int dlss_record(UINT iw, UINT ih, UINT ow, UINT oh, float jx, float jy, int reset) {
    if (!dlss_ready()) return 0;
    if (!g_dlss || iw != g_dlss_in[0] || ih != g_dlss_in[1] || ow != g_dlss_out[0] || oh != g_dlss_out[1]) {
        if (!dlss_make(iw, ih, ow, oh)) return 0;
        reset = 1;
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
    e.Feature.pInColor = g_color.t12, e.Feature.pInOutput = g_out.t12;
    e.pInDepth = g_depth.t12, e.pInMotionVectors = g_motion.t12;
    e.InJitterOffsetX = jx, e.InJitterOffsetY = jy;
    e.InRenderSubrectDimensions.Width = iw, e.InRenderSubrectDimensions.Height = ih;
    e.InReset = reset;
    e.InMVScaleX = (float)iw, e.InMVScaleY = (float)ih; /* screen UV to input pixels */
    e.InPreExposure = e.InExposureScale = 1;
    NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(g_list, g_dlss, g_ngx, &e);
    if (NVSDK_NGX_FAILED(r)) {
        static int told;
        if (!told++) fprintf(stderr, "[dlss] evaluate failed (%08X)\n", (unsigned)r);
        return 0;
    }
    return 1;
}

/* ---------------- FSR 3 ---------------- */

static ffxFunctions g_ffx;
static ffxContext g_fsr;
static UINT g_fsr_in[2], g_fsr_out[2];
static int g_fsr_ok = -1;

static void fsr_message(uint32_t type, const wchar_t *m) { fwprintf(stderr, L"[fsr3] %s: %s\n", type ? L"warning" : L"error", m); }

static int fsr_ready(void) {
    if (g_fsr_ok >= 0) return g_fsr_ok;
    g_fsr_ok = 0;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    HMODULE m = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll"); /* finds the upscaler DLL next to it, by the exe */
    if (!m) {
        fprintf(stderr, "[fsr3] amd_fidelityfx_loader_dx12.dll is missing\n");
        return 0;
    }
    ffxLoadFunctions(&g_ffx, m);
    if (!g_ffx.CreateContext || !g_ffx.Dispatch || !g_ffx.DestroyContext) return 0;
    fprintf(stderr, "[fsr3] ready (%.0f ms)\n", ms_since(t0));
    return g_fsr_ok = 1;
}

static int fsr_make(UINT iw, UINT ih, UINT ow, UINT oh) {
    if (g_fsr) g_ffx.DestroyContext(&g_fsr, NULL), g_fsr = NULL;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    ffxCreateBackendDX12Desc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = g_d12;
    ffxCreateContextDescUpscale up = {};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &be.header;
    up.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE; /* the frame's colours are as displayed, not linear */
    up.maxRenderSize = {iw, ih};
    up.maxUpscaleSize = {ow, oh};
    up.fpMessage = fsr_message;
    ffxReturnCode_t r = g_ffx.CreateContext(&g_fsr, &up.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        fprintf(stderr, "[fsr3] cannot create for %ux%u -> %ux%u (%d)\n", iw, ih, ow, oh, (int)r);
        g_fsr = NULL;
        return 0;
    }
    fprintf(stderr, "[fsr3] %ux%u -> %ux%u (%.0f ms)\n", iw, ih, ow, oh, ms_since(t0));
    g_fsr_in[0] = iw, g_fsr_in[1] = ih, g_fsr_out[0] = ow, g_fsr_out[1] = oh;
    return 1;
}

static int fsr_record(UINT iw, UINT ih, UINT ow, UINT oh, float jx, float jy, int reset, float ms) {
    if (!fsr_ready()) return 0;
    if (!g_fsr || iw != g_fsr_in[0] || ih != g_fsr_in[1] || ow != g_fsr_out[0] || oh != g_fsr_out[1]) {
        if (!fsr_make(iw, ih, ow, oh)) return 0;
        reset = 1;
    }
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = g_list;
    d.color = ffxApiGetResourceDX12(g_color.t12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(g_depth.t12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(g_motion.t12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(g_out.t12, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {(float)iw, (float)ih}; /* screen UV to input pixels */
    d.renderSize = {iw, ih};
    d.upscaleSize = {ow, oh};
    d.enableSharpening = true, d.sharpness = 0.3f;
    d.frameTimeDelta = ms, d.preExposure = 1, d.reset = reset != 0;
    /* the game's camera is not known here: a racing camera's planes and field of view, for FSR's depth estimates */
    d.cameraNear = 0.1f, d.cameraFar = 3000.0f, d.cameraFovAngleVertical = 0.95f, d.viewSpaceToMetersFactor = 1;
    ffxReturnCode_t r = g_ffx.Dispatch(&g_fsr, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (!told++) fprintf(stderr, "[fsr3] dispatch failed (%d)\n", (int)r);
        return 0;
    }
    return 1;
}

/* ---------------- the frame ---------------- */

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r, b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from, b.Transition.StateAfter = to;
    g_list->ResourceBarrier(1, &b);
}

/* The slow first steps on a thread of their own, started with the game: NGX's start-up alone took 1.8 s, a freeze on
 * whichever frame first needed DLSS (car select, or a race's countdown). Until it is done, upscale_warming() says so
 * and the renderer scales those frames the plain way; upscale_run is not called meanwhile. */
static volatile LONG g_warming;
static ID3D11Device *g_warm_dev;
static ID3D11DeviceContext *g_warm_ctx;

static DWORD WINAPI warm_thread(void *p) {
    if (init12(g_warm_dev, g_warm_ctx)) (INT_PTR)p == 2 ? dlss_ready() : fsr_ready();
    InterlockedExchange(&g_warming, 0); /* a full barrier: what it set up is seen by whoever reads 0 */
    return 0;
}

extern "C" void upscale_warm(int which, ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if ((which != 2 && which != 3) || g_warming || (g_ready >= 0 && (which == 2 ? g_dlss_ok : g_fsr_ok) >= 0)) return;
    g_warm_dev = dev, g_warm_ctx = ctx;
    InterlockedExchange(&g_warming, 1);
    HANDLE h = CreateThread(NULL, 0, warm_thread, (void *)(INT_PTR)which, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_warming, 0); /* no thread: the first frame does it, as before */
}

extern "C" int upscale_warming(void) { return InterlockedCompareExchange(&g_warming, 0, 0) != 0; }

/* which (2 DLSS, 3 FSR 3): the frame `color` (iw x ih), its depth (R32F) and motion (RG16F) -> a view of the
 * upscaled picture (ow x oh), or NULL when the upscaler cannot run (the caller scales as before) */
extern "C" ID3D11ShaderResourceView *upscale_run(int which, ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *color,
                                                 ID3D11Texture2D *depth, ID3D11Texture2D *motion, unsigned iw, unsigned ih,
                                                 unsigned ow, unsigned oh, float jx, float jy, int reset, float ms) {
    if (!init12(dev, ctx) || (which == 2 && !dlss_ready()) || (which == 3 && !fsr_ready())) return NULL;
    if (!shared(&g_color, iw, ih, DXGI_FORMAT_B8G8R8A8_UNORM, 0) || !shared(&g_depth, iw, ih, DXGI_FORMAT_R32_FLOAT, 0) ||
        !shared(&g_motion, iw, ih, DXGI_FORMAT_R16G16_FLOAT, 0) ||
        !shared(&g_out, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS))
        return NULL;
    g_c11->CopyResource(g_color.t11, color);
    g_c11->CopyResource(g_depth.t11, depth);
    g_c11->CopyResource(g_motion.t11, motion);
    g_c11->Signal(g_f11, ++g_fence), g_c11->Flush(); /* inputs written; submitted now, or the queue waits for it */
    g_queue->Wait(g_f12, g_fence);

    int k = (int)(g_turn++ & 1); /* an allocator the queue is done with */
    if (g_f12->GetCompletedValue() < g_alloc_used[k]) {
        HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_f12->SetEventOnCompletion(g_alloc_used[k], e);
        WaitForSingleObject(e, 2000);
        CloseHandle(e);
    }
    g_alloc[k]->Reset();
    g_list->Reset(g_alloc[k], NULL);
    ID3D12Resource *in[3] = {g_color.t12, g_depth.t12, g_motion.t12};
    for (int i = 0; i < 3; i++) barrier(in[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(g_out.t12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    int ok = which == 2 ? dlss_record(iw, ih, ow, oh, jx, jy, reset) : fsr_record(iw, ih, ow, oh, jx, jy, reset, ms);
    for (int i = 0; i < 3; i++) barrier(in[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(g_out.t12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    g_list->Close();
    ID3D12CommandList *lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_f12, ++g_fence); /* output written */
    g_alloc_used[k] = g_fence;
    g_c11->Wait(g_f11, g_fence);
    return ok ? g_out.srv : NULL;
}
