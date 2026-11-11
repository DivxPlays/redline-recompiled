/* Achievements (new in this port; Forza Motorsport had none): 32 of them, earned from what the game does, shown as a
 * pop-up (render.c achieve_toast) and listed by the main menu's ACHIEVEMENTS (hle_forza.c). Kept per game profile
 * in hdd\achievements\<profile name>.txt (under a test script, build\achievements_test\, so a script's AI races
 * never earn the player's). What they read (found 2026-11-05, ULTRAWIDE off, by memory dumps against the screen):
 *   the current profile: [0x5902C4] (a counted pointer, CPlayerProfile vtable 0x49235C): +0x250 name (UTF-16),
 *     +0x358 cars owned, +0x3D4 credits, +0x3DC driver level, +0x3F4 distance driven (metres, float);
 *   a car (hle_forza.c 0xB0A50 hook; the player's has a CHumanDriver at +0x40): +0xBC place (from 1), +0x40C
 *     position, +0x458 velocity (m/s; its length is the speedometer's within 1%);
 *   screens on top (UI manager, hle_forza.c): results, gifts, purchases, decals, saved replays, the menus' flow;
 *   files the game opens (file.c): a track's .xb2 (which track loads), a Drivatar's DrivatarProfile.xml (+0x75, a
 *     bit per lesson passed; its name at 0 is the profile's), a tuning setup (*.cht) written. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox.h"

enum {
    A_FIRST_RACE, A_WIN, A_PODIUMS, A_WINS25, A_WINS100, A_COMEBACK, A_NIGHT,
    A_LEVEL5, A_LEVEL10, A_LEVEL25, A_LEVEL50, A_GIFT, A_BUY, A_CARS10, A_CARS50, A_MILLION, A_GLOBE,
    A_UPGRADE, A_PAINT, A_DECAL, A_SETUP,
    A_TRIAL, A_LESSON, A_LESSONS, A_SPLIT, A_REPLAY,
    A_TRACKS, A_CITY, A_NURB, A_100MPH, A_200MPH, A_MILES,
    A_COUNT
};
static const struct {
    const char *name, *desc;
} g_ach[A_COUNT] = {
    {"Lights Out", "Finish your first race."},
    {"Chequered Flag", "Win a race."},
    {"Podium Regular", "Finish on the podium 10 times."},
    {"Serial Winner", "Win 25 races."},
    {"Dominator", "Win 100 races."},
    {"Comeback Kid", "Win a race after starting last on the grid."},
    {"Night Owl", "Win a race on Tokyo."},
    {"On the Ladder", "Reach driver level 5."},
    {"Pro License", "Reach driver level 10."},
    {"Veteran", "Reach driver level 25."},
    {"Legend", "Reach driver level 50."},
    {"Gift Horse", "Win a reward car."},
    {"Showroom Fresh", "Buy a car."},
    {"Collector", "Own 10 cars."},
    {"Museum Curator", "Own 50 cars."},
    {"High Roller", "Have 1,000,000 CR."},
    {"Globetrotter", "Win a race in North America, Europe and Asia."},
    {"Wrench Turner", "Install an upgrade."},
    {"Fresh Coat", "Save a custom paint job."},
    {"Vinyl Artist", "Apply a decal layer."},
    {"Setup Sheet", "Save a tuning setup."},
    {"Against the Clock", "Finish a Time Trial."},
    {"Student Teacher", "Complete a Drivatar lesson."},
    {"Graduation Day", "Complete every Drivatar lesson."},
    {"Couch Rivals", "Finish a split-screen race."},
    {"Director's Cut", "Save a replay."},
    {"World Tour", "Race on every track."},
    {"City Slicker", "Win on New York, Tokyo and Rio."},
    {"Green Hell", "Finish a race on the Nurburgring."},
    {"Ton Up", "Reach 100 mph."},
    {"Double Ton", "Reach 200 mph."},
    {"Long Haul", "Drive 1,000 miles in total."},
};

/* tracks by location (folder names under media\tracks, their layouts folded together): bit, region (0 North America,
 * 1 Europe, 2 Asia, -1 neither); World Tour is every location of Free Run's list */
static const struct {
    const char *prefix;
    int bit, region;
} g_tracks[] = {
    {"FarmersLip", 0, 0},  {"Tsukuba", 1, 2},    {"Silverstone", 2, 1}, {"RoadAtlanta", 3, 0}, {"NewYork", 4, 0},
    {"AlpineYodel", 5, 1}, {"Lagunaseca", 6, 0}, {"Tokyo", 7, 2},       {"SunsetSpeedway", 8, 0}, {"Roaval", 9, 0},
    {"Figjam", 10, 0},     {"Rio", 11, -1},      {"RoadAmerica", 12, 0}, {"Nurburgring", 13, 1}, {"TestTrack", 14, -1},
    {"TestOval", 15, -1},  {"JapaneseM", 16, 2}, {"Vancouver", 17, 0},
};
#define TOUR_MASK 0xFFFFu /* the sixteen locations Free Run lists from the start */
enum { CITY_NY = 1, CITY_TOKYO = 2, CITY_RIO = 4 };

static struct {
    char profile[64];       /* "" until a profile is loaded */
    uint64_t earned;
    int wins, podiums;
    uint32_t tracks, regions, cities; /* raced on, won in, won on */
} g;

static CRITICAL_SECTION g_toast_lock;
static int g_toast_q[A_COUNT], g_toast_n; /* earned, not yet shown */

static int test_mode(void) { return getenv("TESTKEYS") != NULL; }

static void file_for(char *out, size_t n) {
    char dir[MAX_PATH];
    if (test_mode()) snprintf(dir, sizeof dir, "build\\achievements_test");
    else { /* beside the game's own saves (file.c): the launcher starts us in build\, so not relative to that */
        snprintf(dir, sizeof dir, "%s\\..\\hdd", guest_path_root());
        CreateDirectoryA(dir, NULL);
        strncat(dir, "\\achievements", sizeof dir - strlen(dir) - 1);
    }
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s\\%s.txt", dir, g.profile);
}

static void save(void) {
    if (!g.profile[0]) return;
    char p[MAX_PATH];
    file_for(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) {
        fprintf(stderr, "[achieve] cannot save %s\n", p);
        return;
    }
    fprintf(f, "earned=%llx\nwins=%d\npodiums=%d\ntracks=%x\nregions=%x\ncities=%x\n", (unsigned long long)g.earned,
            g.wins, g.podiums, g.tracks, g.regions, g.cities);
    fclose(f);
}

static void load(const char *profile) {
    memset(&g, 0, sizeof g);
    snprintf(g.profile, sizeof g.profile, "%s", profile);
    char p[MAX_PATH], line[128];
    file_for(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "[achieve] profile %s: new\n", g.profile);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "earned=%llx", &v) == 1) g.earned = v;
        sscanf(line, "wins=%d", &g.wins), sscanf(line, "podiums=%d", &g.podiums);
        sscanf(line, "tracks=%x", &g.tracks), sscanf(line, "regions=%x", &g.regions), sscanf(line, "cities=%x", &g.cities);
    }
    fclose(f);
    fprintf(stderr, "[achieve] profile %s: %d of %d earned\n", g.profile, achieve_earned(), A_COUNT);
}

static void unlock(int a) {
    if (!g.profile[0] || (g.earned >> a & 1)) return;
    g.earned |= 1ull << a;
    save();
    fprintf(stderr, "[achieve] earned %s\n", g_ach[a].name);
    EnterCriticalSection(&g_toast_lock);
    if (g_toast_n < A_COUNT) g_toast_q[g_toast_n++] = a;
    LeaveCriticalSection(&g_toast_lock);
}

/* ---------------- the current profile, polled ---------------- */

static uint32_t profile_ptr(void) {
    uint32_t p = M32(0x5902C4u);
    return p && M32(p) == 0x49235Cu ? p : 0;
}

/* the Drivatar save named as the profile: its lessons, a bit each (DrivatarProfile.xml +0x75) */
static int lessons_passed(void) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\..\\hdd\\Partition1\\UDATA\\4d53006e\\*", guest_path_root());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int best = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char p[MAX_PATH];
        uint8_t b[0x78];
        snprintf(p, sizeof p, "%s\\..\\hdd\\Partition1\\UDATA\\4d53006e\\%s\\DrivatarProfile.xml", guest_path_root(), fd.cFileName);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        size_t got = fread(b, 1, sizeof b, f);
        fclose(f);
        char name[33] = "";
        for (int i = 0; i < 32 && b[2 * i]; i++) name[i] = (char)b[2 * i], name[i + 1] = 0;
        if (got == sizeof b && !strcmp(name, g.profile)) {
            int n = 0;
            for (int k = 0; k < 5; k++) n += b[0x75] >> k & 1;
            best = n > best ? n : best;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    fprintf(stderr, "[achieve] Drivatar lessons passed: %d\n", best);
    return best;
}

static uint32_t g_cars_seen = ~0u, g_credits_seen; /* as last polled: a car or credits gone tells a purchase */
static ULONGLONG g_gift_at, g_purchase_at, g_lessons_due;

void achieve_poll(void) {
    static int n;
    if (++n % 30) return; /* about twice a second at 60 */
    uint32_t p = profile_ptr();
    if (!p) return;
    char name[33] = "";
    for (int i = 0; i < 32 && M16(p + 0x250 + 2 * i); i++) {
        uint16_t ch = M16(p + 0x250 + 2 * i);
        name[i] = ch < 128 && ch != '\\' && ch != '/' && ch != ':' && ch != '*' && ch != '?' ? (char)ch : '_';
        name[i + 1] = 0;
    }
    if (!name[0]) return;
    if (strcmp(name, g.profile)) {
        load(name);
        g_cars_seen = ~0u, g_lessons_due = 1;
    }
    uint32_t cars = M32(p + 0x358), credits = M32(p + 0x3D4), level = M32(p + 0x3DC);
    float metres = MF32(p + 0x3F4);
    if (level >= 5) unlock(A_LEVEL5);
    if (level >= 10) unlock(A_LEVEL10);
    if (level >= 25) unlock(A_LEVEL25);
    if (level >= 50) unlock(A_LEVEL50);
    if (cars >= 10 && cars < 10000) unlock(A_CARS10);
    if (cars >= 50 && cars < 10000) unlock(A_CARS50);
    if (credits >= 1000000 && credits < 1000000000) unlock(A_MILLION);
    if (metres >= 1609344.0f && metres < 1e12f) unlock(A_MILES);
    ULONGLONG now = GetTickCount64();
    if (g_cars_seen != ~0u && cars > g_cars_seen && cars < 10000) /* one more car: a gift if one was just shown */
        unlock(now - g_gift_at < 60000 ? A_GIFT : A_BUY);
    if (g_cars_seen != ~0u && cars == g_cars_seen && credits < g_credits_seen && now - g_purchase_at < 30000)
        unlock(A_UPGRADE); /* credits spent after a purchase screen, no car added: a part */
    g_cars_seen = cars, g_credits_seen = credits;
    if (getenv("ACHTEST") && !(g.earned >> (atoi(getenv("ACHTEST")) % A_COUNT) & 1))
        unlock(atoi(getenv("ACHTEST")) % A_COUNT); /* testing: earn achievement n */
    if (g_lessons_due && now >= g_lessons_due) {
        int k = lessons_passed();
        if (k >= 1) unlock(A_LESSON);
        if (k >= 5) unlock(A_LESSONS);
        g_lessons_due = 0;
    }
}

/* ---------------- races ---------------- */

static int g_track = -1;          /* the location loading or raced on */
static int g_flow;                /* how the race was reached, from the screen after the main menu */
enum { FLOW_OTHER, FLOW_ARCADE, FLOW_CAREER, FLOW_MULTI, FLOW_FREERUN, FLOW_DRIVATAR };
static uint32_t g_cars[16];       /* the race's cars, as their drivers run */
static int g_ncars, g_humans, g_start_place, g_racing;
static uint32_t g_player;         /* player 1's car */
static double g_top_mps; /* mph, for the log */

/* file.c: a file the game opens (guest path), and whether for writing */
void achieve_file(const char *path, int writing) {
    const char *t = strstr(path, "\\tracks\\");
    size_t len = strlen(path);
    if (t && len > 4 && !_stricmp(path + len - 4, ".xb2")) {
        t += 8;
        for (int i = 0; i < (int)(sizeof g_tracks / sizeof g_tracks[0]); i++)
            if (!_strnicmp(t, g_tracks[i].prefix, strlen(g_tracks[i].prefix))) g_track = i;
    }
    if (writing && len > 4 && !_stricmp(path + len - 4, ".cht")) unlock(A_SETUP);
    if (writing && len > 19 && !_stricmp(path + len - 19, "DrivatarProfile.xml"))
        g_lessons_due = GetTickCount64() + 3000; /* read once the save is written */
}

/* hle_forza.c 0xB0A50: a car's drivers are about to run; human: a player's car */
void achieve_car(uint32_t car, int human) {
    int k = 0;
    while (k < g_ncars && g_cars[k] != car) k++;
    if (k == g_ncars && g_ncars < 16) {
        g_cars[g_ncars++] = car;
        if (human) {
            if (!g_humans++) g_player = car;
        }
    }
    if (!human || !g_racing || car != g_player) return;
    if (!g_start_place) g_start_place = (int)M32(car + 0xBC); /* places are set once the race runs */
    float vx = MF32(car + 0x458), vy = MF32(car + 0x45C), vz = MF32(car + 0x460);
    double mph = sqrt((double)vx * vx + (double)vy * vy + (double)vz * vz) * 2.23694;
    ULONGLONG now = GetTickCount64();
    static ULONGLONG over100, over200; /* held for half a second: not a bump's spike */
    over100 = mph >= 100 ? (over100 ? over100 : now) : 0, over200 = mph >= 200 ? (over200 ? over200 : now) : 0;
    if (mph < 450 && mph > g_top_mps) g_top_mps = mph;
    if (over100 && now - over100 >= 500) unlock(A_100MPH);
    if (over200 && now - over200 >= 500) unlock(A_200MPH);
}

static void race_finished(void) {
    if (!g_player) return;
    int place = (int)M32(g_player + 0xBC);
    fprintf(stderr, "[achieve] race over: place %d of %d, started %d, %d human, track %s, flow %d, top %.0f mph\n",
            place, g_ncars, g_start_place, g_humans, g_track >= 0 ? g_tracks[g_track].prefix : "?", g_flow,
            g_top_mps);
    if (place < 1 || place > 16) return;
    unlock(A_FIRST_RACE);
    if (g_humans >= 2) unlock(A_SPLIT);
    if (g_ncars == 1 && g_flow != FLOW_DRIVATAR) unlock(A_TRIAL); /* alone on the track: a Time Trial */
    if (g_track >= 0 && !strcmp(g_tracks[g_track].prefix, "Nurburgring")) unlock(A_NURB);
    if (g_ncars < 2) {
        save();
        return;
    }
    if (place <= 3 && ++g.podiums >= 10) unlock(A_PODIUMS);
    if (place == 1) {
        g.wins++;
        unlock(A_WIN);
        if (g.wins >= 25) unlock(A_WINS25);
        if (g.wins >= 100) unlock(A_WINS100);
        if (g_start_place == g_ncars && g_ncars >= 4) unlock(A_COMEBACK);
        if (g_track >= 0) {
            const char *t = g_tracks[g_track].prefix;
            if (!strcmp(t, "Tokyo")) unlock(A_NIGHT), g.cities |= CITY_TOKYO;
            if (!strcmp(t, "NewYork")) g.cities |= CITY_NY;
            if (!strcmp(t, "Rio")) g.cities |= CITY_RIO;
            if (g_tracks[g_track].region >= 0) g.regions |= 1u << g_tracks[g_track].region;
        }
        if (g.cities == (CITY_NY | CITY_TOKYO | CITY_RIO)) unlock(A_CITY);
        if (g.regions == 7) unlock(A_GLOBE);
    }
    save();
}

/* hle_forza.c: the screen on top changed (its class, from RTTI) */
void achieve_screen(const char *cls) {
    static char last[64];
    if (!strcmp(cls, "CUIArcadeMenu") && !strcmp(last, "CUIMainMenu")) g_flow = FLOW_ARCADE;
    else if (!strcmp(cls, "CUICareerMenu") && !strcmp(last, "CUIMainMenu")) g_flow = FLOW_CAREER;
    else if (!strcmp(cls, "CUIMultiplayerMenu") && !strcmp(last, "CUIMainMenu")) g_flow = FLOW_MULTI;
    else if (strstr(cls, "FreeRun") && !strcmp(last, "CUIMainMenu")) g_flow = FLOW_FREERUN;
    else if (strstr(cls, "Drivatar")) g_flow = FLOW_DRIVATAR;
    else if (!strcmp(last, "CUIMainMenu") && strcmp(cls, "CUIPlayerStats")) g_flow = FLOW_OTHER;
    if (!strcmp(cls, "CUILoadGame")) g_ncars = g_humans = 0, g_player = 0, g_racing = 0, g_top_mps = 0, g_start_place = 0;
    if (!strcmp(cls, "CCountDownWindow")) {
        g_racing = 1;
        g_start_place = 0;
        if (g_track >= 0 && g_tracks[g_track].bit < 16) {
            g.tracks |= 1u << g_tracks[g_track].bit;
            if ((g.tracks & TOUR_MASK) == TOUR_MASK) unlock(A_TRACKS);
            save();
        }
    }
    if (!strcmp(cls, "CRaceResultsWindow") && g_racing) race_finished(), g_racing = 0;
    if (!strcmp(cls, "CReplayOverlay") || !strcmp(cls, "CUIMainMenu")) g_racing = 0; /* a replay's cars: not a race */
    if (!strcmp(cls, "CUICarGift") || !strcmp(cls, "CUICarUnlockPopup")) g_gift_at = GetTickCount64();
    if (!strcmp(cls, "LayerColorScreen")) unlock(A_DECAL); /* Apply Decals: a vinyl chosen, on the car, its colour next */
    if (!strcmp(cls, "CSavingReplayPopup")) unlock(A_REPLAY);
    if (!strcmp(cls, "CUIConfirmPurchase") || !strcmp(cls, "UpgradeInstallInfoOverlay")) g_purchase_at = GetTickCount64();
    snprintf(last, sizeof last, "%s", cls);
}

/* hle_forza.c: the CPU read back a render target (Lock2DSurface): saving a paint job draws the livery at 1024 */
void achieve_readback(uint32_t width, uint32_t height) {
    if (width == 1024 && height == 1024) unlock(A_PAINT);
}

/* ---------------- the list and the pop-up ---------------- */

int achieve_count(void) { return A_COUNT; }
int achieve_earned(void) {
    int n = 0;
    for (int i = 0; i < A_COUNT; i++) n += (int)(g.earned >> i & 1);
    return n;
}
int achieve_has(int i) { return i >= 0 && i < A_COUNT && (g.earned >> i & 1); }
const char *achieve_name(int i) { return i >= 0 && i < A_COUNT ? g_ach[i].name : ""; }
const char *achieve_desc(int i) { return i >= 0 && i < A_COUNT ? g_ach[i].desc : ""; }

/* render.c: the next achievement to show (-1: none) */
int achieve_toast_next(void) {
    int a = -1;
    EnterCriticalSection(&g_toast_lock);
    if (g_toast_n) {
        a = g_toast_q[0];
        memmove(g_toast_q, g_toast_q + 1, --g_toast_n * sizeof g_toast_q[0]);
    }
    LeaveCriticalSection(&g_toast_lock);
    return a;
}

void achieve_init(void) { InitializeCriticalSection(&g_toast_lock); }
