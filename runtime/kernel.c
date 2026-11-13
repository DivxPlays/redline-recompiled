/* xboxkrnl HLE: thunk patching, calling-convention dispatch, and export implementations.
 * Implementations are added as the boot reaches them; anything missing stops with its name. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel_exports.h"
#include "version.h"
#include "xbox.h"

#define STATUS_SUCCESS 0x00000000u

static KFn g_kfns[KERNEL_EXPORT_MAX];
static uint32_t g_kdata[KERNEL_EXPORT_MAX]; /* guest VA of each data export */
static int g_trace;
int ktrace(void) { return g_trace; }

/* ---------------- helpers ---------------- */

static uint32_t kdata(const char *name) {
    for (int i = 0; i < KERNEL_EXPORT_MAX; i++)
        if (g_kexports[i].name && !strcmp(g_kexports[i].name, name)) return g_kdata[i];
    return 0;
}

static void tick(void) { /* KeTickCount is read straight from memory; refresh it whenever the guest enters the kernel */
    static uint32_t va;
    if (!va) va = kdata("KeTickCount");
    M32(va) = GetTickCount();
}

/* ANSI_STRING / UNICODE_STRING: { u16 Length, u16 MaximumLength, u32 Buffer } */
static void ansi_init(uint32_t str, uint32_t buf, uint16_t len, uint16_t max) {
    M16(str) = len; M16(str + 2) = max; M32(str + 4) = buf;
}

/* ---------------- Av / Hal / Dbg ---------------- */

static uint32_t k_AvGetSavedDataAddress(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_AvSetSavedDataAddress(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_AvSetDisplayMode(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_AvSendTVEncoderOption(X86Ctx *c, const uint32_t *a) {
    if (a[3]) M32(a[3]) = 0; /* Result */
    return 0;
}
static uint32_t k_DbgPrint(X86Ctx *c, const uint32_t *a) {
    fprintf(stderr, "[DbgPrint] %s", (const char *)(g_mem + a[0]));
    return STATUS_SUCCESS;
}
static uint32_t k_HalReadWritePCISpace(X86Ctx *c, const uint32_t *a) {
    /* (Bus, Slot, Register, Buffer, Length, WritePCISpace) */
    if (!a[5]) memset(g_mem + a[3], 0, a[4]);
    return 0;
}
static uint32_t k_HalRegisterShutdownNotification(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_HalIsResetOrShutdownPending(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_HalReturnToFirmware(X86Ctx *c, const uint32_t *a) {
    fprintf(stderr, "HalReturnToFirmware(%u): title asked to reboot/exit\n", a[0]);
    /* the game leaves for the Xbox dashboard (Multiplayer > Xbox Live > New Account): there is none, so say so
     * rather than vanish */
    if (!getenv("TESTKEYS"))
        MessageBoxA(NULL, "The game asked to go to the Xbox Dashboard (to make an Xbox Live account).\n\n"
                          "There is no dashboard here, and Xbox Live no longer exists, so " RECOMP_NAME " closes now.",
                    RECOMP_NAME, MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    exit(0);
}
static uint32_t k_HalInitiateShutdown(X86Ctx *c, const uint32_t *a) {
    fprintf(stderr, "HalInitiateShutdown\n");
    exit(0);
}
static uint32_t k_KeBugCheck(X86Ctx *c, const uint32_t *a) {
    fprintf(stderr, "KeBugCheck(%08X)\n", a[0]);
    exit(2);
}

/* ---------------- IRQL ---------------- */

static uint32_t set_irql(X86Ctx *c, uint8_t irql) {
    uint8_t old = M8(c->fs_base + 0x24);
    M8(c->fs_base + 0x24) = irql;
    return old;
}
static uint32_t k_KfRaiseIrql(X86Ctx *c, const uint32_t *a) { return set_irql(c, (uint8_t)a[0]); }
static uint32_t k_KfLowerIrql(X86Ctx *c, const uint32_t *a) { set_irql(c, (uint8_t)a[0]); return 0; }
static uint32_t k_KeRaiseIrqlToDpcLevel(X86Ctx *c, const uint32_t *a) { return set_irql(c, 2); }

/* ---------------- memory ---------------- */

/* regions handed out by NtAllocateVirtualMemory, in address order, so NtQueryVirtualMemory can report each one's
 * real extent: XAPI's heap sizes a segment from that answer, and a region "running to the heap top" let it spill
 * over later allocations */
#define MAX_VM_REGIONS 4096
static struct { uint32_t base, size; } g_vm[MAX_VM_REGIONS];
static int g_nvm;

static uint32_t k_NtAllocateVirtualMemory(X86Ctx *c, const uint32_t *a) {
    /* (*BaseAddress, ZeroBits, *RegionSize, AllocationType, Protect) */
    uint32_t want = M32(a[0]), size = (M32(a[2]) + 0xFFF) & ~0xFFFu;
    if (want) { /* committing inside a region we already handed out: memory is always committed */
        M32(a[2]) = size;
        return STATUS_SUCCESS;
    }
    /* 64 KiB-aligned as on the Xbox, where that costs only address space; here each 4 KiB VirtualAlloc leaves a 60 KiB
     * hole. Page-aligned cut a 720p race load's heap top by 24 MB but crashed in 0027D540 (NOTES, GitHub #17) */
    uint32_t base = guest_alloc(size, 0x10000);
    M32(a[0]) = base;
    M32(a[2]) = size;
    if (g_nvm == MAX_VM_REGIONS) {
        fprintf(stderr, "too many virtual memory regions\n");
        fatal_exit(1);
    }
    int i = g_nvm++;
    for (; i > 0 && g_vm[i - 1].base > base; i--) g_vm[i] = g_vm[i - 1]; /* freed space gets reused: keep the order */
    g_vm[i].base = base, g_vm[i].size = size;
    return STATUS_SUCCESS;
}
static uint32_t k_NtFreeVirtualMemory(X86Ctx *c, const uint32_t *a) {
    /* (*BaseAddress, *RegionSize, FreeType): a released region goes back to guest_alloc; decommitted pages stay
     * mapped but are zeroed, since a page committed again must read as zeros: XAPI's heap decommits and recommits,
     * and stale headers left in a recommitted page corrupted its free lists */
    uint32_t base = M32(a[0]) & ~0xFFFu, size = (M32(a[1]) + (M32(a[0]) & 0xFFF) + 0xFFF) & ~0xFFFu;
    int region = -1;
    for (int i = 0; i < g_nvm; i++)
        if (g_vm[i].base == base) region = i;
    if ((a[2] & 0x8000) && region >= 0) { /* MEM_RELEASE: the whole region */
        size = g_vm[region].size;
        memmove(&g_vm[region], &g_vm[region + 1], (--g_nvm - region) * sizeof g_vm[0]);
        guest_free(base);
    } else if (base >= GUEST_HEAP_BASE && base + size <= GUEST_RAM_END && base + size > base) {
        memset(g_mem + base, 0, size);
    }
    M32(a[0]) = base;
    M32(a[1]) = size;
    return STATUS_SUCCESS;
}
/* Contiguous memory lives at PHYS_ALIAS + physical, as on the Xbox: games range-check (Data | 0x80000000) against
 * pools carved from it (Forza's texture pool never got its blocks back while it sat at its low address) */
static uint32_t k_MmAllocateContiguousMemory(X86Ctx *c, const uint32_t *a) {
    return PHYS_ALIAS + guest_alloc(a[0], 4096);
}
static uint32_t k_MmAllocateContiguousMemoryEx(X86Ctx *c, const uint32_t *a) {
    /* (NumberOfBytes, LowestAcceptableAddress, HighestAcceptableAddress, Alignment, ProtectionType) */
    return PHYS_ALIAS + guest_alloc(a[0], a[3] > 4096 ? a[3] : 4096);
}
static uint32_t k_MmAllocateSystemMemory(X86Ctx *c, const uint32_t *a) { return guest_alloc(a[0], 4096); }
static uint32_t k_MmFreeContiguousMemory(X86Ctx *c, const uint32_t *a) { guest_free(a[0]); return 0; }
static uint32_t k_MmFreeSystemMemory(X86Ctx *c, const uint32_t *a) { guest_free(a[0]); return a[1]; }
/* (NumberOfBytes, DebuggerThread) -> the stack's top (it grows down); (StackBase, StackLimit) */
static uint32_t k_MmCreateKernelStack(X86Ctx *c, const uint32_t *a) { return guest_alloc(a[0], 4096) + a[0]; }
static uint32_t k_MmDeleteKernelStack(X86Ctx *c, const uint32_t *a) { guest_free(a[1]); return 0; }
static uint32_t k_MmPersistContiguousMemory(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_MmGetPhysicalAddress(X86Ctx *c, const uint32_t *a) {
    /* low guest addresses are their own physical address; strip the RAM aliases */
    if (a[0] - PHYS_ALIAS < GUEST_RAM_END) return a[0] - PHYS_ALIAS;
    if (a[0] - WC_ALIAS < GUEST_RAM_END) return a[0] - WC_ALIAS;
    return a[0];
}
static uint32_t k_MmSetAddressProtect(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_MmQueryAddressProtect(X86Ctx *c, const uint32_t *a) { return PAGE_READWRITE; }
static uint32_t k_MmClaimGpuInstanceMemory(X86Ctx *c, const uint32_t *a) {
    /* (NumberOfBytes, *NumberOfPaddingBytes) -> end of the claimed region */
    if (a[1]) M32(a[1]) = 0;
    uint32_t size = a[0] == 0xFFFFFFFFu ? 0x5000 : a[0];
    return guest_alloc(size, 4096) + size;
}
static uint32_t k_MmQueryStatistics(X86Ctx *c, const uint32_t *a) {
    /* MM_STATISTICS: Length, TotalPhysicalPages, AvailablePages, VirtualMemoryBytesCommitted, ... */
    uint32_t s = a[0];
    if (M32(s) != 0x24) return 0xC000000Du; /* STATUS_INVALID_PARAMETER */
    memset(g_mem + s + 4, 0, 0x20);
    M32(s + 4) = (GUEST_RAM_END - GUEST_RAM_BASE) / 4096;
    M32(s + 8) = (GUEST_RAM_END - GUEST_HEAP_BASE) / 4096 / 2;
    return STATUS_SUCCESS;
}
static uint32_t k_NtQueryVirtualMemory(X86Ctx *c, const uint32_t *a) {
    /* (BaseAddress, MEMORY_BASIC_INFORMATION*): BaseAddress, AllocationBase, AllocationProtect, RegionSize,
     * State, Protect, Type. Regions: [0, image) free, [image, heap top) committed (each VM region on its own, the
     * space between them as one region each), [heap top, 2 GiB) free. */
    uint32_t va = a[0] & ~0xFFFu, out = a[1];
    if (va >= PHYS_ALIAS) return 0xC000000Du; /* STATUS_INVALID_PARAMETER: end of user space, ends the walk */
    uint32_t top = (guest_heap_top() + 0xFFF) & ~0xFFFu, start, end, committed;
    if (va < g_xbe.base) start = 0, end = g_xbe.base, committed = 0;
    else if (va < top) { /* inside a VM region, or among the other allocations between two of them */
        start = g_xbe.base, end = top, committed = 1;
        for (int i = 0; i < g_nvm; i++) {
            uint32_t b = g_vm[i].base, e = b + g_vm[i].size;
            if (va >= b && va < e) {
                start = b, end = e;
                break;
            }
            if (e <= va && e > start) start = e;
            if (b > va && b < end) end = b;
        }
    } else start = top, end = PHYS_ALIAS, committed = 0;
    M32(out + 0) = va;
    M32(out + 4) = committed ? start : 0;
    M32(out + 8) = committed ? PAGE_READWRITE : 0;
    M32(out + 12) = end - va;
    M32(out + 16) = committed ? MEM_COMMIT : MEM_FREE;
    M32(out + 20) = committed ? PAGE_READWRITE : PAGE_NOACCESS;
    M32(out + 24) = committed ? MEM_PRIVATE : 0;
    return STATUS_SUCCESS;
}
static uint32_t k_ExAllocatePoolWithTag(X86Ctx *c, const uint32_t *a) { return guest_alloc(a[0], 16); }
static uint32_t k_ExFreePool(X86Ctx *c, const uint32_t *a) { guest_free(a[0]); return 0; }

/* ---------------- time ---------------- */

static uint32_t k_KeQueryPerformanceCounter(X86Ctx *c, const uint32_t *a) {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    c->edx = (uint32_t)(v.QuadPart >> 32);
    return (uint32_t)v.QuadPart;
}
static uint32_t k_KeQueryPerformanceFrequency(X86Ctx *c, const uint32_t *a) {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    c->edx = (uint32_t)(v.QuadPart >> 32);
    return (uint32_t)v.QuadPart;
}
static uint32_t k_KeQuerySystemTime(X86Ctx *c, const uint32_t *a) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    M32(a[0]) = ft.dwLowDateTime;
    M32(a[0] + 4) = ft.dwHighDateTime;
    return 0;
}
static uint32_t k_KeStallExecutionProcessor(X86Ctx *c, const uint32_t *a) { return 0; }

/* ---------------- settings ---------------- */

/* a dword setting. The time zone is the PC's, so XAPI's local times (the date a save shows) match its clock:
 * settings 0-6 are the bias, the standard and daylight names (4 chars), dates {month, day (week of the month), day
 * of week, hour} and biases, in minutes like Win32's */
static uint32_t setting(uint32_t index) {
    static TIME_ZONE_INFORMATION tz;
    static int have_tz;
    if (!have_tz) GetTimeZoneInformation(&tz), have_tz = 1;
    uint32_t v = 0;
    switch (index) {
    case 0x00: return (uint32_t)tz.Bias;
    case 0x01: case 0x04: { /* "PST", "PDT": the capitals of the Windows names, as the Xbox dashboard's short names */
        const WCHAR *n = index == 1 ? tz.StandardName : tz.DaylightName;
        for (int i = 0, k = 0; n[i] && k < 4; i++)
            if (n[i] >= 'A' && n[i] <= 'Z') v |= (uint32_t)n[i] << (8 * k++);
        return v;
    }
    case 0x02: case 0x05: {
        const SYSTEMTIME *d = index == 2 ? &tz.StandardDate : &tz.DaylightDate;
        return (uint32_t)d->wMonth | (uint32_t)d->wDay << 8 | (uint32_t)d->wDayOfWeek << 16 | (uint32_t)d->wHour << 24;
    }
    case 0x03: return (uint32_t)tz.StandardBias;
    case 0x06: return (uint32_t)tz.DaylightBias;
    case 0x07: { /* language (XC_LANGUAGE): the game has 1 English .. 8 Chinese (media\Loc); until one is chosen,
                  * Windows' display language's (saved, so Recomp Settings shows it; the same in the launcher).
                  * GAMELANG for scripts, as LANGUAGE is Wine's and Linux's locale list */
        if (!cfg_saved("language", 0)) {
            int w = 1;
            switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
            case LANG_JAPANESE: w = 2; break;
            case LANG_GERMAN: w = 3; break;
            case LANG_FRENCH: w = 4; break;
            case LANG_SPANISH: w = 5; break;
            case LANG_ITALIAN: w = 6; break;
            case LANG_KOREAN: w = 7; break;
            case LANG_CHINESE: w = 8; break;
            }
            cfg_save("language", w);
        }
        int l = cfg_get("GAMELANG", "language", 1);
        return l >= 1 && l <= 8 ? (uint32_t)l : 1;
    }
    case 0x08: /* video flags: PAL-60 (0x400000, unused in NTSC), widescreen (0x10000) unless WIDESCREEN=0, and with
                * VIDEOMODE=720 / 1080 the HDTV modes 480p (0x80000), 720p (0x20000), 1080i (0x40000). XGetVideoFlags
                * keeps those only on the HDTV AV pack (HalBootSMCVideoMode 1); Forza then picks 1280x720 / 1920x1080 */
        v = 0x00400000 | (cfg_widescreen() ? 0x00010000 : 0);
        if (cfg_video_lines() >= 720) v |= 0x00080000 | 0x00020000;
        if (cfg_video_lines() >= 1080) v |= 0x00040000;
        return v;
    case 0x103: return 0x00400100; /* factory AV region: NTSC-M */
    case 0x104: return 1;          /* factory game region: North America */
    default: {
        static uint8_t told[0x200];
        if (index < 0x200 && !told[index]++) fprintf(stderr, "[k] setting %X reads as 0\n", index);
        return 0;
    }
    }
}

static uint32_t k_ExQueryNonVolatileSetting(X86Ctx *c, const uint32_t *a) {
    /* (ValueIndex, *Type, Value, ValueLength, *ResultLength) */
    uint32_t user[24] = {0}, len = 4, type = 4 /* REG_DWORD */;
    const void *src = NULL;
    if (a[0] == 0xFF) { /* XC_MAX_OS: the EEPROM's user section, where XAPI reads the time zone: a checksum, then the
                         * settings above at their places (dwords 1-3, 6-7, 10-13), audio, parental, network, misc, DVD
                         * region (1); the checksum is the NOT of the carried sum of the rest */
        static const uint8_t at[] = {1, 2, 6, 10, 3, 7, 11, 12, 13};
        for (int i = 0; i < 9; i++) user[at[i]] = setting(i);
        user[23] = 1;
        uint64_t sum = 0;
        for (int i = 1; i < 24; i++) sum += user[i];
        user[0] = ~(uint32_t)((sum & 0xFFFFFFFF) + (sum >> 32));
        src = user, len = sizeof user, type = 3 /* REG_BINARY */;
        static int told;
        if (!told++) fprintf(stderr, "[k] user settings read into %u bytes\n", a[3]);
    } else if (a[0] == 0xFFFF) {
        len = 0x100, type = 3;
    }
    uint32_t v = a[0] == 0xFF || a[0] == 0xFFFF ? 0 : setting(a[0]);
    /* the whole buffer is always written: XAPI reads a 256-byte EEPROM image (0xFFFF) into a stack buffer and, when
     * XboxHDKey is all zeros, keys save signatures off it, so leftover stack bytes made every save "damaged" */
    if (a[1]) M32(a[1]) = type;
    if (a[2] && a[3]) memset(g_mem + a[2], 0, a[3]);
    if (a[2] && src) memcpy(g_mem + a[2], src, a[3] < len ? a[3] : len);
    else if (a[2] && a[3] >= 4 && len == 4) M32(a[2]) = v;
    if (a[4]) M32(a[4]) = len;
    return STATUS_SUCCESS;
}

/* ---------------- Rtl ---------------- */

static uint32_t k_RtlInitAnsiString(X86Ctx *c, const uint32_t *a) {
    uint16_t len = a[1] ? (uint16_t)strlen((const char *)(g_mem + a[1])) : 0;
    ansi_init(a[0], a[1], len, a[1] ? len + 1 : 0);
    return 0;
}
static uint32_t k_RtlEqualString(X86Ctx *c, const uint32_t *a) {
    uint16_t l1 = M16(a[0]), l2 = M16(a[1]);
    if (l1 != l2) return 0;
    const char *s1 = (const char *)(g_mem + M32(a[0] + 4)), *s2 = (const char *)(g_mem + M32(a[1] + 4));
    return a[2] ? _strnicmp(s1, s2, l1) == 0 : memcmp(s1, s2, l1) == 0;
}
static uint32_t k_RtlAnsiStringToUnicodeString(X86Ctx *c, const uint32_t *a) {
    /* (Dest, Src, AllocateDestination) */
    uint16_t len = M16(a[1]);
    uint32_t buf = a[2] ? guest_alloc(len * 2 + 2, 2) : M32(a[0] + 4);
    if (a[2]) ansi_init(a[0], buf, 0, len * 2 + 2);
    else if (M16(a[0] + 2) < len * 2) return 0x80000005u; /* STATUS_BUFFER_OVERFLOW */
    for (uint16_t i = 0; i < len; i++) M16(buf + i * 2) = M8(M32(a[1] + 4) + i);
    M16(a[0]) = len * 2;
    return STATUS_SUCCESS;
}
static uint32_t k_RtlUnicodeStringToAnsiString(X86Ctx *c, const uint32_t *a) {
    uint16_t len = M16(a[1]) / 2;
    uint32_t buf = a[2] ? guest_alloc(len + 1, 1) : M32(a[0] + 4);
    if (a[2]) ansi_init(a[0], buf, 0, len + 1);
    else if (M16(a[0] + 2) < len) return 0x80000005u;
    for (uint16_t i = 0; i < len; i++) M8(buf + i) = (uint8_t)M16(M32(a[1] + 4) + i * 2);
    M16(a[0]) = len;
    return STATUS_SUCCESS;
}
static uint32_t k_RtlNtStatusToDosError(X86Ctx *c, const uint32_t *a) {
    switch (a[0]) {
    case 0: return 0;
    case 0xC0000034u: return ERROR_FILE_NOT_FOUND;   /* OBJECT_NAME_NOT_FOUND */
    case 0xC000003Au: return ERROR_PATH_NOT_FOUND;
    case 0xC0000035u: return ERROR_ALREADY_EXISTS;   /* OBJECT_NAME_COLLISION */
    case 0xC0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000011u: return ERROR_HANDLE_EOF;       /* END_OF_FILE */
    case 0x00000102u: return WAIT_TIMEOUT;
    default: return ERROR_GEN_FAILURE;
    }
}
/* NtUserIoApcDispatcher(ApcContext = Win32 completion routine, IoStatusBlock = the OVERLAPPED, Reserved):
 * XAPI's ReadFileEx/WriteFileEx APC; calls Completion(dwErrorCode, dwNumberOfBytesTransferred, lpOverlapped). */
static uint32_t k_NtUserIoApcDispatcher(X86Ctx *c, const uint32_t *a) {
    uint32_t status = M32(a[1]), status_arg[1] = {status};
    uint32_t args[3] = {status ? k_RtlNtStatusToDosError(c, status_arg) : 0, M32(a[1] + 4), a[1]};
    guest_call(c, a[0], 3, args);
    return 0;
}

static uint32_t k_RtlCompareMemoryUlong(X86Ctx *c, const uint32_t *a) {
    uint32_t n = 0;
    while (n + 4 <= a[1] && M32(a[0] + n) == a[2]) n += 4;
    return n;
}
static uint32_t k_RtlTimeToTimeFields(X86Ctx *c, const uint32_t *a) {
    /* TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (all u16) */
    FILETIME ft = {M32(a[0]), M32(a[0] + 4)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    uint16_t f[8] = {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, st.wDayOfWeek};
    memcpy(g_mem + a[1], f, sizeof f);
    return 0;
}
static uint32_t k_RtlTimeFieldsToTime(X86Ctx *c, const uint32_t *a) {
    uint16_t f[8];
    memcpy(f, g_mem + a[0], sizeof f);
    SYSTEMTIME st = {f[0], f[1], f[7], f[2], f[3], f[4], f[5], f[6]};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    M32(a[1]) = ft.dwLowDateTime;
    M32(a[1] + 4) = ft.dwHighDateTime;
    return 1;
}

/* ---------------- sections ---------------- */

static uint32_t k_XeLoadSection(X86Ctx *c, const uint32_t *a) {
    M32(a[0] + 0x18) += 1; /* XBE section header SectionReferenceCount; data was loaded at boot */
    return STATUS_SUCCESS;
}
static uint32_t k_XeUnloadSection(X86Ctx *c, const uint32_t *a) {
    if (M32(a[0] + 0x18)) M32(a[0] + 0x18) -= 1;
    return STATUS_SUCCESS;
}

/* ---------------- no-op hardware/bookkeeping exports ---------------- */

/* Page locking, cache sizing and FPU state saving have no host-side meaning (memory is always resident,
 * the FPU state lives in X86Ctx). */
static uint32_t k_MmLockUnlockBufferPages(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_MmLockUnlockPhysicalPage(X86Ctx *c, const uint32_t *a) { return 0; }
static uint32_t k_MmQueryAllocationSize(X86Ctx *c, const uint32_t *a) { return guest_block_size(a[0]); }
static uint32_t k_ExQueryPoolBlockSize(X86Ctx *c, const uint32_t *a) { return guest_block_size(a[0]); }
static uint32_t k_FscSetCacheSize(X86Ctx *c, const uint32_t *a) { return STATUS_SUCCESS; }
static uint32_t k_KeSaveFloatingPointState(X86Ctx *c, const uint32_t *a) { return STATUS_SUCCESS; }
static uint32_t k_KeRestoreFloatingPointState(X86Ctx *c, const uint32_t *a) { return STATUS_SUCCESS; }
static uint32_t k_PhyInitialize(X86Ctx *c, const uint32_t *a) { return STATUS_SUCCESS; }
static uint32_t k_PhyGetLinkState(X86Ctx *c, const uint32_t *a) { return 0; } /* no network link */

/* ---------------- I/O manager ---------------- */

static uint32_t k_IoCreateDevice(X86Ctx *c, const uint32_t *a) {
    /* (DriverObject, DeviceExtensionSize, DeviceName, DeviceType, Exclusive, *DeviceObject)
     * DEVICE_OBJECT: Type 0x00, Size 0x02, ReferenceCount 0x04, DriverObject 0x08, ..., Flags 0x14,
     * DeviceExtension 0x18, DeviceType 0x1C, StackSize 0x1E, SectorSize 0x20; extension follows at 0x48 */
    uint32_t dev = guest_alloc(0x48 + a[1], 16);
    M16(dev) = 3; /* IO_TYPE_DEVICE */
    M16(dev + 2) = 0x48;
    M32(dev + 8) = a[0];
    M32(dev + 0x14) = 0x80; /* DO_DEVICE_INITIALIZING */
    M32(dev + 0x18) = a[1] ? dev + 0x48 : 0;
    M8(dev + 0x1C) = (uint8_t)a[3];
    M8(dev + 0x1E) = 1;
    M32(a[5]) = dev;
    return STATUS_SUCCESS;
}

/* ---------------- registration ---------------- */

#define K(name) {#name, k_##name}
static const KImpl g_impls[] = {
    K(AvGetSavedDataAddress), K(AvSetSavedDataAddress), K(AvSetDisplayMode), K(AvSendTVEncoderOption),
    K(DbgPrint), K(HalReadWritePCISpace), K(HalRegisterShutdownNotification), K(HalIsResetOrShutdownPending),
    K(HalReturnToFirmware), K(HalInitiateShutdown), K(KeBugCheck),
    K(KfRaiseIrql), K(KfLowerIrql), K(KeRaiseIrqlToDpcLevel),
    K(NtAllocateVirtualMemory), K(NtFreeVirtualMemory), K(MmAllocateContiguousMemory),
    K(MmAllocateContiguousMemoryEx), K(MmAllocateSystemMemory), K(MmFreeContiguousMemory), K(MmFreeSystemMemory),
    K(MmCreateKernelStack), K(MmDeleteKernelStack), K(MmPersistContiguousMemory), K(MmGetPhysicalAddress), K(MmSetAddressProtect), K(MmQueryAddressProtect),
    K(MmClaimGpuInstanceMemory), K(MmQueryStatistics), K(NtQueryVirtualMemory), K(ExAllocatePoolWithTag), K(ExFreePool),
    K(KeQueryPerformanceCounter), K(KeQueryPerformanceFrequency), K(KeQuerySystemTime),
    K(KeStallExecutionProcessor), K(ExQueryNonVolatileSetting),
    K(RtlInitAnsiString), K(RtlEqualString), K(RtlAnsiStringToUnicodeString),
    K(RtlUnicodeStringToAnsiString), K(RtlNtStatusToDosError), K(RtlCompareMemoryUlong),
    K(RtlTimeToTimeFields), K(RtlTimeFieldsToTime), K(NtUserIoApcDispatcher),
    K(XeLoadSection), K(XeUnloadSection), K(IoCreateDevice),
    K(MmLockUnlockBufferPages), K(MmLockUnlockPhysicalPage), K(MmQueryAllocationSize), K(ExQueryPoolBlockSize),
    K(FscSetCacheSize), K(KeSaveFloatingPointState), K(KeRestoreFloatingPointState), K(PhyInitialize),
    K(PhyGetLinkState),
    {NULL, NULL},
};

static void register_impls(const KImpl *impls) {
    for (; impls->name; impls++) {
        int found = 0;
        for (int o = 0; o < KERNEL_EXPORT_MAX; o++)
            if (g_kexports[o].name && !strcmp(g_kexports[o].name, impls->name)) {
                g_kfns[o] = impls->fn;
                found = 1;
            }
        if (!found) {
            fprintf(stderr, "kernel_init: no export named %s\n", impls->name);
            fatal_exit(1);
        }
    }
}

static uint32_t data_size(const char *name) {
    if (!strcmp(name, "XePublicKeyData")) return 284;
    if (!strcmp(name, "XboxAlternateSignatureKeys")) return 16 * 16;
    if (!strcmp(name, "IdexChannelObject")) return 0x100;
    return 0x40;
}

static void init_data(void) {
    for (int o = 0; o < KERNEL_EXPORT_MAX; o++) {
        if (g_kexports[o].conv != K_DATA) continue;
        const char *n = g_kexports[o].name;
        uint32_t va = kernel_alloc(data_size(n));
        memset(g_mem + va, 0, data_size(n));
        g_kdata[o] = va;
        if (!strcmp(n, "XboxKrnlVersion")) {
            M16(va) = 1; M16(va + 2) = 0; M16(va + 4) = 5838; M16(va + 6) = 1;
        } else if (!strcmp(n, "HalDiskCachePartitionCount")) {
            M32(va) = 3;
        } else if (!strcmp(n, "XboxHDKey")) {
            /* this console's hard-disk key, which signs non-roamable saves: any fixed value works, but it must not be
             * all zeros (XAPI then falls back to the EEPROM) and must never change, or existing saves turn invalid */
            static const uint8_t key[16] = "ForzaRecompHDKey";
            memcpy(g_mem + va, key, 16);
        } else if (!strcmp(n, "IdexChannelObject")) {
            /* the channel's queue of pending IRPs (LIST_ENTRY at +0x28), empty: XAPI's CancelIo walks it */
            M32(va + 0x28) = M32(va + 0x2C) = va + 0x28;
        } else if (!strcmp(n, "HalBootSMCVideoMode")) {
            M32(va) = 1;
        } else if (!strcmp(n, "KeTimeIncrement")) {
            M32(va) = 10000;
        } else if (!strcmp(n, "XeImageFileName")) {
            static const char path[] = "\\Device\\CdRom0\\default.xbe";
            uint32_t buf = kernel_alloc(sizeof path);
            memcpy(g_mem + buf, path, sizeof path);
            ansi_init(va, buf, sizeof path - 1, sizeof path);
        } else if (!strcmp(n, "HalDiskModelNumber") || !strcmp(n, "HalDiskSerialNumber")) {
            uint32_t buf = kernel_alloc(16);
            memcpy(g_mem + buf, "RECOMP", 7);
            ansi_init(va, buf, 6, 7);
        }
        /* LaunchDataPage stays NULL: a normal cold boot */
    }
}

void kernel_init(void) {
    g_trace = getenv("KTRACE") != NULL;
    register_impls(g_impls);
    register_impls(g_thread_impls);
    register_impls(g_file_impls);
    register_impls(g_crypto_impls);
    crypto_selftest();
    mmx_selftest();
    guest_alloc_selftest();
    init_data();

    /* thunk table: 0x80000000|ordinal per slot, terminated by 0 */
    int n = 0;
    for (uint32_t slot = g_xbe.kernel_thunk; M32(slot); slot += 4, n++) {
        uint32_t ord = M32(slot) & 0x7FFFFFFFu;
        if (ord >= KERNEL_EXPORT_MAX || !g_kexports[ord].name) {
            fprintf(stderr, "kernel_init: bad import ordinal %u\n", ord);
            fatal_exit(1);
        }
        M32(slot) = g_kexports[ord].conv == K_DATA ? g_kdata[ord] : KTHUNK_BASE + ord;
        if (g_trace && g_kexports[ord].conv == K_DATA)
            fprintf(stderr, "[k] data %s at %08X\n", g_kexports[ord].name, g_kdata[ord]);
    }
    fprintf(stderr, "kernel: patched %d imports\n", n);
}

/* KWATCH=<hex guest address>: report whenever that dword changed since the previous kernel call */
static void watch(X86Ctx *c, const char *name) {
    static uint32_t addr, last;
    static int init;
    if (!init) {
        const char *w = getenv("KWATCH");
        addr = w ? strtoul(w, NULL, 16) : 0;
        last = addr ? M32(addr) : 0;
        init = 1;
    }
    if (addr && M32(addr) != last) {
        fprintf(stderr, "[watch] %08X: %08X -> %08X before %s, last function %08X\n", addr, last, M32(addr), name,
                c->last_fn);
        last = M32(addr);
    }
}

void kernel_call(X86Ctx *c, uint32_t ord) {
    const KExport *e = ord < KERNEL_EXPORT_MAX ? &g_kexports[ord] : NULL;
    if (!e || !e->name) x86_fault(c, KTHUNK_BASE + ord, "call to unknown kernel ordinal");
    watch(c, e->name);

    /* gather args: [esp] is the return address, stack args follow */
    uint32_t a[12] = {0}, nstack;
    if (e->conv == K_FASTCALL) {
        a[0] = c->ecx;
        a[1] = c->edx;
        nstack = e->argbytes > 8 ? (e->argbytes - 8) / 4 : 0;
        for (uint32_t i = 0; i < nstack && i < 10; i++) a[2 + i] = M32(c->esp + 4 + 4 * i);
    } else {
        nstack = e->conv == K_STDCALL ? e->argbytes / 4 : 0;
        for (uint32_t i = 0; i < 12; i++) a[i] = M32(c->esp + 4 + 4 * i); /* cdecl: read a few, pop none */
    }

    if (g_trace)
        fprintf(stderr, "[k] %s(%08X, %08X, %08X, %08X, %08X, %08X, %08X, %08X) from %08X\n", e->name, a[0], a[1],
                a[2], a[3], a[4], a[5], a[6], a[7], c->last_fn);
    if (!g_kfns[ord]) {
        fprintf(stderr, "UNIMPLEMENTED kernel export %u %s (args %08X %08X %08X %08X %08X %08X)\n", ord, e->name,
                a[0], a[1], a[2], a[3], a[4], a[5]);
        exit(4);
    }
    tick();
    c->eax = g_kfns[ord](c, a);
    c->esp += 4 + (e->conv == K_CDECL ? 0 : nstack * 4);
}
