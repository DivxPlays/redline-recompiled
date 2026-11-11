/* The pieces of Visual Studio's runtime that NVIDIA's NGX static library (nvsdk_ngx_s.lib, DLSS) uses, for the
 * build with zig (clang, mingw-w64 and the Universal CRT), which has none of them: operator new / delete and two STL
 * error paths under their MSVC names, the /GS stack cookie, thread-safe static initialisation, and _fltused.
 * Visual Studio's own build has them all, so this file is empty there. */
#ifndef _MSC_VER
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

void *msvc_new(unsigned long long n) __asm__("??2@YAPEAX_K@Z");
void msvc_delete(void *p) __asm__("??3@YAXPEAX@Z");
void msvc_xlength_error(const char *what) __asm__("?_Xlength_error@std@@YAXPEBD@Z");
void msvc_xbad_alloc(void) __asm__("?_Xbad_alloc@std@@YAXXZ");

void *msvc_new(unsigned long long n) {
    void *p = malloc(n ? n : 1);
    if (!p) msvc_xbad_alloc();
    return p;
}
void msvc_delete(void *p) { free(p); }
void msvc_xlength_error(const char *what) { (void)what; abort(); } /* the library would have thrown: nothing catches */
void msvc_xbad_alloc(void) { abort(); }

int _fltused = 0x9875;

/* /GS: a cookie the library's functions check on return; a mismatch is a stack overrun */
uintptr_t __security_cookie = 0x00002B992DDFA232ull;
void __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie) abort();
}
/* the unwind handlers /GS functions name: only exceptions unwinding through the library reach them, and it throws
 * none we would catch, so they pass them on */
EXCEPTION_DISPOSITION __GSHandlerCheck(EXCEPTION_RECORD *r, void *frame, CONTEXT *c, void *dispatch) {
    (void)r, (void)frame, (void)c, (void)dispatch;
    return ExceptionContinueSearch;
}
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(EXCEPTION_RECORD *r, void *frame, CONTEXT *c, void *dispatch) {
    (void)r, (void)frame, (void)c, (void)dispatch;
    return ExceptionContinueSearch;
}

/* thread-safe function-local statics, MSVC's protocol (its thread_safe_statics.cpp): a guard is 0 (not yet), -1
 * (being initialised) or the global epoch at which it was; each thread caches the epoch it has seen */
static SRWLOCK g_init_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_init_done = CONDITION_VARIABLE_INIT;
static int g_init_epoch = INT_MIN;
__thread int _Init_thread_epoch = INT_MIN;

void _Init_thread_header(int *guard) {
    AcquireSRWLockExclusive(&g_init_lock);
    if (*guard == 0) {
        *guard = -1;
    } else {
        while (*guard == -1) SleepConditionVariableSRW(&g_init_done, &g_init_lock, INFINITE, 0);
        if (*guard == 0) *guard = -1; /* the thread that started it gave up (an exception): this one does it */
        else _Init_thread_epoch = g_init_epoch;
    }
    ReleaseSRWLockExclusive(&g_init_lock);
}
void _Init_thread_footer(int *guard) {
    AcquireSRWLockExclusive(&g_init_lock);
    *guard = ++g_init_epoch;
    _Init_thread_epoch = g_init_epoch;
    ReleaseSRWLockExclusive(&g_init_lock);
    WakeAllConditionVariable(&g_init_done);
}
#endif
