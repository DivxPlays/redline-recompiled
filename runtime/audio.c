/* Host audio output: 48 kHz float through WASAPI (shared mode, the default device), pulled from a mix callback on
 * its own thread: stereo, or with SPEAKERS on AUTO and a 5.1 or 7.1 device, the game's 5.1 (on 7.1 the back pair
 * plays from the sides and, softer, the back). Without a device the callback still runs on a 10 ms timer, so
 * whatever paces itself by playback (stream packets) keeps real time. */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
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

static void pull(float *buf, uint32_t frames) {
    g_mix(buf, frames, g_mixed);
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
