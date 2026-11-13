/* File system HLE: Xbox object paths -> host files.
 *
 * \Device\CdRom0              -> <game dir>          (read-only disc)
 * \Device\Harddisk0\PartitionN -> <game dir>\..\hdd\PartitionN
 * Drive letters (D:, T:, U:, Z: ...) resolve through symbolic links the title creates with IoCreateSymbolicLink;
 * D: defaults to the disc. File handles are small kernel objects whose index points into g_files.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "xbox.h"

#define STATUS_SUCCESS 0x00000000u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_ACCESS_DENIED 0xC0000022u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_FILE_IS_A_DIRECTORY 0xC00000BAu
#define STATUS_NOT_A_DIRECTORY 0xC0000103u

#define FILE_OBJ_TYPE 0xF0 /* our marker in the DISPATCHER_HEADER Type byte */
#define MAX_FILES 1024
#define MAX_LINKS 64
#define GPATH 260

typedef struct {
    uint32_t obj;
    HANDLE h;
    HANDLE find; /* directory enumeration state */
    int is_dir;
    int unbuffered;   /* opened with FILE_NO_INTERMEDIATE_BUFFERING: whole-sector writes */
    int has_eof;      /* end of file as the title last set it (or found at open) ... */
    int64_t eof;      /* ... see rw() */
    char guest[GPATH], host[MAX_PATH];
} HFile;
static HFile g_files[MAX_FILES];
static struct {
    char link[64], target[GPATH];
} g_links[MAX_LINKS] = {{"\\??\\D:", "\\Device\\CdRom0"}};

/* ---------------- paths ---------------- */

static void ansi_read(uint32_t str, char *out, size_t cap) {
    uint16_t len = M16(str);
    if (len >= cap) len = (uint16_t)(cap - 1);
    memcpy(out, g_mem + M32(str + 4), len);
    out[len] = 0;
}

static HFile *file_of(uint32_t handle) {
    if (handle < KERNEL_BASE || handle >= KERNEL_BASE + KERNEL_SIZE || M8(handle) != FILE_OBJ_TYPE) return NULL;
    HFile *f = &g_files[M32(handle + 0x10)];
    return f->obj == handle ? f : NULL;
}

/* OBJECT_ATTRIBUTES (Xbox): RootDirectory 0x00, ObjectName (ANSI_STRING*) 0x04, Attributes 0x08 */
static void guest_path(uint32_t oa, char *out) {
    char name[GPATH];
    ansi_read(M32(oa + 4), name, sizeof name);
    HFile *root = file_of(M32(oa));
    if (root) snprintf(out, GPATH, "%s\\%s", root->guest, name);
    else snprintf(out, GPATH, "%s", name);
}

static int starts_with_i(const char *s, const char *prefix) { return !_strnicmp(s, prefix, strlen(prefix)); }

/* "\Device\Harddisk0\PartitionN" with nothing (or just "\") after it: the whole volume */
static int is_partition_root(const char *guest) {
    const char *p = guest;
    if (!starts_with_i(p, "\\Device\\Harddisk0\\Partition")) return 0;
    p += strlen("\\Device\\Harddisk0\\Partition");
    while (*p >= '0' && *p <= '9') p++;
    return !*p || (p[0] == '\\' && !p[1]);
}

/* ---------------- mods ----------------
 * <game dir>\..\mods\<mod>\ holds files at the disc's own paths (media\cars\...), each taking the place of the disc's
 * file. Every folder in mods is a mod, on unless recomp.ini mod_<name> = 0 (Recomp Settings > MODS); by name, the
 * first mod that has a file wins. Read once at startup, so a change takes effect at the next start. */
#define MAX_MODS 24
static char g_mod_name[MAX_MODS][64];
static int g_nmods = -1;
typedef struct {
    char *rel, *host; /* "\media\x.xpr" (as the guest's path after the device), the mod's file */
    int mod;
} ModFile;
static ModFile *g_modfiles;
static int g_nmodfiles;

static void mod_walk(const char *dir, const char *rel, int mod, int *cap) {
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sub[MAX_PATH], subrel[MAX_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        snprintf(subrel, sizeof subrel, "%s\\%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) mod_walk(sub, subrel, mod, cap);
        else {
            if (g_nmodfiles == *cap) g_modfiles = realloc(g_modfiles, (*cap = *cap ? *cap * 2 : 256) * sizeof *g_modfiles);
            g_modfiles[g_nmodfiles++] = (ModFile){_strdup(subrel), _strdup(sub), mod};
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int modfile_cmp(const void *a, const void *b) {
    const ModFile *x = a, *y = b;
    int c = _stricmp(x->rel, y->rel);
    return c ? c : x->mod - y->mod;
}
static int name_cmp(const void *a, const void *b) { return _stricmp(a, b); }

static const char *mod_key(int i) { /* mod_<name>, with anything but letters and digits as _ */
    static char k[80];
    snprintf(k, sizeof k, "mod_%s", g_mod_name[i]);
    for (char *p = k + 4; *p; p++)
        if (!isalnum((unsigned char)*p)) *p = '_';
    return k;
}

static void mods_init(void) {
    if (g_nmods >= 0) return;
    g_nmods = 0;
    char dir[MAX_PATH], pat[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\..\\mods", guest_path_root());
    CreateDirectoryA(dir, NULL); /* there to be seen: where mods go */
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && g_nmods < MAX_MODS &&
            strlen(fd.cFileName) < sizeof g_mod_name[0])
            strcpy(g_mod_name[g_nmods++], fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_mod_name, g_nmods, sizeof g_mod_name[0], name_cmp);
    int cap = 0;
    for (int i = 0; i < g_nmods; i++) {
        int on = cfg_saved(mod_key(i), 1);
        fprintf(stderr, "[mods] %s: %s\n", g_mod_name[i], on ? "on" : "off");
        if (!on) continue;
        char sub[MAX_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", dir, g_mod_name[i]);
        mod_walk(sub, "", i, &cap);
    }
    qsort(g_modfiles, g_nmodfiles, sizeof *g_modfiles, modfile_cmp);
    int n = 0; /* one per path: the first mod's */
    for (int i = 0; i < g_nmodfiles; i++)
        if (!n || _stricmp(g_modfiles[n - 1].rel, g_modfiles[i].rel)) g_modfiles[n++] = g_modfiles[i];
    g_nmodfiles = n;
}

int mods_count(void) { return mods_init(), g_nmods; }
const char *mods_name(int i) { return i >= 0 && i < mods_count() ? g_mod_name[i] : ""; }
int mods_on(int i) { return i >= 0 && i < mods_count() && cfg_saved(mod_key(i), 1); } /* as saved: next start */
void mods_toggle(int i) {
    if (i >= 0 && i < mods_count()) cfg_save(mod_key(i), !mods_on(i));
}

/* the mod's file for a path on the disc ("\media\x.xpr"), or NULL */
static const char *mod_file(const char *rest) {
    mods_init();
    if (!g_nmodfiles) return NULL;
    char key[GPATH];
    size_t n = 0;
    for (const char *s = rest; *s && n < sizeof key - 1; s++) /* one backslash between names, none at the end */
        if (*s != '\\' && *s != '/') key[n++] = *s;
        else if (n == 0 || key[n - 1] != '\\') key[n++] = '\\';
    while (n > 1 && key[n - 1] == '\\') n--;
    key[n] = 0;
    ModFile k = {key, NULL, 0};
    int lo = 0, hi = g_nmodfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = _stricmp(g_modfiles[mid].rel, k.rel);
        if (!c) {
            static int logged[4096]; /* each path once */
            if (mid < 4096 && !logged[mid]++) fprintf(stderr, "[mods] %s from %s\n", key, g_mod_name[g_modfiles[mid].mod]);
            return g_modfiles[mid].host;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* guest object path -> host path; returns 1 for the disc, 2 for the hard disk, 0 if unknown */
static int host_path(const char *guest, char *out) {
    char p[GPATH * 2];
    snprintf(p, sizeof p, "%s", guest);
    if (starts_with_i(p, "\\??\\")) memmove(p, p + 4, strlen(p + 4) + 1);
    if (p[0] && p[1] == ':') { /* drive letter -> symbolic link target */
        char link[8] = {'\\', '?', '?', '\\', p[0], ':', 0};
        int i;
        for (i = 0; i < MAX_LINKS && _stricmp(g_links[i].link, link); i++) {}
        if (i == MAX_LINKS) return 0;
        char rest[GPATH];
        snprintf(rest, sizeof rest, "%s", p + 2);
        snprintf(p, sizeof p, "%s%s", g_links[i].target, rest);
    }
    const char *root = guest_path_root();
    int dev;
    if (starts_with_i(p, "\\Device\\CdRom0")) {
        const char *mod = mod_file(p + strlen("\\Device\\CdRom0"));
        if (mod) snprintf(out, MAX_PATH, "%s", mod);
        else snprintf(out, MAX_PATH, "%s%s", root, p + strlen("\\Device\\CdRom0"));
        dev = 1;
    } else if (starts_with_i(p, "\\Device\\Harddisk0\\Partition")) {
        const char *rest = p + strlen("\\Device\\Harddisk0\\Partition");
        int n = atoi(rest);
        while (*rest >= '0' && *rest <= '9') rest++;
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\..\\hdd", root);
        CreateDirectoryA(dir, NULL); /* hdd\PartitionN is created next to the game folder on first use */
        snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "\\Partition%d", n);
        CreateDirectoryA(dir, NULL);
        snprintf(out, MAX_PATH, "%s%s", dir, rest);
        dev = 2;
    } else {
        return 0;
    }
    size_t n = strlen(out);
    while (n > 3 && (out[n - 1] == '\\' || out[n - 1] == '/')) out[--n] = 0; /* "dir\" opens the dir */
    return dev;
}

static uint32_t status_from_win32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
    case ERROR_ACCESS_DENIED: return STATUS_ACCESS_DENIED;
    case ERROR_HANDLE_EOF: return STATUS_END_OF_FILE;
    default: return 0xC0000001u; /* STATUS_UNSUCCESSFUL */
    }
}

static void iosb(uint32_t p, uint32_t status, uint32_t info) {
    if (!p) return;
    M32(p) = status;
    M32(p + 4) = info;
}

static void put_time(uint32_t va, FILETIME t) {
    M32(va) = t.dwLowDateTime;
    M32(va + 4) = t.dwHighDateTime;
}

/* FILELOG=1: the [file] trace without the rest of KTRACE (which slows the game enough to change its path) */
static int flog(void) {
    static int on = -1;
    if (on < 0) on = ktrace() || getenv("FILELOG");
    return on;
}

/* ---------------- open / create ---------------- */

static uint32_t open_file(X86Ctx *c, uint32_t out_handle, uint32_t access, uint32_t oa, uint32_t iosb_va,
                          uint32_t disposition, uint32_t options) {
    char guest[GPATH], host[MAX_PATH];
    guest_path(oa, guest);
    achieve_file(guest, (access & 0x40000006u) != 0 || disposition != 1); /* GENERIC_WRITE, FILE_WRITE/APPEND_DATA */
    int dev = host_path(guest, host);
    if (!dev) {
        fprintf(stderr, "[file] unknown device in %s\n", guest);
        iosb(iosb_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    int want_dir = options & 1 /* FILE_DIRECTORY_FILE */;
    /* the host calls of an open leave the guest lock (paths are our own copies): an open can take tens of
     * milliseconds (the antivirus looks at the file), and the game opens files while a video plays or a race loads,
     * which held up every other thread, the video's included. One slower than 20 ms is logged. */
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    gil_release();
    DWORD attr = GetFileAttributesA(host);
    gil_acquire();
    int exists = attr != INVALID_FILE_ATTRIBUTES, is_dir = exists && (attr & FILE_ATTRIBUTE_DIRECTORY);
    uint32_t info = 1; /* FILE_OPENED */

    if (want_dir && !exists && (disposition == 2 || disposition == 3)) { /* FILE_CREATE / FILE_OPEN_IF */
        if (!CreateDirectoryA(host, NULL)) {
            uint32_t st = status_from_win32(GetLastError());
            if (flog()) fprintf(stderr, "[file] mkdir %s -> %08X\n", guest, st);
            return st;
        }
        is_dir = exists = 1;
        info = 2; /* FILE_CREATED */
    }
    if (exists && is_dir && (options & 0x40 /* FILE_NON_DIRECTORY_FILE */)) {
        if (flog()) fprintf(stderr, "[file] open %s -> is a directory\n", guest);
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    if (exists && !is_dir && want_dir) {
        if (flog()) fprintf(stderr, "[file] open %s -> not a directory\n", guest);
        return STATUS_NOT_A_DIRECTORY;
    }
    if (exists && disposition == 2 && info != 2) { /* a directory this call just created is no collision */
        if (flog()) fprintf(stderr, "[file] create %s -> exists\n", guest);
        iosb(iosb_va, STATUS_OBJECT_NAME_COLLISION, 4);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    HANDLE h = INVALID_HANDLE_VALUE;
    if (!is_dir) {
        static const DWORD dispositions[] = {CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS,
                                             TRUNCATE_EXISTING, CREATE_ALWAYS};
        DWORD disp = disposition < 6 ? dispositions[disposition] : OPEN_EXISTING;
        int writable = (access & (0x40000000u /* GENERIC_WRITE */ | 0x2 /* FILE_WRITE_DATA */ | 0x4 | 0x10000000u)) != 0;
        if (dev == 1) writable = 0; /* never write into the extracted disc */
        gil_release();
        h = CreateFileA(host, GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD err = GetLastError();
        gil_acquire();
        QueryPerformanceCounter(&q1), QueryPerformanceFrequency(&qf);
        double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
        if (ms > 20) fprintf(stderr, "[file] opening %s took %.0f ms\n", guest, ms);
        if (h == INVALID_HANDLE_VALUE) {
            uint32_t st = status_from_win32(err);
            iosb(iosb_va, st, exists ? 1 : 5);
            if (flog())
                fprintf(stderr, "[file] open %s -> %08X (disposition %u, options %08X, access %08X)\n", guest, st,
                        disposition, options, access);
            return st;
        }
        if (!exists) info = 2;
        else if (disposition == 4 || disposition == 5) info = 3; /* FILE_OVERWRITTEN */
    } else if (!exists) {
        iosb(iosb_va, STATUS_OBJECT_NAME_NOT_FOUND, 5);
        if (flog()) fprintf(stderr, "[file] open %s -> not found\n", guest);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    } else if (dev == 2 && is_partition_root(guest)) {
        /* raw volume access (cache-partition table, formatting) goes to a sector image beside the folder */
        char raw[MAX_PATH];
        snprintf(raw, sizeof raw, "%s.raw", host);
        h = CreateFileA(raw, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER size;
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && !size.QuadPart) {
            size.QuadPart = 16 << 20; /* blank disk: reads return zeros like an unformatted drive */
            SetFilePointerEx(h, size, NULL, FILE_BEGIN);
            SetEndOfFile(h);
        }
    }

    int i;
    for (i = 0; i < MAX_FILES && g_files[i].obj; i++) {}
    if (i == MAX_FILES) {
        fprintf(stderr, "too many open files\n");
        fatal_exit(1);
    }
    uint32_t obj = kernel_alloc(0x20);
    memset(g_mem + obj, 0, 0x20);
    M8(obj) = FILE_OBJ_TYPE;
    M32(obj + 4) = 1; /* signaled: all I/O completes synchronously */
    M32(obj + 0x10) = i;
    /* FILE_OBJECT.FsContext (+8), read by XGetFilePhysicalSortKey: GDFX keeps the start sector in dword 0,
     * FATX has flags in dword 0 (bit 0: directory) and the first cluster at +0x1C. Any stable value sorts. */
    uint32_t fs_ctx = kernel_alloc(0x20);
    memset(g_mem + fs_ctx, 0, 0x20);
    M32(fs_ctx) = dev == 1 ? i + 1 : is_dir;
    M32(fs_ctx + 0x1C) = i + 1;
    M32(obj + 8) = fs_ctx;
    g_files[i] = (HFile){obj, h, INVALID_HANDLE_VALUE, is_dir};
    LARGE_INTEGER size;
    if ((options & 8) && !is_dir && GetFileSizeEx(h, &size)) /* FILE_NO_INTERMEDIATE_BUFFERING */
        g_files[i].unbuffered = 1, g_files[i].has_eof = 1, g_files[i].eof = size.QuadPart;
    snprintf(g_files[i].guest, GPATH, "%s", guest);
    snprintf(g_files[i].host, MAX_PATH, "%s", host);
    M32(out_handle) = obj;
    iosb(iosb_va, STATUS_SUCCESS, info);
    if (flog()) fprintf(stderr, "[file] open %s -> %08X%s\n", guest, obj, is_dir ? " (dir)" : "");
    return STATUS_SUCCESS;
}

static uint32_t k_NtCreateFile(X86Ctx *c, const uint32_t *a) {
    /* (*FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes,
     *  ShareAccess, CreateDisposition, CreateOptions) */
    return open_file(c, a[0], a[1], a[2], a[3], a[7], a[8]);
}
static uint32_t k_NtOpenFile(X86Ctx *c, const uint32_t *a) {
    /* (*FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions) */
    return open_file(c, a[0], a[1], a[2], a[3], 1 /* FILE_OPEN */, a[5]);
}

int file_close(uint32_t handle) {
    HFile *f = file_of(handle);
    if (!f) return 0;
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
    if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
    memset(f, 0, sizeof *f);
    M8(handle) = 0;
    return 1;
}

/* ---------------- read / write ---------------- */

static void complete_io(X86Ctx *c, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb_va) {
    if (event) M32(event + 4) = 1;
    if (apc) queue_user_apc(c, apc, apc_ctx, iosb_va, 0); /* runs at the thread's next alertable wait */
}

static uint32_t cluster_sectors(const HFile *f);
static int partition_of(const HFile *f);

/* delete everything in a folder: a cache partition the title formats */
static void empty_dir(const char *dir) {
    char p[MAX_PATH];
    WIN32_FIND_DATAA d;
    snprintf(p, sizeof p, "%s\\*", dir);
    HANDLE h = FindFirstFileA(p, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, "..")) continue;
        snprintf(p, sizeof p, "%s\\%s", dir, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) empty_dir(p), RemoveDirectoryA(p);
        else DeleteFileA(p);
    } while (FindNextFileA(h, &d));
    FindClose(h);
}

static uint32_t rw(X86Ctx *c, const uint32_t *a, int write) {
    /* (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, *ByteOffset) */
    HFile *f = file_of(a[0]);
    if (!f || f->h == INVALID_HANDLE_VALUE) return STATUS_INVALID_HANDLE; /* dirs only via a raw image */
    DWORD done = 0;
    BOOL ok;
    if (!write && a[7]) { /* a read at an offset leaves the guest lock while the disk works (a hard drive stalled
                           * every thread); positioned, so another thread's use of the handle cannot move it */
        uint64_t at = M64(a[7]);
        OVERLAPPED o = {.Offset = (DWORD)at, .OffsetHigh = (DWORD)(at >> 32)};
        HANDLE h = f->h;
        uint8_t *buf = g_mem + a[5];
        DWORD len = a[6];
        gil_release();
        ok = ReadFile(h, buf, len, &done, &o);
        DWORD err = GetLastError();
        gil_acquire();
        if (!ok && err == ERROR_HANDLE_EOF) ok = TRUE, done = 0; /* at the end: 0 bytes, as an unpositioned read gives */
        else SetLastError(err);
    } else {
        if (a[7]) {
            LARGE_INTEGER off;
            off.QuadPart = (LONGLONG)M64(a[7]);
            SetFilePointerEx(f->h, off, NULL, FILE_BEGIN);
        }
        ok = write ? WriteFile(f->h, g_mem + a[5], a[6], &done, NULL) : ReadFile(f->h, g_mem + a[5], a[6], &done, NULL);
    }
    uint32_t st = !ok ? status_from_win32(GetLastError()) : (!write && a[6] && !done) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    if (write && ok && f->unbuffered && f->has_eof) {
        /* FATX: an unbuffered write that runs past the end of file but stays inside the file's last allocated
         * cluster does not move the end of file (installers presize to the exact size, then write whole
         * sectors). Windows would extend the file, so put the title's end of file back. */
        LARGE_INTEGER size;
        int64_t cluster = cluster_sectors(f) * 512ll, alloc_end = (f->eof + cluster - 1) / cluster * cluster;
        if (GetFileSizeEx(f->h, &size) && size.QuadPart > f->eof && size.QuadPart <= alloc_end) {
            LARGE_INTEGER pos, end = {.QuadPart = f->eof}, zero = {0};
            SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
            SetFilePointerEx(f->h, end, NULL, FILE_BEGIN);
            SetEndOfFile(f->h);
            SetFilePointerEx(f->h, pos, NULL, FILE_BEGIN);
        }
    }
    if (write && ok && a[7] && !M64(a[7]) && done >= 4 && !memcmp(g_mem + a[5], "FATX", 4) && is_partition_root(f->guest) &&
        partition_of(f) >= 3) { /* a new FATX superblock: the title formatted a cache partition (Forza does N: at every
                                 * start), which leaves it empty; the files in its folder go too (a replay's "Created"
                                 * date is LastReplay.FRP's creation, and the file was days old) */
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s", f->host);
        size_t n = strlen(dir);
        if (n && dir[n - 1] == '\\') dir[n - 1] = 0;
        empty_dir(dir);
        if (flog()) fprintf(stderr, "[file] %s formatted: emptied %s\n", f->guest, dir);
    }
    if (flog())
        fprintf(stderr, "[file] %s %s: %u bytes at %lld, status %08X, data %08X %08X\n", write ? "write" : "read",
                f->guest, done, a[7] ? (long long)M64(a[7]) : -1ll, st, done >= 4 ? M32(a[5]) : 0,
                done >= 8 ? M32(a[5] + 4) : 0);
    /* NT: a request that fails synchronously with an error status leaves the I/O status block untouched and
     * neither signals the event nor queues the APC (XAPI pre-sets OVERLAPPED.Internal to STATUS_PENDING, so an
     * overlapped read past the end of file then completes as "0 bytes" rather than as an error). */
    if (st >= 0xC0000000u) return st;
    iosb(a[4], st, done);
    complete_io(c, a[1], a[2], a[3], a[4]);
    return st;
}
static uint32_t k_NtReadFile(X86Ctx *c, const uint32_t *a) { return rw(c, a, 0); }
static uint32_t k_NtWriteFile(X86Ctx *c, const uint32_t *a) { return rw(c, a, 1); }
static uint32_t k_NtFlushBuffersFile(X86Ctx *c, const uint32_t *a) {
    HFile *f = file_of(a[0]);
    if (f && f->h != INVALID_HANDLE_VALUE) FlushFileBuffers(f->h);
    iosb(a[1], STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

/* ---------------- information ---------------- */

static void network_open_info(uint32_t va, const WIN32_FILE_ATTRIBUTE_DATA *d) {
    put_time(va + 0x00, d->ftCreationTime);
    put_time(va + 0x08, d->ftLastAccessTime);
    put_time(va + 0x10, d->ftLastWriteTime);
    put_time(va + 0x18, d->ftLastWriteTime);
    uint64_t size = ((uint64_t)d->nFileSizeHigh << 32) | d->nFileSizeLow;
    M64(va + 0x20) = (size + 4095) & ~4095ull;
    M64(va + 0x28) = size;
    M32(va + 0x30) = d->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY) ? d->dwFileAttributes
                                                                                                 : FILE_ATTRIBUTE_NORMAL;
    M32(va + 0x34) = 0;
}

static uint32_t k_NtQueryInformationFile(X86Ctx *c, const uint32_t *a) {
    /* (FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
    HFile *f = file_of(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    WIN32_FILE_ATTRIBUTE_DATA d;
    GetFileAttributesExA(f->host, GetFileExInfoStandard, &d);
    uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    uint32_t out = a[2], len = 0;
    switch (a[4]) {
    case 4: /* FileBasicInformation */
        put_time(out, d.ftCreationTime); put_time(out + 8, d.ftLastAccessTime);
        put_time(out + 16, d.ftLastWriteTime); put_time(out + 24, d.ftLastWriteTime);
        M32(out + 32) = d.dwFileAttributes; len = 40;
        break;
    case 5: /* FileStandardInformation */
        M64(out) = (size + 4095) & ~4095ull; M64(out + 8) = size; M32(out + 16) = 1;
        M8(out + 20) = 0; M8(out + 21) = (uint8_t)f->is_dir; len = 24;
        break;
    case 14: { /* FilePositionInformation */
        LARGE_INTEGER zero = {0}, pos = {0};
        if (!f->is_dir) SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        M64(out) = (uint64_t)pos.QuadPart; len = 8;
        break;
    }
    case 34: /* FileNetworkOpenInformation */
        network_open_info(out, &d); len = 56;
        break;
    default:
        fprintf(stderr, "NtQueryInformationFile: class %u not implemented\n", a[4]);
        exit(4);
    }
    iosb(a[1], STATUS_SUCCESS, len);
    return STATUS_SUCCESS;
}

static uint32_t k_NtSetInformationFile(X86Ctx *c, const uint32_t *a) {
    /* (FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
    HFile *f = file_of(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    LARGE_INTEGER v;
    switch (a[4]) {
    case 4: { /* FileBasicInformation: Creation, LastAccess, LastWrite, Change times; 0 or -1 = leave as is.
               * Installers stamp copies with the source's time and compare them later, so this must stick. */
        FILETIME t[3];
        FILETIME *p[3];
        for (int i = 0; i < 3; i++) {
            uint64_t v = M64(a[2] + 8 * i);
            t[i].dwLowDateTime = (DWORD)v;
            t[i].dwHighDateTime = (DWORD)(v >> 32);
            p[i] = v && v != ~0ull ? &t[i] : NULL;
        }
        if (f->h != INVALID_HANDLE_VALUE && (p[0] || p[1] || p[2]) && !SetFileTime(f->h, p[0], p[1], p[2]) && ktrace())
            fprintf(stderr, "[file] SetFileTime on %s failed (%lu)\n", f->guest, GetLastError());
        break;
    }
    case 14: /* FilePositionInformation */
        v.QuadPart = (LONGLONG)M64(a[2]);
        SetFilePointerEx(f->h, v, NULL, FILE_BEGIN);
        break;
    case 19:   /* FileAllocationInformation: reserves space; only shrinks the file if it is smaller than it */
    case 20: { /* FileEndOfFileInformation */
        LARGE_INTEGER zero = {0}, pos, size;
        v.QuadPart = (LONGLONG)M64(a[2]);
        if (a[4] == 19 && (!GetFileSizeEx(f->h, &size) || v.QuadPart >= size.QuadPart)) break;
        if (a[4] == 20) f->has_eof = 1, f->eof = v.QuadPart;
        SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        SetFilePointerEx(f->h, v, NULL, FILE_BEGIN);
        SetEndOfFile(f->h);
        SetFilePointerEx(f->h, pos, NULL, FILE_BEGIN);
        break;
    }
    case 13: /* FileDispositionInformation: delete when closed */
        if (M8(a[2])) {
            if (f->h != INVALID_HANDLE_VALUE) { CloseHandle(f->h); f->h = INVALID_HANDLE_VALUE; }
            if (f->is_dir ? !RemoveDirectoryA(f->host) : !DeleteFileA(f->host)) return status_from_win32(GetLastError());
        }
        break;
    default:
        fprintf(stderr, "NtSetInformationFile: class %u not implemented\n", a[4]);
        exit(4);
    }
    iosb(a[1], STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static uint32_t k_NtQueryFullAttributesFile(X86Ctx *c, const uint32_t *a) {
    /* (ObjectAttributes, FileNetworkOpenInformation*) */
    char guest[GPATH], host[MAX_PATH];
    guest_path(a[0], guest);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!host_path(guest, host) || !GetFileAttributesExA(host, GetFileExInfoStandard, &d)) {
        if (flog()) fprintf(stderr, "[file] attributes %s -> not found\n", guest);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    network_open_info(a[1], &d);
    return STATUS_SUCCESS;
}

/* hard-disk partition number a handle lives on (from its resolved host path), or -1 for the disc */
static int partition_of(const HFile *f) {
    const char *p = strstr(f->host, "\\hdd\\Partition");
    return p ? atoi(p + strlen("\\hdd\\Partition")) : -1;
}

static uint64_t partition_size(const HFile *f);

/* sectors per cluster from the FATX superblock the title formatted into PartitionN.raw (default 16 KiB) */
static uint32_t cluster_sectors(const HFile *f) {
    const char *p = strstr(f->host, "\\hdd\\Partition");
    if (!p) return 32;
    char raw[MAX_PATH];
    snprintf(raw, sizeof raw, "%.*s%d.raw", (int)(p - f->host + strlen("\\hdd\\Partition")), f->host, partition_of(f));
    uint32_t hdr[3] = {0};
    FILE *fp = fopen(raw, "rb");
    if (fp) {
        fread(hdr, 4, 3, fp);
        fclose(fp);
    }
    return !memcmp(hdr, "FATX", 4) && hdr[2] ? hdr[2] : 32; /* FATX: Signature, VolumeId, SectorsPerCluster */
}

static uint32_t k_NtQueryVolumeInformationFile(X86Ctx *c, const uint32_t *a) {
    /* (FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass) */
    HFile *f = file_of(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    uint32_t spc = cluster_sectors(f);
    uint64_t units = (partition_of(f) < 0 ? 0x1C0000000ull : partition_size(f)) / (spc * 512ull);
    uint64_t avail = units - units / 64; /* report the volume as almost empty */
    if (flog())
        fprintf(stderr, "[file] volume info class %u on %s: %llu units of %u sectors, %llu free\n", a[4], f->guest,
                units, spc, avail);
    uint32_t out = a[2], len;
    switch (a[4]) {
    case 3: /* FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits, SectorsPerUnit, BytesPerSector */
        M64(out) = units; M64(out + 8) = avail; M32(out + 16) = spc; M32(out + 20) = 512; len = 24;
        break;
    case 7: /* FileFsFullSizeInformation: Total, CallerAvailable, ActualAvailable, SectorsPerUnit, BytesPerSector */
        M64(out) = units; M64(out + 8) = avail; M64(out + 16) = avail;
        M32(out + 24) = spc; M32(out + 28) = 512; len = 32;
        break;
    case 1: /* FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects, Label */
        memset(g_mem + out, 0, a[3] < 24 ? a[3] : 24); M32(out + 8) = 0x12345678; len = 18;
        break;
    case 5: /* FileFsAttributeInformation: Attributes, MaxComponentNameLength, NameLength, Name */
        M32(out) = 0; M32(out + 4) = 42; M32(out + 8) = 4;
        memcpy(g_mem + out + 12, partition_of(f) < 0 ? "GDFX" : "FATX", 4); /* disc vs hard disk */
        len = 16;
        break;
    default:
        fprintf(stderr, "NtQueryVolumeInformationFile: class %u not implemented\n", a[4]);
        exit(4);
    }
    iosb(a[1], STATUS_SUCCESS, len);
    return STATUS_SUCCESS;
}

static uint32_t k_IoQueryVolumeInformation(X86Ctx *c, const uint32_t *a) {
    /* (FileObject, FsInformationClass, Length, FsInformation, *ReturnedLength): file objects are our handles */
    static uint32_t iosb_va;
    if (!iosb_va) iosb_va = kernel_alloc(8);
    uint32_t args[5] = {a[0], iosb_va, a[3], a[2], a[1]};
    uint32_t st = k_NtQueryVolumeInformationFile(c, args);
    if (a[4]) M32(a[4]) = M32(iosb_va + 4);
    return st;
}

static uint32_t k_NtQueryDirectoryFile(X86Ctx *c, const uint32_t *a) {
    /* (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass,
     *  FileMask (ANSI_STRING*), RestartScan) -- one FILE_DIRECTORY_INFORMATION entry per call, ANSI name */
    HFile *f = file_of(a[0]);
    if (!f || !f->is_dir) return STATUS_INVALID_HANDLE;
    WIN32_FIND_DATAA fd;
    BOOL ok;
    int restart = (uint8_t)a[9];
    do {
        if (f->find == INVALID_HANDLE_VALUE || restart) { /* restart once, then step past "." and ".." */
            restart = 0;
            char mask[GPATH] = "*", pattern[MAX_PATH];
            if (a[8] && M16(a[8])) ansi_read(a[8], mask, sizeof mask);
            if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
            snprintf(pattern, sizeof pattern, "%s\\%s", f->host, mask);
            f->find = FindFirstFileA(pattern, &fd);
            ok = f->find != INVALID_HANDLE_VALUE;
        } else {
            ok = FindNextFileA(f->find, &fd);
        }
    } while (ok && (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")));
    if (!ok) {
        iosb(a[4], STATUS_NO_MORE_FILES, 0);
        return STATUS_NO_MORE_FILES;
    }
    uint32_t out = a[5], name_len = (uint32_t)strlen(fd.cFileName);
    if (0x40 + name_len > a[6]) return STATUS_INVALID_PARAMETER;
    memset(g_mem + out, 0, 0x40);
    put_time(out + 0x08, fd.ftCreationTime);
    put_time(out + 0x10, fd.ftLastAccessTime);
    put_time(out + 0x18, fd.ftLastWriteTime);
    put_time(out + 0x20, fd.ftLastWriteTime);
    uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    M64(out + 0x28) = size;
    M64(out + 0x30) = (size + 4095) & ~4095ull;
    M32(out + 0x38) = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    M32(out + 0x3C) = name_len;
    memcpy(g_mem + out + 0x40, fd.cFileName, name_len);
    iosb(a[4], STATUS_SUCCESS, 0x40 + name_len);
    complete_io(c, a[1], a[2], a[3], a[4]);
    return STATUS_SUCCESS;
}

static uint32_t k_NtDeleteFile(X86Ctx *c, const uint32_t *a) {
    char guest[GPATH], host[MAX_PATH];
    guest_path(a[0], guest);
    if (!host_path(guest, host)) return STATUS_OBJECT_PATH_NOT_FOUND;
    DWORD attr = GetFileAttributesA(host);
    BOOL ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(host) : DeleteFileA(host);
    return ok ? STATUS_SUCCESS : status_from_win32(GetLastError());
}

/* ---------------- volume control ---------------- */

static uint64_t partition_size(const HFile *f) {
    /* stock Xbox partition sizes: 0 config, 1 E:, 2 C:, 3-5 cache (X/Y/Z) */
    static const uint64_t sizes[] = {0x80000ull, 0x1312D6000ull, 0x1F400000ull, 0x2EE00000ull, 0x2EE00000ull,
                                     0x2EE00000ull};
    int n = partition_of(f);
    return n >= 0 && n < 6 ? sizes[n] : sizes[1];
}

static uint32_t k_NtDeviceIoControlFile(X86Ctx *c, const uint32_t *a) {
    /* (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength,
     *  OutputBuffer, OutputBufferLength) */
    HFile *f = file_of(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    uint64_t size = partition_size(f);
    uint32_t out = a[8], len = 0;
    switch (a[5]) {
    case 0x70000: /* IOCTL_DISK_GET_DRIVE_GEOMETRY: Cylinders, MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector */
        M64(out) = size / 512; M32(out + 8) = 12 /* FixedMedia */; M32(out + 12) = 1; M32(out + 16) = 1;
        M32(out + 20) = 512; len = 24;
        break;
    case 0x74004: /* IOCTL_DISK_GET_PARTITION_INFO: StartingOffset, PartitionLength, HiddenSectors, Number, Type... */
        memset(g_mem + out, 0, 32);
        M64(out + 8) = size; M8(out + 24) = 0x0B; M8(out + 26) = 1; len = 32;
        break;
    default:
        fprintf(stderr, "NtDeviceIoControlFile: IOCTL %08X not implemented\n", a[5]);
        exit(4);
    }
    iosb(a[4], STATUS_SUCCESS, len);
    complete_io(c, a[1], a[2], a[3], a[4]);
    return STATUS_SUCCESS;
}

static uint32_t k_NtFsControlFile(X86Ctx *c, const uint32_t *a) {
    /* same layout as NtDeviceIoControlFile; dismount/lock/unlock need no host work */
    if (flog()) fprintf(stderr, "[file] FSCTL %08X\n", a[5]);
    iosb(a[4], STATUS_SUCCESS, 0);
    complete_io(c, a[1], a[2], a[3], a[4]);
    return STATUS_SUCCESS;
}

/* ---------------- symbolic links ---------------- */

static uint32_t k_IoCreateSymbolicLink(X86Ctx *c, const uint32_t *a) {
    /* (SymbolicLinkName, DeviceName) */
    char link[64], target[GPATH];
    ansi_read(a[0], link, sizeof link);
    ansi_read(a[1], target, sizeof target);
    int i, free_slot = -1;
    for (i = 0; i < MAX_LINKS; i++) {
        if (!_stricmp(g_links[i].link, link)) break;
        if (!g_links[i].link[0] && free_slot < 0) free_slot = i;
    }
    if (i == MAX_LINKS) i = free_slot;
    if (i < 0) return 0xC0000017u;
    snprintf(g_links[i].link, sizeof g_links[i].link, "%s", link);
    snprintf(g_links[i].target, sizeof g_links[i].target, "%s", target);
    if (flog()) fprintf(stderr, "[file] link %s -> %s\n", link, target);
    return STATUS_SUCCESS;
}
static uint32_t k_IoDeleteSymbolicLink(X86Ctx *c, const uint32_t *a) {
    char link[64];
    ansi_read(a[0], link, sizeof link);
    for (int i = 0; i < MAX_LINKS; i++)
        if (!_stricmp(g_links[i].link, link)) g_links[i].link[0] = 0;
    return STATUS_SUCCESS;
}
static uint32_t k_NtOpenSymbolicLinkObject(X86Ctx *c, const uint32_t *a) {
    /* (*LinkHandle, ObjectAttributes): handle = 1-based link index tagged with 0x4C000000 */
    char name[GPATH];
    guest_path(a[1], name);
    for (int i = 0; i < MAX_LINKS; i++)
        if (g_links[i].link[0] && !_stricmp(g_links[i].link, name)) {
            M32(a[0]) = 0x4C000000u | (i + 1);
            return STATUS_SUCCESS;
        }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
static uint32_t k_NtQuerySymbolicLinkObject(X86Ctx *c, const uint32_t *a) {
    /* (LinkHandle, LinkTarget (ANSI_STRING*), *ReturnedLength) */
    uint32_t i = (a[0] & 0xFFFF) - 1;
    if ((a[0] & 0xFF000000u) != 0x4C000000u || i >= MAX_LINKS) return STATUS_INVALID_HANDLE;
    uint16_t len = (uint16_t)strlen(g_links[i].target);
    if (M16(a[1] + 2) < len) return 0xC0000023u; /* STATUS_BUFFER_TOO_SMALL */
    memcpy(g_mem + M32(a[1] + 4), g_links[i].target, len);
    M16(a[1]) = len;
    if (a[2]) M32(a[2]) = len;
    return STATUS_SUCCESS;
}

#define K(name) {#name, k_##name}
const KImpl g_file_impls[] = {
    K(NtCreateFile), K(NtOpenFile), K(NtReadFile), K(NtWriteFile), K(NtFlushBuffersFile),
    K(NtQueryInformationFile), K(NtSetInformationFile), K(NtQueryFullAttributesFile),
    K(NtQueryVolumeInformationFile), K(NtQueryDirectoryFile), K(NtDeleteFile), K(NtDeviceIoControlFile), K(NtFsControlFile), K(IoQueryVolumeInformation),
    K(IoCreateSymbolicLink), K(IoDeleteSymbolicLink), K(NtOpenSymbolicLinkObject), K(NtQuerySymbolicLinkObject),
    {NULL, NULL},
};
