/* Shared declarations between the loader (main.c) and the kernel HLE (kernel.c). */
#pragma once
#include "x86.h"

/* a per-thread variable: clang's mingw target (the zig build) ignores __declspec(thread) and made them globals that
 * every thread shared */
#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

/* Guest address map (all inside the 4 GiB g_mem reservation). */
#define GUEST_RAM_BASE 0x00010000u   /* first 64 KiB left unmapped to catch NULL derefs */
#define GUEST_RAM_END g_ram_end      /* 128 MiB like a devkit (retail has 64); 192 with VIDEOMODE 720 or 1080 (main.c) */
#define GUEST_HEAP_BASE 0x00800000u  /* bump heap for all game allocations, above the XBE image */
#define PHYS_ALIAS 0x80000000u       /* all of RAM again, as on the Xbox: VA 0x80000000 + physical */
#define WC_ALIAS 0xF0000000u         /* and again, the write-combined view D3D uses for texture memory */
#define KERNEL_BASE 0xB0000000u      /* kernel-owned structures: KPCR, threads, data exports */
#define KERNEL_SIZE 0x01000000u
#define KTHUNK_BASE 0xFFFF0000u      /* kernel function thunk slots hold KTHUNK_BASE + ordinal */
#define HLE_THUNK_BASE 0xFFFE0000u   /* host functions handed out as guest code addresses (hle_thunk) */
#define NV2A_BASE 0xFD000000u        /* GPU registers (16 MiB), plain memory managed by nv2a.c */
#define NV2A_SIZE 0x01000000u

typedef struct {
    uint32_t base, entry, kernel_thunk, tls_dir, cert;
    uint32_t image_size;
} XbeInfo;

extern XbeInfo g_xbe;
extern uint32_t g_ram_end;

/* kernel export implementation: a[] holds the arguments in declaration order */
typedef uint32_t (*KFn)(X86Ctx *c, const uint32_t *a);
typedef struct {
    const char *name;
    KFn fn;
} KImpl;
extern const KImpl g_thread_impls[]; /* thread.c, NULL-terminated */
extern const KImpl g_file_impls[];   /* file.c */
extern const KImpl g_crypto_impls[]; /* crypto.c */
void crypto_selftest(void);
void mmx_selftest(void);

uint32_t guest_alloc(uint32_t size, uint32_t align);
uint32_t guest_free(uint32_t va);       /* returns the freed block's size, 0 if va is not a block */
uint32_t guest_block_size(uint32_t va); /* 0 if va is not a block */
void guest_alloc_selftest(void);
uint32_t kernel_alloc(uint32_t size);
uint32_t guest_heap_top(void);
void guest_heap_stats(uint32_t *live, uint32_t *top, uint32_t *free_bytes, uint32_t *largest);
void kernel_init(void);
void game_load(void); /* runtime.c: the lifted game code (forza_recomp.dll), once g_mem is reserved */
void kernel_call(X86Ctx *c, uint32_t ordinal);
const char *guest_path_root(void);

uint32_t thread_env_init(X86Ctx *c, uint32_t stack_size, uint32_t tls_size);
void thread_run_main(X86Ctx *c, uint32_t entry);
uint32_t current_thread(X86Ctx *c);
void gil_acquire(void);
void gil_release(void);
void dpc_start(void);
void dpc_kick(void); /* thread.c: an interrupt was raised; run the DPC thread's pass now (any thread) */
uint32_t guest_call(X86Ctx *c, uint32_t target, int n, const uint32_t *args);
void queue_user_apc(X86Ctx *c, uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2); /* thread.c */
int file_close(uint32_t handle);
void nv2a_init(void);
void nv2a_tick(void);
uint32_t nv2a_pushbuffer_run(uint32_t put);
void nv2a_report(void);
void nv2a_vblank(void);
int nv2a_irq_pending(void);
void nv2a_pushbuffer_set_get(uint32_t get);
void nv2a_dma_object(uint32_t id, uint32_t cls, uint32_t base, uint32_t limit);
uint32_t nv2a_flip_count(void);
uint32_t nv2a_vblank_count(void);
int64_t nv2a_push_ticks(void); /* QPC time spent running the push buffer so far */
void watchdog_start(void);
void memwatch_poll(void); /* main.c: arms MEMWATCH=r<address> when build/watch.now appears */
int cfg_widescreen(void);   /* main.c: anamorphic 16:9, on unless WIDESCREEN=0 (or recomp.ini widescreen=0) */
int cfg_video_lines(void);  /* main.c: VIDEOMODE 480 (default), 720 or 1080: the HDTV modes offered to the game */
int cfg_saved(const char *key, int def);  /* main.c: recomp.ini [recomp] as saved, for the settings menu */
void cfg_save(const char *key, int value);
int cfg_get(const char *env, const char *key, int def); /* main.c: environment variable env, else as saved */
#define DEF_RENDERSCALE 2  /* recomp.ini defaults the renderer and the settings menu share */
#define DEF_ANISOTROPY 8
#define DEF_FSR 1
void render_fullscreen(int on);  /* render.c: borderless fullscreen on the window's monitor, or back */
void render_anisotropy(int n);   /* render.c: anisotropic filtering of linear-filtered textures, 1 = off */
void render_fsr(int on);         /* render.c: FSR 1 when the frame is smaller than the window's picture, else bilinear */
void render_motion(int on);      /* render.c: the scene writes motion vectors and jitters, for DLSS and FSR 3 */
float render_widen(void);        /* render.c: ULTRAWIDE, how much wider than 16:9 the picture is (1: not) */
__declspec(noreturn) void fatal_exit(int code); /* main.c: the game stops on an error and tells the player why */
void render_texpack(int on);     /* render.c: texture packs in textures\ next to the exe (texpack.c) */
void render_texdump(int on);     /* render.c: write each new texture to textures\dump, for making packs */
void video_open(uint32_t bink, const char *name);       /* video.c: Bink opened a video (upscaled cutscenes) */
void video_copied(uint32_t bink, uint32_t frame, uint32_t dest, uint32_t span, uint32_t alpha); /* and frame n to dest */
int video_ready(uint32_t bink, uint32_t frame, uint32_t dest); /* video.c: that frame shows upscaled already */
void video_enable(int on);
enum { DEV_KEYBOARD, DEV_XBOX, DEV_PLAYSTATION, DEV_OTHER }; /* what the player last pressed something on */
int input_last_device(void);                                    /* hle_xinput.c */
int input_icon_state(void); /* hle_xinput.c: the prompts' device (BUTTON PROMPTS or the last used), plus 16 x the
                             * times that or a key binding changed */
void input_set_prompts(int v); /* 0 auto, else DEV_* + 1 */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LT, BTN_RT, BTN_BLACK, BTN_WHITE, BTN_START, BTN_BACK, BTN_LEFT, BTN_DOWN,
       BTN_RIGHT, BTN_UP, BTN_COUNT }; /* the buttons a prompt can name */
void icons_key_name(int button, wchar_t *t, int n); /* hle_xinput.c: the menu key standing in for a button */
int input_nbindings(int menu);  /* hle_xinput.c: keyboard bindings, driving (menu = 0) or menu keys, for the settings menu */
void input_binding_text(int menu, int k, char *t, size_t n);
void input_capture(int menu, int k); /* the next key pressed becomes binding k */
void forza_init(void);              /* hle_forza.c: the game's own settings, before it runs (frame pacing) */
void forza_vblank(void);            /* hle_forza.c: each emulated vblank */
uint32_t nv2a_display(void);        /* nv2a.c: the frame on screen (PCRTC start) */
extern int g_vblank_hz;             /* hle_forza.c: the emulated vblank's rate (FRAME RATE above 60 raises it) */
const char *forza_screen(void);     /* hle_forza.c: the class of the screen on top, "" before the first */
int forza_menu_open(void);
/* achieve.c: achievements, from what the game does */
void achieve_init(void);
void achieve_poll(void);                       /* the current profile's state, about twice a second */
void achieve_screen(const char *cls);          /* the screen on top changed */
void achieve_car(uint32_t car, int human);     /* a car's drivers are about to run */
void achieve_file(const char *path, int writing); /* the game opens a file */
void achieve_readback(uint32_t width, uint32_t height);
int achieve_count(void), achieve_earned(void), achieve_has(int i), achieve_toast_next(void);
const char *achieve_name(int i), *achieve_desc(int i);           /* hle_forza.c: a menu is on screen (the pause menu too), not just the race */
int forza_race_view(void);          /* hle_forza.c: the race, its countdown or its replay is on top; any thread */
int forza_racing(void);              /* hle_forza.c: the race (or its countdown) is the screen on top; any thread */
void profile_run(int seconds); /* profile.c */
#define AUDIO_RATE 48000         /* the Xbox APU's rate, and the host output's */
void audio_start(void (*mix)(float *out, uint32_t frames, int channels)); /* audio.c: mix fills frames of 2 (L R)
                                                                             or 6 (FL FR C LFE BL BR) floats */
/* audiofx.c: ENHANCED AUDIO */
#define FX_TAPS 8 /* the resampler's taps, centred on the frame before the point read */
#define FX_CUTS 6
void fx_init(void);
void fx_enable(int on);
int fx_enabled(void);
int fx_cut(double step);
const float *fx_taps(int cut, double frac);
void fx_process(float *out, const float *send, uint32_t frames, int channels);
void render_init(void);
uint32_t hle_thunk(void (*fn)(X86Ctx *c));
void render_present(uint32_t addr);
void render_overlay(int enable, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, const int *dst,
                    int key_on, uint32_t key); /* key: ARGB colour key */
int ktrace(void);
extern THREAD_LOCAL X86Ctx *t_ctx;
