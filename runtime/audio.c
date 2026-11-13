/* Host audio output: 48 kHz float through WASAPI (shared mode, the default device), pulled from a mix callback on
 * its own thread: stereo, or with SPEAKERS on AUTO and a 5.1 or 7.1 device, the game's 5.1 (on 7.1 the back pair
 * plays from the sides and, softer, the back). Without a device the callback still runs on a 10 ms timer, so
 * whatever paces itself by playback (stream packets) keeps real time. */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox.h"

#pragma comment(lib, "ole32.lib")

static const GUID g_clsid_enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID g_iid_enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID g_iid_client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID g_iid_render = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

static void (*g_mix)(float *out, uint32_t frames, int channels);
static int g_mixed = 2, g_device = 2; /* channels mixed (2 or 6) and sent to the device (2, 6 or 8) */

/* creating build/wav.now records the next 20 s of output to build/audio.wav (16-bit stereo; surround folded down) */
static void record(const float *buf, uint32_t frames) {
    static int16_t *rec;
    static uint32_t have, want;
    static ULONGLONG next_check;
    if (!want && GetTickCount64() >= next_check) {
        next_check = GetTickCount64() + 1000;
        if (DeleteFileA("build/wav.now") && (rec = realloc(rec, 20 * AUDIO_RATE * 4))) want = 20 * AUDIO_RATE, have = 0;
    }
    for (uint32_t i = 0; i < frames && have < want; i++, have++) {
        const float *o = buf + (size_t)i * g_mixed;
        float l = o[0], r = o[1];
        if (g_mixed == 6) l += 0.707f * (o[2] + o[4]), r += 0.707f * (o[2] + o[5]);
        rec[2 * have] = (int16_t)(32767 * (l < -1 ? -1 : l > 1 ? 1 : l));
        rec[2 * have + 1] = (int16_t)(32767 * (r < -1 ? -1 : r > 1 ? 1 : r));
    }
    if (!want || have < want) return;
    uint32_t bytes = want * 4, hdr[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16, 0x00020001, AUDIO_RATE,
                                         AUDIO_RATE * 4, 0x00100004, 0x61746164, bytes}; /* RIFF/WAVE fmt PCM, data */
    FILE *f = fopen("build/audio.wav", "wb");
    if (f) fwrite(hdr, 4, 11, f), fwrite(rec, 4, want, f), fclose(f);
    fprintf(stderr, "[audio] saved build/audio.wav (%u s)\n", want / AUDIO_RATE);
    want = 0;
}

/* the achievement sound (render.c's pop-up), as the owner chose it: the first 3.5 s of the game's own fanfare
 * (FanFare in media\Audio\UISounds.xwb, Xbox ADPCM, 44.1 kHz stereo), read from the player's disc the first time,
 * faded in over 10 ms and out over its last 0.8 s, at -6 dBFS peak (x0.55), resampled to 48 kHz and added to the
 * front pair. Without the bank, silence */
static float *g_chime;
static uint32_t g_chime_n;
static volatile LONG g_chime_pos = -1;
static void chime_make(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\media\\Audio\\UISounds.xwb", guest_path_root());
    FILE *f = fopen(path, "rb");
    long size = f && !fseek(f, 0, SEEK_END) ? ftell(f) : 0;
    uint8_t *d = size > 48 ? malloc(size) : NULL;
    if (f) fseek(f, 0, SEEK_SET);
    if (!d || fread(d, 1, size, f) != (size_t)size || memcmp(d, "WBND", 4)) size = 0;
    if (f) fclose(f);
#define U32(o) ((uint64_t)(o) + 4 <= (uint64_t)size ? *(const uint32_t *)(d + (o)) : 0)
    /* an XACT wave bank: regions {offset, length} for the bank, the entries {flags, format, offset, length, ...}, their
     * names and the wave data; the format's tag (1: Xbox ADPCM), channels and rate in bits 0-1, 2-4, 5-22 */
    uint32_t bank = U32(8), meta = U32(16), names = U32(24), names_len = U32(28), wave = U32(32);
    uint32_t count = U32(bank + 4), meta_size = U32(bank + 24), name_size = U32(bank + 28);
    for (uint32_t i = 0; size && name_size && (uint64_t)names + names_len <= (uint64_t)size && i < count &&
                         (uint64_t)(i + 1) * name_size <= names_len; i++) {
        if (strncmp((const char *)d + names + i * name_size, "FanFare", name_size)) continue;
        uint32_t m = meta + i * meta_size, fmt = U32(m + 4), off = U32(m + 8), len = U32(m + 12);
        uint32_t ch = (fmt >> 2) & 7, rate = (fmt >> 5) & 0x3FFFF, blocks = len / (36 * 2), want = rate * 7 / 2;
        if ((fmt & 3) != 1 || ch != 2 || rate < 8000 || (uint64_t)wave + off + len > (uint64_t)size || blocks * 65 <= want) break;
        int16_t *pcm = malloc((size_t)blocks * 65 * 2 * sizeof(int16_t));
        g_chime_n = (uint32_t)((uint64_t)want * AUDIO_RATE / rate);
        float *out = pcm ? malloc((size_t)g_chime_n * 2 * sizeof(float)) : NULL;
        for (uint32_t b = 0; out && b < blocks; b++) adpcm_block(d + wave + off + b * 72, 2, pcm + b * 65 * 2);
        for (uint32_t k = 0; out && k < g_chime_n; k++) {
            double at = (double)k * rate / AUDIO_RATE;
            uint32_t s = (uint32_t)at;
            float fr = (float)(at - s), t = (float)k / AUDIO_RATE, left = (float)(g_chime_n - k) / AUDIO_RATE, x;
            float g = 0.55f / 32768 * (t < 0.01f ? t / 0.01f : 1) * (left < 0.8f ? (x = cosf((0.8f - left) / 0.8f * 1.5707963f)) * x : 1);
            for (int c = 0; c < 2; c++) out[2 * k + c] = g * ((1 - fr) * pcm[2 * s + c] + fr * pcm[2 * (s + 1) + c]);
        }
        free(pcm);
        if (out) g_chime = out, fprintf(stderr, "[audio] achievement sound: FanFare from UISounds.xwb, %.1f s\n", 3.5);
        break;
    }
#undef U32
    if (!g_chime) fprintf(stderr, "[audio] achievement sound: FanFare not found in %s; the pop-up is silent\n", path);
    free(d);
}
void audio_chime(void) {
    static int made;
    if (!cfg_saved("achsound", 1)) return; /* ACHIEVEMENT SOUND off */
    if (!made++) chime_make(); /* the first time: the game's files are known by then */
    if (g_chime) InterlockedExchange(&g_chime_pos, 0);
}

static void pull(float *buf, uint32_t frames) {
    g_mix(buf, frames, g_mixed);
    LONG at = g_chime_pos, pos = at;
    for (uint32_t i = 0; g_chime && pos >= 0 && i < frames && (uint32_t)pos < g_chime_n; i++, pos++)
        buf[(size_t)i * g_mixed] += g_chime[2 * pos], buf[(size_t)i * g_mixed + 1] += g_chime[2 * pos + 1];
    if (at >= 0) InterlockedCompareExchange(&g_chime_pos, (uint32_t)pos >= g_chime_n ? -1 : pos, at); /* unless restarted */
    record(buf, frames);
    if (g_device == 8) /* 7.1 from 5.1, in place from the end: the back pair to the sides, and softer to the back */
        for (uint32_t i = frames; i-- > 0;) {
            const float *o = buf + (size_t)i * 6;
            float f[8] = {o[0], o[1], o[2], o[3], o[4] * 0.6f, o[5] * 0.6f, o[4], o[5]};
            memcpy(buf + (size_t)i * 8, f, sizeof f);
        }
}

/* no device: mix into scratch at the rate wall time advances */
static void run_timer(void) {
    static float scratch[8 * 4800];
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (uint64_t done = 0;;) {
        Sleep(10);
        QueryPerformanceCounter(&t);
        uint64_t due = (uint64_t)(t.QuadPart - t0.QuadPart) * AUDIO_RATE / f.QuadPart;
        while (done < due) {
            uint32_t n = due - done > 4800 ? 4800 : (uint32_t)(due - done);
            pull(scratch, n);
            done += n;
        }
    }
}

static DWORD WINAPI audio_thread(void *p) {
    thread_name("audio");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT32 size = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(&g_clsid_enumerator, NULL, CLSCTX_ALL, &g_iid_enumerator, (void **)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &g_iid_client, CLSCTX_ALL, NULL, (void **)&ac);
    WAVEFORMATEX *mixf = NULL; /* the device's own channel count decides surround; SPEAKERS=6 forces 5.1 (testing:
                                * Windows folds it down for a stereo device) */
    int speakers = cfg_get("SPEAKERS", "speakers", 0);
    if (SUCCEEDED(hr) && SUCCEEDED(IAudioClient_GetMixFormat(ac, &mixf)) && mixf) {
        if ((speakers == 0 && mixf->nChannels >= 6) || speakers == 6) g_mixed = 6, g_device = mixf->nChannels >= 8 && speakers == 0 ? 8 : 6;
        CoTaskMemFree(mixf);
    }
    static const GUID float_sub = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    WAVEFORMATEXTENSIBLE wf = {{WAVE_FORMAT_EXTENSIBLE, (WORD)g_device, AUDIO_RATE, AUDIO_RATE * 4 * g_device,
                                (WORD)(4 * g_device), 32, 22}};
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = g_device == 8 ? 0x63F : g_device == 6 ? 0x3F : 0x3; /* 7.1, 5.1 (back), stereo */
    wf.SubFormat = float_sub;
    /* 40 ms of buffering; the engine converts our 48 kHz float to the device's mix format */
    if (SUCCEEDED(hr))
        hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     400000, 0, (WAVEFORMATEX *)&wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(ac, ev);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(ac, &size);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(ac, &g_iid_render, (void **)&rc);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(ac);
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] no output device (%08lX): mixing silently\n", (unsigned long)hr);
        g_mixed = g_device = 2;
        run_timer();
    }
    fprintf(stderr, "[audio] WASAPI %u Hz, %d channels, %u-frame buffer\n", AUDIO_RATE, g_device, size);
    for (;;) {
        WaitForSingleObject(ev, 100);
        UINT32 pad;
        BYTE *buf;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad)) || pad >= size) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, size - pad, &buf))) continue;
        pull((float *)buf, size - pad);
        IAudioRenderClient_ReleaseBuffer(rc, size - pad, 0);
    }
}

void audio_start(void (*mix)(float *out, uint32_t frames, int channels)) {
    g_mix = mix;
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
}
