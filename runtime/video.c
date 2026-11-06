/* Upscaled cutscenes. The game keeps playing its Bink videos (timing, sound, which frame); for each one with a
 * build\videos\<path under media>.mp4 (tools/vidpack.py: 4x Real-ESRGAN, 1920x1440 H.264), the frame Bink copies to
 * a buffer is shown from the .mp4 instead, wherever that buffer is drawn: the video overlay (intro, attract) or a
 * texture (the event and track previews in the menus). hle_bink.c reports opens and copies. Each video decodes with
 * Media Foundation on its own thread, a few frames ahead of the one asked for; the renderer uploads the newest
 * decoded frame not past it, and keeps the original while there is none yet.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>

#include "pgraph.h"
#include "video.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#define SLOTS 4 /* videos open at once */
#define RING 8  /* decoded frames kept per video */
#define AHEAD 4 /* frames decoded past the one asked for */
#define DESTS 4 /* buffers a video copies frames to (Forza's alternate between two) */

typedef struct {
    uint32_t addr; /* guest buffer (physical), 0 none */
    uint32_t span; /* its bytes (pitch x height) */
    uint64_t sig;  /* of its first and last rows as Bink left them: anything else there means it was reused */
    LONG frame;    /* the frame Bink copied there last */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    LONG shown; /* the frame tex holds */
    ID3D11ShaderResourceView *gpu; /* or, decoded on the GPU, the ring texture holding it (the slot's) */
    uint64_t set;
} Dest;

typedef struct {
    uint32_t bink, alpha;
    Dest dest[DESTS];
    volatile LONG want; /* the frame Bink copied last (1-based): the decoder runs up to a few past it */
    wchar_t path[MAX_PATH];
    HANDLE thread, wake;
    volatile LONG stop, idle; /* idle: its decoder went (nothing copied for a while); a copy starts it again */
    volatile ULONGLONG copied; /* GetTickCount64 of Bink's last copy */
    SRWLOCK lock;
    uint32_t w, h;
    LONG stride; /* of the decoder's RGB32 rows: negative when they are stored bottom-up */
    uint32_t *frame[RING];
    ID3D11Texture2D *gpu[RING]; /* frames decoded on the GPU, in place of frame[] */
    ID3D11ShaderResourceView *gpu_srv[RING];
    LONG num[RING]; /* the frame each holds, 0 none */
    uint64_t used;
} Slot;
static Slot g_slot[SLOTS];
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static int g_enabled = 1;
static uint64_t g_clock;
static char g_dir[MAX_PATH]; /* ...\videos */
static int g_log; /* VIDEOLOG=1: the decoders' progress and the renderer's lookups */
/* decoding on the GPU: Media Foundation's software H.264 decoder ran a thread per core at 1920x1440 and the
 * cutscenes stuttered while it crowded out the game; frames then stay textures, never copied through the CPU */
static IMFDXGIDeviceManager *g_mgr;

void video_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int enabled) {
    g_dev = dev, g_ctx = ctx, g_enabled = enabled, g_log = getenv("VIDEOLOG") != NULL;
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    strcpy(slash ? slash + 1 : g_dir, "videos");
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    UINT token; /* the decoders use the device from their threads: it serialises its own calls */
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mt)))
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE), ID3D10Multithread_Release(mt);
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &g_mgr)) &&
        FAILED(IMFDXGIDeviceManager_ResetDevice(g_mgr, (IUnknown *)dev, token)))
        IMFDXGIDeviceManager_Release(g_mgr), g_mgr = NULL;
}

void video_enable(int on) { g_enabled = on; }

static void slot_close(Slot *s) {
    if (s->thread) {
        InterlockedExchange(&s->stop, 1);
        SetEvent(s->wake);
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread), CloseHandle(s->wake);
    }
    for (int i = 0; i < RING; i++) {
        free(s->frame[i]);
        if (s->gpu_srv[i]) ID3D11ShaderResourceView_Release(s->gpu_srv[i]);
        if (s->gpu[i]) ID3D11Texture2D_Release(s->gpu[i]);
    }
    for (int i = 0; i < DESTS; i++) {
        if (s->dest[i].srv) ID3D11ShaderResourceView_Release(s->dest[i].srv);
        if (s->dest[i].tex) ID3D11Texture2D_Release(s->dest[i].tex);
    }
    memset(s, 0, sizeof *s);
}

/* one decoded frame into the ring slot of the oldest (or a passed) frame; returns 0 at the end or on errors */
static int store(Slot *s, IMFSample *sample, LONG num) {
    IMFMediaBuffer *gbuf = NULL;
    IMFDXGIBuffer *dx = NULL;
    if (SUCCEEDED(IMFSample_GetBufferByIndex(sample, 0, &gbuf)) &&
        SUCCEEDED(IMFMediaBuffer_QueryInterface(gbuf, &IID_IMFDXGIBuffer, (void **)&dx))) {
        ID3D11Texture2D *src = NULL;
        UINT sub = 0;
        int ok = SUCCEEDED(IMFDXGIBuffer_GetResource(dx, &IID_ID3D11Texture2D, (void **)&src)) &&
                 SUCCEEDED(IMFDXGIBuffer_GetSubresourceIndex(dx, &sub));
        if (ok) {
            AcquireSRWLockExclusive(&s->lock);
            int k = 0;
            for (int i = 1; i < RING; i++)
                if (s->num[i] < s->num[k]) k = i; /* the oldest goes; the renderer shows the newest, so not this one */
            if (!s->gpu[k]) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc(src, &td); /* RGB32: B8G8R8X8, alpha reading as 1 like Bink's 32A frames */
                td.Width = s->w, td.Height = s->h, td.MipLevels = td.ArraySize = 1, td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.CPUAccessFlags = td.MiscFlags = 0;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &s->gpu[k])))
                    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s->gpu[k], NULL, &s->gpu_srv[k]);
            }
            if (s->gpu[k]) {
                D3D11_BOX box = {0, 0, 0, s->w, s->h, 1};
                ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)s->gpu[k], 0, 0, 0, 0,
                                                          (ID3D11Resource *)src, sub, &box);
                s->num[k] = num;
            }
            ReleaseSRWLockExclusive(&s->lock);
        }
        if (src) ID3D11Texture2D_Release(src);
        IMFDXGIBuffer_Release(dx), IMFMediaBuffer_Release(gbuf);
        return ok;
    }
    if (gbuf) IMFMediaBuffer_Release(gbuf);
    IMFMediaBuffer *buf = NULL;
    IMF2DBuffer *b2 = NULL;
    BYTE *scan0 = NULL, *base = NULL;
    LONG pitch = 0;
    DWORD len = 0;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) return 0;
    int ok = SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
             SUCCEEDED(IMF2DBuffer_Lock2D(b2, &scan0, &pitch));
    if (!ok && SUCCEEDED(IMFMediaBuffer_Lock(buf, &base, NULL, &len))) { /* a plain buffer: the media type's stride */
        LONG row = s->stride < 0 ? -s->stride : s->stride;
        ok = len >= (DWORD)row * s->h;
        pitch = s->stride, scan0 = s->stride < 0 ? base + (size_t)row * (s->h - 1) : base;
        if (!ok) IMFMediaBuffer_Unlock(buf), base = NULL;
    }
    if (ok) {
        AcquireSRWLockExclusive(&s->lock);
        int k = 0;
        for (int i = 1; i < RING; i++)
            if (s->num[i] < s->num[k]) k = i; /* the oldest goes */
        if (!s->frame[k]) s->frame[k] = malloc((size_t)s->w * s->h * 4);
        for (uint32_t y = 0; y < s->h && s->frame[k]; y++) {
            const uint32_t *src = (const uint32_t *)(scan0 + (LONG_PTR)y * pitch);
            uint32_t *dst = s->frame[k] + (size_t)y * s->w;
            for (uint32_t x = 0; x < s->w; x++) dst[x] = (src[x] & 0xFFFFFF) | s->alpha << 24;
        }
        s->num[k] = num;
        ReleaseSRWLockExclusive(&s->lock);
        if (base) IMFMediaBuffer_Unlock(buf);
        else IMF2DBuffer_Unlock2D(b2);
    }
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(buf);
    return ok;
}

static DWORD WINAPI decoder(void *arg) {
    Slot *s = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMFAttributes *a = NULL;
    IMFSourceReader *r = NULL;
    IMFMediaType *t = NULL, *cur = NULL;
    MFCreateAttributes(&a, 3);
    if (g_mgr) { /* decoded and converted to RGB on the GPU */
        IMFAttributes_SetUnknown(a, &MF_SOURCE_READER_D3D_MANAGER, (IUnknown *)g_mgr);
        IMFAttributes_SetUINT32(a, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        IMFAttributes_SetUINT32(a, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    } else IMFAttributes_SetUINT32(a, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    UINT64 size = 0, rate = 0;
    if (FAILED(MFCreateSourceReaderFromURL(s->path, a, &r)) || FAILED(MFCreateMediaType(&t)) ||
        FAILED(IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)) ||
        FAILED(IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32)) ||
        FAILED(IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, t)) ||
        FAILED(IMFSourceReader_GetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) ||
        FAILED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &size)) ||
        FAILED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_RATE, &rate))) {
        fprintf(stderr, "[video] cannot play %ls\n", s->path);
        goto done;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockExclusive(&s->lock);
    s->w = (uint32_t)(size >> 32), s->h = (uint32_t)size;
    UINT32 stride = 0; /* MF_MT_DEFAULT_STRIDE is a signed value; RGB32 without it is top-down rows */
    s->stride = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)s->w * 4;
    ReleaseSRWLockExclusive(&s->lock);
    double fps = (double)(rate >> 32) / (double)(uint32_t)rate;
    if (g_log) fprintf(stderr, "[video] decoding %ls: %ux%u at %.2f fps\n", s->path, s->w, s->h, fps);
    LONG next = 1; /* the frame the reader gives next */
    while (!s->stop) {
        /* a video the game no longer copies (left on a menu) lets its decoder go: Media Foundation's H.264 decoder
         * kept ~8 threads busy per open video, which took 3-4 cores from a race */
        if (GetTickCount64() - s->copied > 1000) {
            if (g_log) fprintf(stderr, "[video] %ls idle: decoder closed\n", s->path);
            InterlockedExchange(&s->idle, 1);
            break;
        }
        LONG want = s->want;
        /* jumped: seek to it. Back means further behind than the decoder ever runs ahead (it waits once AHEAD past):
         * with "RING / 2 behind" a decoder that had run ahead seeked back at once and decoded the same frames again,
         * so the frame asked for was often missing and cutscenes held for up to a third of a second */
        if (next != -1 && (want < next - AHEAD - 2 || want > next + 2 * RING)) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8, pos.hVal.QuadPart = (LONGLONG)((want - 1) / fps * 1e7);
            IMFSourceReader_SetCurrentPosition(r, &GUID_NULL, &pos);
            PropVariantClear(&pos);
            next = -1; /* known again from the next sample's time */
        }
        if (next != -1 && next > want + AHEAD) {
            WaitForSingleObject(s->wake, 100);
            continue;
        }
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &ts, &sample);
        if (g_log && (FAILED(hr) || !sample)) fprintf(stderr, "[video] read %08lX flags %lX sample %p\n", hr, flags, (void *)sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (sample) IMFSample_Release(sample);
            WaitForSingleObject(s->wake, 100); /* at the end: idle until asked for something else */
            if (s->want < next) next = 0x7FFFFFFF; /* force a seek */
            continue;
        }
        if (!sample) continue;
        LONG num = (LONG)(ts / 1e7 * fps + 0.5) + 1;
        if (g_log && num % 30 == 1) fprintf(stderr, "[video] %llu ms: decoded frame %ld (asked %ld)\n", GetTickCount64(), num, s->want);
        if (num >= s->want - 1) store(s, sample, num); /* frames before the one asked for are passed over */
        IMFSample_Release(sample);
        next = num + 1;
    }
done:
    if (cur) IMFMediaType_Release(cur);
    if (t) IMFMediaType_Release(t);
    if (r) IMFSourceReader_Release(r);
    if (a) IMFAttributes_Release(a);
    return 0;
}

/* hle_bink.c: BinkOpen(name) returned bink; name as the game gave it (D:\MEDIA\...\x.bik) */
static void video_open_(uint32_t bink, const char *name) {
    for (int i = 0; i < SLOTS; i++) /* the handle is reused for the next video: the old one's frames are gone */
        if (bink && g_slot[i].bink == bink) slot_close(&g_slot[i]);
    const char *media = strstr(name, "MEDIA\\");
    if (!media) media = strstr(name, "media\\");
    if (!g_dev || !media || !bink) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", g_dir, media + 6);
    char *dot = strrchr(path, '.');
    if (!dot) return;
    strcpy(dot, ".mp4");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    Slot *s = &g_slot[0];
    for (int i = 0; i < SLOTS; i++) /* a free slot, else the least recently used */
        if (!g_slot[i].bink || g_slot[i].used < s->used) s = &g_slot[i];
    slot_close(s);
    s->bink = bink, s->used = ++g_clock, s->want = 1, s->copied = GetTickCount64();
    InitializeSRWLock(&s->lock);
    MultiByteToWideChar(CP_ACP, 0, path, -1, s->path, MAX_PATH);
    s->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s->thread = CreateThread(NULL, 0, decoder, s, 0, NULL);
    fprintf(stderr, "[video] %s -> %s\n", name, path);
}
void video_open(uint32_t bink, const char *name) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    video_open_(bink, name);
    gpu_leave();
}

/* hle_bink.c: Bink copied frame `frame` to guest buffer `dest`; `alpha` is what it wrote there as alpha (BINK32A) */
/* the buffer's first and last 4 KiB */
static uint64_t signature(uint32_t addr, uint32_t span) {
    uint32_t n = span < 4096 ? span : 4096;
    return hash_bytes(g_mem + addr + span - n, n, hash_bytes(g_mem + addr, n, 0));
}

static void video_copied_(uint32_t bink, uint32_t frame, uint32_t dest, uint32_t span, uint32_t alpha) {
    if (g_log) { /* VIDEOLOG: when the game copied each frame, against when it came to the screen (video_for) */
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q), QueryPerformanceFrequency(&f);
        fprintf(stderr, "[video] copied frame %u at %.1f ms\n", frame, q.QuadPart * 1000.0 / f.QuadPart);
    }
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &g_slot[i];
        if (s->bink != bink) continue;
        Dest *d = &s->dest[0];
        for (int j = 0; j < DESTS; j++) /* this buffer's entry, else the one set longest ago */
            if (s->dest[j].addr == (dest & 0x0FFFFFFF) || (d->addr != (dest & 0x0FFFFFFF) && s->dest[j].set < d->set))
                d = &s->dest[j];
        d->addr = dest & 0x0FFFFFFF, d->frame = (LONG)frame, d->set = ++g_clock, d->span = span;
        d->sig = signature(d->addr, span);
        s->alpha = alpha, s->used = g_clock, s->copied = GetTickCount64();
        InterlockedExchange(&s->want, (LONG)frame);
        if (s->idle) { /* copying again: a new decoder, which seeks to the frame asked for */
            WaitForSingleObject(s->thread, INFINITE);
            CloseHandle(s->thread);
            s->idle = 0;
            s->thread = CreateThread(NULL, 0, decoder, s, 0, NULL);
        }
        SetEvent(s->wake);
    }
}
void video_copied(uint32_t bink, uint32_t frame, uint32_t dest, uint32_t span, uint32_t alpha) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    video_copied_(bink, frame, dest, span, alpha);
    gpu_leave();
}

/* whether the upscaled video already has frame `frame` of bink for buffer dest, which has had a frame of the game's
 * own before (its alpha and signature): then that frame shows upscaled and the game's conversion into the buffer can
 * be passed over (hle_bink.c) */
static int video_ready_(uint32_t bink, uint32_t frame, uint32_t dest) {
    if (!g_enabled) return 0;
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &g_slot[i];
        if (s->bink != bink || s->idle) continue;
        int seen = 0, ready = 0;
        for (int j = 0; j < DESTS; j++) seen |= s->dest[j].addr == (dest & 0x0FFFFFFF);
        AcquireSRWLockShared(&s->lock);
        for (int n = 0; n < RING && seen; n++) ready |= s->num[n] == (LONG)frame && (s->gpu_srv[n] || s->frame[n]);
        ReleaseSRWLockShared(&s->lock);
        return ready;
    }
    return 0;
}
int video_ready(uint32_t bink, uint32_t frame, uint32_t dest) { /* other threads' way in: with the GPU lock */
    gpu_enter();
    int r = video_ready_(bink, frame, dest);
    gpu_leave();
    return r;
}

/* the upscaled frame for guest buffer addr, or NULL: show the original */
ID3D11ShaderResourceView *video_for(uint32_t addr) {
    addr &= 0x0FFFFFFF;
    if (!g_enabled || !addr) return NULL;
    if (g_log) {
        static uint32_t seen[16];
        int k = 0;
        while (k < 16 && seen[k] && seen[k] != addr) k++;
        if (k < 16 && !seen[k]) {
            seen[k] = addr;
            fprintf(stderr, "[video] asked for %08X:", addr);
            for (int i = 0; i < SLOTS; i++)
                for (int j = 0; j < DESTS; j++)
                    if (g_slot[i].dest[j].addr) fprintf(stderr, " %08X", g_slot[i].dest[j].addr);
            fprintf(stderr, "\n");
        }
    }
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &g_slot[i];
        for (int j = 0; j < DESTS && s->bink; j++) {
            Dest *d = &s->dest[j];
            if (d->addr != addr) continue;
            if (signature(addr, d->span) != d->sig) { /* the video has moved on and the memory holds something else */
                d->addr = 0;
                return NULL;
            }
            AcquireSRWLockShared(&s->lock);
            int k = -1;
            for (int n = 0; n < RING; n++) /* the newest decoded frame not past the one Bink copied here */
                if (s->num[n] && s->num[n] <= d->frame && (k < 0 || s->num[n] > s->num[k])) k = n;
            long before = d->shown;
            static int misses;
            if (g_log && k < 0 && misses++ % 120 == 0)
                fprintf(stderr, "[video] %08X wants frame %ld (latest %ld); ring %ld %ld %ld %ld %ld %ld %ld %ld\n", addr, d->frame,
                        s->want, s->num[0], s->num[1], s->num[2], s->num[3], s->num[4], s->num[5], s->num[6], s->num[7]);
            if (k >= 0 && s->gpu_srv[k] && d->frame - s->num[k] < 15) { /* decoded on the GPU: that texture itself */
                if (g_log && !d->shown) fprintf(stderr, "[video] showing %ls in %08X from frame %ld (GPU)\n", s->path, addr, s->num[k]);
                d->shown = s->num[k];
                d->gpu = s->gpu_srv[k];
            } else if (k >= 0 && s->num[k] != d->shown && d->frame - s->num[k] < 15) {
                if (!d->tex) {
                    D3D11_TEXTURE2D_DESC td = {s->w, s->h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DYNAMIC,
                                               D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0};
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &d->tex)))
                        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)d->tex, NULL, &d->srv);
                }
                D3D11_MAPPED_SUBRESOURCE m;
                if (d->tex && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)d->tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    for (uint32_t y = 0; y < s->h; y++)
                        memcpy((uint8_t *)m.pData + y * m.RowPitch, s->frame[k] + (size_t)y * s->w, s->w * 4);
                    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)d->tex, 0);
                    if (g_log && !d->shown) fprintf(stderr, "[video] showing %ls in %08X from frame %ld\n", s->path, addr, s->num[k]);
                    d->shown = s->num[k];
                }
            }
            ReleaseSRWLockShared(&s->lock);
            if (g_log && d->shown != before) { /* VIDEOLOG: when each frame came to the screen, to see the pacing */
                LARGE_INTEGER q, f;
                QueryPerformanceCounter(&q), QueryPerformanceFrequency(&f);
                fprintf(stderr, "[video] %08X shows frame %ld at %.1f ms (Bink copied %ld)\n", addr, d->shown,
                        q.QuadPart * 1000.0 / f.QuadPart, d->frame);
            }
            return d->shown && d->frame - d->shown < 15 ? (d->gpu ? d->gpu : d->srv) : NULL;
        }
    }
    return NULL;
}
