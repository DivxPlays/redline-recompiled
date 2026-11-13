# Forza Motorsport (Xbox, 2005) recomp — notes

## Layout
- `tools/xiso_extract.py` — XDVDFS extractor (`python tools/xiso_extract.py <iso> game`)
- `tools/xbe_info.py` — XBE header/section/library/kernel-import dump
- `tools/find_functions.py` — function discovery; `tools/why_missing.py` explains a missed address
- `tools/lift.py` — x86 -> C lifter; `tools/insn_stats.py` — instruction histogram
- `tools/gen_kernel_exports.py` — builds `runtime/kernel_exports.h` from nxdk's CC0 `third_party/nxdk/xboxkrnl.exe.def`
- `tools/oovpa_pp.py` + `tools/find_symbols.py` — names XDK library functions using XbSymbolDatabase signatures
  (`third_party/XbSymbolDatabase`, MIT code / ODbL data; only its tables are preprocessed, nothing of it runs)
- `tools/build.py` — incremental build (only stale objects recompile; the linker is incremental)
- `tools/texextract.py` — every texture on the disc into the texture dump; `tools/texpack.py` — 4x texture pack
  from the dump (Real-ESRGAN); `launcher/launcher.c` — the launcher
  (`python tools/build.py --launcher`); `third_party/SDL3` — gamepads (zlib licence)
- `README.md` — for players: what they need, install, controls, settings, the optional packs, credits
- `tools/package.py` — the release zip (`dist/RedlineRecompiled-<version>.zip`: git archive HEAD + the launcher;
  fails if the author's address or the home folder path is in any file)
- `runtime/hle.txt` — guest functions replaced by host code (`hle_<Name>` in `runtime/hle_*.c`)
- `runtime/` — host side: `main.c` loader, `kernel.c` kernel HLE + dispatch, `thread.c` threads/sync/DPCs/timers,
  `file.c` file system, `runtime.c` indirect dispatch, `x86.h` CPU state + helpers
- Never commit (copyrighted or generated): `game/`, `analysis/`, `recomp/`, `build/`, `hdd/`, the ISO/7z

## Disc
Plain XISO (no video partition), 1234 files, 3.57 GB.

| XBE | What | Notes |
|---|---|---|
| `Forza.xbe` | **the game** | title 4D53006E, 6 MB |
| `default.xbe` | "CDX" launcher (MS bonus-content shell) | boots first, launches Forza.xbe |
| `update.xbe`, `dashupdate.xbe` | MS dashboard/online updaters | ignore |
| `XBLA/Default.xbe` | XBLA demo launcher | ignore |

## Forza.xbe
- Retail, base 0x10000, image size 0x620480, entry **0x002693D1**, kernel thunk 0x00486300 (start of .rdata), TLS 0x4CEC54
- Build path: `c:\sd\Racing\Forza\Dev\src\xbox\Game\Rtx\ForzaRTX.exe`
- XDK **5849** for every library (the final XDK, best supported by existing HLE signature databases)
- Linked libs: XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XBOXKRNL, XACTENG, XVOICE, VOICMAIL, XONLINES, LIBCPMT, LIBCMT
- 159 kernel imports, 17 of them data exports
- Game code: `.text` (3.1 MB), `FZGINIT`, `FZFULLUI`. Everything else is statically linked XDK/middleware:
  D3D/D3DX/XGRPH (graphics), DSOUND/WMADEC/XACTENG/DOLBY (audio), XONLINE/XNET/XON_RD (Live),
  BINK* (Bink video), XPP (input), DDERR.

## Approach: full static recompilation (x86-32 -> C -> x64)

Pipeline:
```
python tools/xiso_extract.py <iso> game
python tools/find_functions.py game/Forza.xbe analysis/Forza.functions.json
python tools/oovpa_pp.py
python tools/find_symbols.py game/Forza.xbe build/oovpa third_party/nxdk/xboxkrnl.exe.def analysis/Forza.symbols.json
python tools/lift.py game/Forza.xbe analysis/Forza.functions.json recomp --hle runtime/hle.txt --symbols analysis/Forza.symbols.json
python tools/build.py      (Visual Studio, first build ~2 min, then incremental; --zig: the players' build, ~1.5 min)
build\forza.exe game       (KTRACE=1 traces every kernel call; KWATCH=<hex addr> reports writes to a dword)
```

HLE (host replacements): a function listed in `runtime/hle.txt` is lifted as a thunk `f_X -> hle_<Name>(c)`; the
original lifted body stays available as `o_X` so host code can wrap it. Host functions read stack args at
`[esp+4..]` and must pop them like the guest callee (stdcall/thiscall `ret n`).

Lifter design (`tools/lift.py`, `runtime/x86.h`):
- One C function per guest function: `void f_XXXXXXXX(X86Ctx *c)`; blocks become labels + `goto`.
- Guest memory = flat 4 GiB host reservation `g_mem`; `M32(addr)` etc.
- Guest stack is faithful: `call` pushes the real return address, `ret` pops it, so `jmp` tail calls just work.
- Flags are a function-local struct; the C optimiser deletes the flag work nobody reads.
- Indirect calls/jumps -> `x86_call_indirect` (binary search over `g_func_table`). Jump tables -> `switch`.
- x87 uses `double` (no 80-bit precision). SSE scalar/packed maps to C floats.
- Each function stores its address in `c->last_fn`, so crashes report the guest function plus a stack scan.

Discovery seeds, iterated to a fixpoint: entry point, call targets, code addresses used as `push`/`mov`
immediates (thread routines, callbacks), and code pointers in data — including pointer tables inside code
sections (FZGINIT's static-initializer list). Pointers into code already reached are treated as labels.
Jump tables: `cmp`-bounded forward tables, plus the CRT memcpy forms (slot 0 padding, negative index).

Runtime design:
- **Memory**: guest RAM (128 MiB) is one shared section mapped at its own addresses and again at 0x80000000,
  like the Xbox's physical-memory alias that drivers use for DMA; physical address == low guest address.
  The first MiB of the alias is separate and holds a stub kernel PE header at 0x80010000 (XAPI inspects it).
  Kernel-owned structures live at 0xB0000000. Game allocations come from a bump heap at 0x00800000.
- **Kernel calls**: thunk slots hold 0xFFFF0000+ordinal (functions) or the address of the data export.
  `kernel_call` unpacks stdcall/fastcall/cdecl arguments and pops them like the real callee.
- **Threads**: one host thread per guest thread, but a global lock lets only one run guest code at a time
  (lifted code is not thread-safe; the Xbox had one CPU). Blocking calls release the lock. Each thread has its
  own KPCR/KTHREAD/TLS (`fs:[4]` = TLS slot array, `fs:[0x20]` Prcb, `fs:[0x24]` IRQL, `fs:[0x28]` thread).
- **Sync objects** are real DISPATCHER_HEADERs in guest memory; handles are their addresses; waits poll.
- **DPCs/timers** run on a dedicated thread at DISPATCH_LEVEL. Interrupts are registered but never fire.
- **Files**: `\Device\CdRom0` -> `game/`, `\Device\Harddisk0\PartitionN` -> `hdd/PartitionN/`, with drive letters
  resolved through the title's own symbolic links. A bare partition root also gets a raw sector image
  (`hdd/PartitionN.raw`) for the cache-partition table and FATX formatting; volume info reads the formatted
  cluster size back from it.

### Status (2026-10-27)
- Discovery: 22,205 functions, 92.8% of code bytes reached, 456 jump tables.
- Lifting: 99.59% of 1.87M instructions translated. Gaps: MMX (mostly BINK/D3DX/DSOUND), privileged I/O.
- Boot: XAPI startup completes — main game thread, CRT static initializers, heaps, save-data folders
  (`UDATA/TDATA` under `hdd/Partition1`), `T:`/`U:` links, cache-partition table, formats and mounts `Z:` and `N:`,
  allocates the game's 32 MB memory pool and the framebuffers.
- Direct3D init completes against the stand-in NV2A; the game loads `D:\MEDIA\FrontEnd\EN\DisclaimerScrn.xpr`
  and renders its first screen in a 60 fps loop (see "Graphics: hybrid design").

### XDK symbols (find_symbols.py)
395 functions named: D3D8 117, DSOUND 201, XAPI 50, XACT 20, XGRAPHICS 7. Derived values agree with what boot
needed by hand (`_tls_index` 0x5E5208, `_tls_array` 4). `D3D_g_pDevice` = 0x3C8FC8; push-buffer write pointer at
0x3C8FD0, limit 0x3C8FD4. Not found: `Direct3D_CreateDevice` and some setters (inlined or other revisions).

### How Forza drives the GPU
- 1,945 calls from game code to 87 named D3D functions, plus 1,169 calls to 33 unnamed ones.
- No game code touches D3D device internals directly.
- But most render state goes straight into the NV2A push buffer: `0x3B7A80` = `D3DDevice_SetRenderState_Simple`
  (fastcall method/value -> push buffer) has 662 call sites; `SetVertexData2f/4f/Color` immediate mode 560;
  `BeginPush`/`EndPush` 16 (the game writes GPU commands itself); `LoadVertexShaderProgram` uploads NV2A microcode.
- So a renderer must understand NV2A push-buffer commands whichever route is taken.

### Graphics: hybrid design (decided 2026-10-27)
The lifted XDK D3D library runs unmodified and talks to a stand-in NV2A (`runtime/nv2a.c`):
- Code in the D3D section is lifted with range-checked accessors (`MR*`/`MW*`, `MMIO_SECTIONS` in lift.py), so
  register accesses at 0xFD000000 reach `mmio_read`/`mmio_write`; game code keeps plain memory access.
- Register behaviour: PMC/PCRTC/PGRAPH interrupt status is write-1-to-clear and the PMC summary is computed;
  the write-combine flush completes at once; PTIMER runs; `DMA_PUT` runs the push buffer and updates `DMA_GET`;
  `PCRTC_START` = the framebuffer to show.
- Push buffer interpreter: jumps/calls/returns, (non-)increasing methods. Implemented methods: semaphore release
  (D3D fences, through the DMA contexts recorded from `CMiniport_CreateCtxDmaObject`), flip queue
  (FLIP_* and `PGRAPH_SURFACE`/`PGRAPH_INCREMENT`; FLIP_STALL holds the FIFO until the driver flips), software
  methods (NO_OPERATION with an argument traps into PGRAPH_TRAPPED_*, raises a PGRAPH interrupt and holds the
  FIFO until the handler re-enables FIFO access), surface setup and colour clears.
- 60 Hz vblank from the DPC thread; GPU interrupts are level-triggered into D3D's own ISR/DPC.
- Guest lock is a fair ticket lock, and lifted code yields it at every backward branch (`SAFEPOINT`) so guest spin
  loops (e.g. D3D_BlockOnTime) cannot starve the DPC/interrupt thread.
- Host renderer `runtime/render.c` (D3D11): window, one host render target per NV2A colour surface at
  `RENDER_SCALE` (2x) resolution, colour clears, presentation on CRTC flips.
Result: the game runs its front-end loop at 60 fps (~1,630 flips in 27 s incl. boot) with triple buffering; the
window stays black until draws are translated. `HANGDUMP=<s>` prints per-thread function samples and the method
histogram (`nv2a_report`).

### Known shortcuts
- USB (OHCI) registers at 0xFED00000 are plain zeroed memory so the XDK USB driver finds no devices (main.c).
- Kernel objects/handles leak; waits poll every 1 ms; thread priorities are ignored (one fair guest lock).
- Named kernel objects ignore their names; interrupts never fire.
- Crypto (Xc*), networking, and most Io* driver calls are unimplemented (stop with their name when reached).
- Lock-prefixed ops are lifted non-atomically (safe only because of the global guest lock).
- SEH/C++ exception unwinding has no runtime support yet.

### Release checklist (0.1, as of 2026-11-04)
**At the next release (the owner, 2026-11-12, "not right now"):** the owner chooses a README design and a trailer
design before they are made: show the choices (/kit:design; Repo Trailers' `trailer.py styles redline` previews each
trailer style from the footage), and build the release's README and its new trailer in the ones picked.
Works: boot, intro, title, profiles, main menu, Arcade and Free Run races (Maple Valley, Silverstone, Tsukuba),
showroom, assists, Recomp Settings, pause/quit, videos, sound, saves.
- **Rendered cube maps** (found by the track sweep): the table of cube maps the game renders (car reflections) had 8
  slots and never gave one up; each track makes its own, so the 9th track of a session stopped the game ("too many
  rendered cube maps"). A full table now gives up the cube used longest ago; one at a new size is made again.
  With that, the sweep (build/sweep.ps1) ran all 20 Free Run tracks in one session: each loads in 13-15 s, drives,
  and quits back to the list.
- **Coverage.** New code paths can reach the last 0.06% of unlifted instructions, an unimplemented kernel call or
  SEH (no runtime support): each ends the game. Reached without a crash (2026-11-04), besides the track sweep:
  - Whole races, by AUTODRIVE=1 (hle_forza.c; testing only): the player's car is driven by its own AI, the second
    driver every player car gets (car+0x44) for after the finish, stepped inside the human driver's step (0x597E0)
    in the AI cars' mode, so that the human driver records its controls for the replay as in play.
    build/autorace.ps1 finishes the first Arcade race (2 laps of Silverstone, ~3 min) to the results;
    build/autoreplay.ps1 then plays View Replay. (The game's own debug switch for this, 0x588B0E, makes a
    CAutoHumanDriver whose AI wanders off the track.)
  - Replays: View Replay, Save Replay (REPLAY_<time>.frp in the profile), Options > Replay Theatre lists and plays
    it. A replay is the cars' start and each step's controls, played again: each record keeps the step it was
    recorded with (+8, 0xC2920; 0.033 s if none) and playback steps by those, catching up with real time
    (0xB3304), so the variable step above 30 fps is replayed exactly; two plays of one replay agree at 30 and 144.
    (An AUTODRIVE race's replay leaves its path after ~30 s, likely the AI changing more than the recorded
    controls; a race driven by hand is still to watch.) Ghosts: a hot lap's ghost drives lap 2 at 144 as at 30.
    Replays showed "Created" two days old: Forza formats its scratch partition N: (Partition4) at every start and
    re-copies its caches, and the host folder kept every file (LastReplay.FRP from the first race ever, 2 GB of
    copied tracks). A FATX superblock written to a cache partition (3-5) now empties its folder (file.c).
  - Career garage: buying an upgrade (Modified Engine Tuning, 2,800 CR: installed, D2 -> D1, Sell offered), every
    upgrade category, Customize Paint (and saving it), Apply Decals (vinyl layers), My Cars, Tune Car Setup;
    credits, class and paint survive a restart. Two faults found and fixed:
    - The vinyl shapes were missing: linear AY8 and A8 (0x1B, 0x1F) were not decoded. A8 is now white with alpha,
      as xemu has it (black before; no menu or race uses A8 otherwise), so a vinyl takes the colour chosen.
    - A painted car was near-black in races and My Cars, its icon black: saving paint renders the livery into
      512x512 and 1024x1024 A8R8G8B8 targets that the game reads (Lock2DSurface) and compresses to DXT5 itself, and
      the icon is 128x64 copied off the screen; only the host targets were drawn, so it saved zeros. A lock of a
      surface that is a host target now copies it back into guest memory first (render_readback, averaged down
      from the render scale; ~9 such locks a session, at screen changes).
  - The rest of Career (2026-11-04, build/menu_path.ps1 now also takes X, Y and H<n>: hold W n s):
    - Buy Cars: region, manufacturer, the car's showroom, Car Options > Buy, which refuses without the credits
      ("This car costs 20,000 CR. You need 19,800 CR more").
    - Train Drivatar: its introduction, naming one (the on-screen keyboard), the Drivatar menu, Lesson One (Tsukuba,
      Ford Focus) loads and drives (held W, a minute). AUTODRIVE cannot drive a lesson (hle_forza.c).
    - Set Difficulty; Profile to Profile ("No other player profiles found").
    - Tune Car Setup: every tab (tires to braking), a change (front pressure +3), Save (named on the keyboard;
      "Setup Saved"), Accept, Load Setup ("Setup Loaded"), Benchmark, Test Drive (Select Track, a lap of Maple
      Valley, the pause menu), Defaults. A change survives a restart (26 -> 29 -> ... -> 38 psi over five boots;
      back to 26 with Defaults and Accept, also after a restart).
    - A Career race driven to the end: Results, Race Winnings (3,046 CR), Level Progress (level 1), Level 1 Reward,
      the scoreboard save, a tip, the next event; then eight more back to back (AUTODRIVE), to level 5, whose
      reward is a car (CUICarGift, CUICarUnlockPopup). With 22,011 CR, Buy Cars bought the Camry Solara (20,000 CR,
      "buy and get in"): 2,011 CR left, the Camry the current car, and after a restart My Cars lists it with the
      gift and the Lexus IS300, at level 5 and 2,011 CR.
    - Time Trials: Time Trial 1 (Maple Valley, 1973 911 RS) to its results (0:55.575) and the scoreboard save;
      after a restart the scoreboard lists it and the race shows the split against it (the ghost itself, 3.5 s ahead
      of AUTODRIVE's lap, was not seen).
  Played by hand (2026-11-05, about two hours at 32:9, 144): Drivatar lessons and replays work.
- **Graceful failure** for what cannot work. Checked (2026-11-04, build/menu_path.ps1 follows a path of keys from the
  main menu, build/menu_paths.ps1 the risky ones from fresh boots): Multiplayer > System Link, Split Screen and
  Scoreboards do nothing when chosen (no network, one pad, no Live); Xbox Live > New Account asks to quit to the
  dashboard, and Yes calls HalReturnToFirmware, which now says in a box that there is no dashboard before closing
  (it closed silently).
- **Achievements** (requested for 0.2, 2026-11-06): runtime/achieve.c, 32 of them (docs/achievements.md), a pop-up
  (render.c achieve_toast: GDI into a texture, the emblem from the exe's icon, sliding down from the top for 4 s) and
  ACHIEVEMENTS: n OF 32 on the main menu (hle_forza.c: page 4; first in Recomp Settings, moved 2026-11-08 as the
  owner asked; an empty icon (0) for one not earned, the gauge (5) for one
  earned; SLOTS 256 for the menu's strings). Kept in hdd\achievements\<profile name>.txt (beside the game's hdd,
  <game>\..\hdd: until 2026-11-10 the path was relative, and the launcher starts the game in build\, so nothing was
  ever saved and each start re-earned the progress ones with pop-ups, as the owner reported), under TESTKEYS in
  build\achievements_test\ (test races auto-select the first profile, the player's). What they read, found by RAM
  dumps (build/ram.now) and car dumps against the screen:
  - the profile: [0x5902C4] -> CPlayerProfile (vtable 0x49235C): +0x250 name (UTF-16), +0x358 cars, +0x35C its
    CGarageImpl, +0x3D4 credits, +0x3D8 winnings, +0x3DC level, +0x3F0 time played (s, float), +0x3F4 distance (m);
  - a car (the 0xB0A50 hook; a player's has CHumanDriver 0x4902A4 at +0x40): +0xBC place (from 1, set once racing),
    +0x40C position, +0x458 velocity (m/s; its length matched the speedometer within 1% over 13 dumps);
  - a Drivatar's DrivatarProfile.xml (named as its profile at 0): +0x75 a bit per lesson passed (0x1F: all five);
  - screens: CRaceResultsWindow (a race over), CSavingReplayPopup, CUICarGift / CUICarUnlockPopup, Apply Decals'
    LayerColorScreen (a vinyl placed), UpgradeInstallInfoOverlay / CUIConfirmPurchase (then credits down, no car:
    an upgrade); the screen after the main menu tells the mode (Arcade, Career, Multiplayer, Free Run; Time Trials
    is CUIEventSelectWindow); a race alone on the track is a Time Trial;
  - files: a track's .xb2 (which location), a *.cht written (a tuning setup), a 1024x1024 read back (render.c: a
    paint job saved).
  Checked in the game: Lights Out, Chequered Flag and Couch Rivals (a split-screen win), Against the Clock, Ton Up,
  Director's Cut, Student Teacher and Graduation Day (the player's Drivatar has all five), Setup Sheet, Vinyl Artist,
  Fresh Coat, Wrench Turner; the place, track and mode in the log line "[achieve] race over". The player's saves
  were backed up before and restored after (the tests bought a part, saved a setup and a paint job).
- **Split screen** (requested for 0.2, 2026-11-05): works with a second controller (host pad 2 = port 1). Checked
  with TESTPAD2=1 (hle_xinput.c: port 1 reported, held by test keys + 4096: Enter/Space A, Esc B, arrows, Tab
  Start, W/S triggers, A/D steer): Multiplayer > Split Screen shows Player 1 Start / Join, port 1's A joins, Create
  Match (Circuit Race, Maple Valley Short, 3 laps), each player's class, car and assists
  (CUIFreeRunCarClassSelect, SelectCarView, CUIAssistsMenu), then a two-view race, each with its HUD; at 32:9 each
  view is a 64:9 strip in proportion (the split camera's 0x495CE0 is widened too) and the HUDs go to the edges.
  The keyboard is merged into port 0, so one pad and the keyboard are one player. INPUTLOG also logs XInputOpen.
  Menus repeat a held direction by frames: one press of 150 ms moves one item at 60 and 144, 300 ms two at 60 and
  three at 144 (build: test taps of 40-60 ms).
  Also reached without a crash: Time Trials to a race; Career from a new profile (home region, buying the Lexus
  IS300, the career menu, the level tip) to a North American Open race; the European and Asian Opens refuse the
  car, as they should.
- **Crash message.** Done (2026-11-04): every stop on an error goes through fatal_exit (main.c): exits with an
  error code, abort(), the crash handler and unhandled host exceptions show a box with the log's last report and
  where the log is (not under TESTKEYS).
- **No command prompt with the game** (reported). Done (2026-11-04): redline.exe is a Windows program
  (/SUBSYSTEM:WINDOWS, /ENTRY:mainCRTStartup); its output goes where it is sent (the launcher's build\redline.log,
  a script's file) or, started on its own, to redline.log beside the exe.
- **Long session.** Checked over the track sweep (build/memwatch.ps1 samples the game's memory every 30 s): private
  memory grew ~1.1 GB a track (0.6 -> 10 GB over 9 tracks). Each decoded-texture slot holds its texture pack
  replacement, a reference the pack's budget cannot drop, and with 8192 slots a session kept every texture of every
  track. A slot unused for 1200 frames now gives both up (texture_age, a few slots a frame): over 10 tracks memory
  levels off at ~6 GB from the 4th, handles ~900 and threads ~45-60 flat. The pack's budget is now a third of the
  GPU's own memory, 512 MB to 4 GB (4 GB on the RTX 4090, 673 MB on the Radeon iGPU; the log says which).
  Kernel objects are still never freed, but handles did not grow over these runs.
- **Lag far from the start line** (reported; never reached by the test scripts, which cannot steer). Done what can
  be without driving there (2026-11-04), to be confirmed in play:
  - The decoded-texture cache held 512 textures, searched in turn, and a full one gave up slots round-robin, in use
    or not: a track plus cars outgrows that as new scenery comes in, and every texture thrown out is decoded,
    uploaded and looked up in the texture pack again. Now 1024 sets x 8 ways by address (8192), the slot used longest
    ago giving way: 8 compares a lookup instead of up to 512.
  - Hitch lines, always on: a new game frame more than 50 ms after the last is logged ([hitch], at most one a second)
    with the screen on top and what happened in the gap: shaders compiled, textures decoded, render targets made,
    texture pack loads and drops and its queue. Any player's build\redline.log now says what a stutter was. FPSLOG
    lines add textures decoded; DECODELOG=1 names each decode; TEXBUDGET=<MB> shrinks the texture pack's budget.
  - Measured in an Arcade race at Silverstone (7 AI cars) at FRAME RATE 60: 35-40 new frames a second while the
    cars are close, ~52 once they draw away. Not the decoding (~250 a second of thirty-two 1x1 textures at
    0x9AE000-0x9AEF80 whose contents the game rewrites; a texture that changes again within a second is now made
    updatable once and updated in place, UpdateSubresource, instead of a new D3D11 texture each time): the
    profiler put a quarter of the game thread in vertex assembly (array_vertex, decode_attr, emit, assemble) and
    nearly half in the driver under render_draw. Two fixes (2026-11-04):
    - An indexed draw (ARRAY_ELEMENT16/32) decoded a whole vertex every time an index named it, about six times
      per vertex of a mesh. pgraph.c now decodes an index once per primitive (g_seen, by index, reset with a stamp)
      and the primitive's elements refer to vertices (g_ref); the arrays' formats and addresses are found once per
      primitive (arrays_setup), again only if they or the immediate attributes change.
    - Each draw discarded its vertex, index and constant buffers (Map WRITE_DISCARD), and the driver found each new
      memory every time. They are now appended to rings mapped NO_OVERWRITE (ring_put), the constants bound at an
      offset (VSSetConstantBuffers1, D3D 11.1; the old discarded buffer if the driver cannot).
    Now the race at 60 holds 60 new frames a second with the cars close; at 144, 67-94. At 144 the game thread is
    ~88% busy: about a quarter vertex decoding, a quarter copying to the GPU, the rest the game.
    - Raw vertex reads (2026-11-04). A race frame decoded ~240,000 vertices into 256-byte Vertex records (16 float4
      attributes), 61 MB uploaded a frame. The vertex shader now reads the arrays' own bytes from g_vring, a raw
      buffer (shaders.c vfetch decodes each attribute by its NV2A type and size, as pgraph.c decode_attr; Constants
      va/vk say where each is, or its constant value), so a primitive drawn from one set of arrays keeps only its
      elements on the CPU, and the bytes they span go up once per overlapping range (an interleaved buffer once).
      Immediate and inline vertices, a primitive that mixes them in or changes its arrays midway (decode_refs), and
      sparse elements (span > 4 x elements + 4096) are decoded as before and read the same way. CPU looks at
      vertices (ULTRAWIDE's 2D test, the motion vectors' key, frame dumps) go through pgraph_attr. Only the
      attributes a program reads are fetched: all sixteen inlined took a shader ~160 ms to compile (~50 ms now;
      shadercache keeps them). An Arcade race at 144 (AUTODRIVE, 8 cars, shaders cached): 94-119 new frames a
      second, from 64-93; the picture, menus and ULTRAWIDE unchanged.
    - The GPU thread (2026-11-04; GPUTHREAD=0 or recomp.ini gputhread=0 for the old way). The push buffer ran on
      the thread that wrote DMA_PUT, ~60% of the game thread at 144. Now nv2a.c runs it on a thread of its own:
      DMA_PUT records PUT and wakes it, GET (published after each command, and at the end of every run: the
      first kick has nothing to run, and D3D waited forever on a GET left at 0), the semaphores and the beta
      marker advance as it goes. It holds the GPU lock (gpu_enter, nests per thread), never the guest lock;
      presenting at the vblank, render_readback, render_overlay, the settings (anisotropy, FSR, texture pack),
      Bink's video_open/copied/ready, DMA objects and GET writes take it, and the thread lets it go between
      commands when one waits. Flip and software-method stalls are set and resumed under g_flip_lock, so a
      vblank's resume cannot slip between the check and the stall. Same race at 144: 111-128 new frames a second
      (from ~108), the game thread 32% busy (from 86%) and the GPU thread 76%; the intro video 33.34 ms a frame,
      worst gap 36 ms. Checked: boot, menus, Select Car, the grid, a race, readbacks, the intro, all 20 of the
      sweep's tracks (13-14 s loads), an AI race and its replay, Customize Paint saved (the livery's 512 and 1024
      targets read back, the car orange in My Cars and its icon), ULTRAWIDE at 21:9 and 32:9, GPUTHREAD=0.
      A burst of ARRAY_ELEMENT16/32 words is taken at once (2026-11-05: nv2a.c run hands it to
      pgraph_array_elements, which keeps the indices as array_vertex would once the arrays are set up). Measured
      while the RTX 4090 held the frame rate, it changed nothing (86 against 90 new frames a second); once that was
      fixed (Frame rate, below), at 32:9 where the GPU thread is the limit: 126.9 new frames a second on average
      over an AI race at 144 from 118.3 (median 131.5 from 120), push buffer time 3.74 ms from 4.79.
  - Disc reads (file.c rw) ran under the guest lock: a millisecond on this NVMe drive, but a hard drive would stall
    every thread. A read at an offset now leaves the lock while Windows reads, positioned (OVERLAPPED offset) so
    another thread's use of the handle cannot move it; at the end of the file it gives 0 bytes as before.
- **Cutscenes still lag** (reported). Mostly done (2026-11-04). VIDEOLOG=1 now times every frame the game copies and
  every frame shown (build/video_pace.ps1 prints how far apart they came; 33.3 ms is right for these 30 fps videos).
  Over the attract video and 15 s of the intro: 355 frames shown, ~80 gaps of 67-350 ms, before; ~700 and a handful
  after. Two faults:
  - The decoder seeked back to frames it had just decoded: it waits once AHEAD (4) frames past the one asked for,
    and took "more than RING / 2 (4) behind" for a jump, so every time it got ahead it went back, re-decoded (the
    ring held 99 100 101 99 100... while the game wanted 98) and the frame asked for was missing. A jump back is now
    further behind than AHEAD + 1.
  - The game's own conversion of each frame into its buffer (BinkCopyToBuffer: lifted MMX at 0x47C030) held the
    guest lock up to ~95 ms a frame. A frame the upscaled video already has (video_ready) skips it: the HLE returns
    as the stdcall would (7 arguments; the one caller ignores the result). The first frame into a buffer is still
    converted (its alpha and signature), and so is any the decoder has not reached.
  Left: during the intro the game thread is ~98% busy decoding the original Bink frames (lifted MMX; that decode
  also gives the audio, so it stays), and some copies still come 100-140 ms apart. Native replacements for Bink's
  hot loops would fix that. (GILLOG's "hold worst ... at" names where the lock was let go, not where the time went;
  a thread refused a yield for raised IRQL now says so.)
  - (2026-11-04) Those 100-140 ms gaps came from the profiler (build/prof.now suspends every thread to sample it):
    profiled, the intro had them every second or so; unprofiled (video_pace.ps1), 714 frames shown, 572 33.3 ms
    apart, 98 one vblank late (50 ms), 3 over 66 ms (one the video starting). The game thread now spends most of
    the intro in D3D's wait for the GPU (0x3BDAD0, polling the push buffer's GET), Bink's decode a few percent.
  - MMX is SSE2 now (x86.h mmx: each op its SSE2 instruction on the low quadword; SSE2's shifts take MMX's 64-bit
    count). The scalar lanes stay as runtime.c mmx_ref, and mmx_selftest compares every op with them on edge and
    random values at each start. On a decode-like chain of eight ops, 5.6x faster (946 -> 170 ms for 20 M rounds).
- **Flicker: models blink, car shadows and shadows vanish at a distance** (reported). Done (2026-11-04), to be
  confirmed in play: two faults in the race scene's depth.
  - W-buffering. The scene's draws (~930 of a race frame's depth-tested ones) set NV097_SET_CONTROL0 bit 16
    (Z_PERSPECTIVE_ENABLE), which render.c ignored: depth was z/w x 1/zmax in D24, whose step at 500 m is ~0.15 m.
    D3D scales w so the far plane is zmax (16777215): depth is now w x 1/zmax, linear and as fine far as near (the
    scene renders right with it, which a wrong scale would not). The vertex shader passes it (VOut.wd), the pixel
    shader writes it interpolated perspective-correct (the NV2A interpolates 1/w), as SV_DepthLessEqual (w is convex
    along a screen line, so the chord the rasterizer gives is never below it: early depth testing stays; the
    position input must then be noperspective centroid). psh_key carries the bit. WBUFFER=0 or creating
    build/wbuf.now goes back to z/w for comparison.
  - Polygon offset. 346 w-buffered draws use it: the road +4000, decals and the car's shadow and badges -2000. The
    rasterizer flipped the bias's sign for every draw (right only for the shadow-map pass, whose shadow test is
    likely inverted too), which pushed decals 6000 units behind the road: in z/w a world distance that grows with
    range, so shadows vanished beyond some distance. W-buffered draws take GL's sign, in the rasterizer and in the
    pixel shader (which writes depth, so it adds the offset itself: bias / zmax + factor x slope per guest pixel).
    The car's trunk badges and the shadow under it, missing with the flipped sign, now show.
  - The scale, measured (WSHOW=1 paints each w-buffered pixel's depth, red x256, green x16, blue x1): w beside the car
    is ~65,000-100,000 and the horizon reaches zmax, so ~20,000 units a metre, a far plane ~1 km, and the decals'
    -2000 a ~10 cm pull toward the viewer (enough to darken a bumper's lowest edge at a grazing angle; the game's own
    value). (The scene snapshot's depth is not the scene's: a probe of it read one value everywhere.)
  - Car shadows (reported: "shadows look weird under the car", on the track sweep's dusk oval): hard black slabs
    reaching far from the car. The race draws its cars' shadows with the stencil test (~1,000 draws a frame set
    NV097_SET_STENCIL_TEST_ENABLE), which render.c had never implemented. It now does: enable 0x032C, write mask
    0x0360, func 0x0364, ref 0x0368, read mask 0x036C, ops on fail / depth fail / pass 0x0370-0x0378 (GL's values;
    one set for both faces), in the depth-stencil state and its cache key. The shadow is now the car's shape,
    stretched by the low sun, and grey.
  - Shadow maps: the pixel shader compared one filtered depth sample with r/q (hard, stepped edges). As the NV2A's
    shadow buffer, each of the four nearest texels is compared and the results blended by where the sample falls,
    on the game's own texel grid (the map is rendered RENDER_SCALE times finer; texscale.z carries the scale), so an
    edge spreads over a texel of the game's map as on the Xbox.
  - The texture pack's pop (2026-11-04): a race asks for ~290 replacements, ~200 in its first frame and ~70 at the
    countdown; one worker loaded them one after another, so the originals showed first and the 4x pictures came in
    over half a second (TEXTIME=1 logs each load and each replacement that came after its original showed: 347 such
    in the Arcade race, ~20 frames late on average). Now a frame waits up to 100 ms in all (g_texwait_ms; TEXWAIT=<ms>)
    for replacements still loading, and half the cores (2-8; TEXWORKERS=<n>) load them. Same race: 34, under two
    frames late on average, nearly all at the countdown. The cost is the race's first frame, ~0.3 s longer (it
    already took ~0.6 s after the loading screen): the renderer asks for one texture at a time, so those loads run
    one after another, ~2.5 ms each, ~90% of it WIC's PNG decode (a faster decoder or a pack of DDS files would cut
    that). Mid-race, waiting made the first lap stutter: a whole Arcade race at 60 (FPSLOG, AUTODRIVE) had 87
    frames over 20 ms on lap 1 (50-115 ms each, a few new textures and up to 9 replacements in each) against 9 on
    lap 2. So a frame waits only in the first second after the screen on top changes (g_scene_frame: the race's
    first frame, the grid, the countdown); elsewhere the original shows until the replacement is ready, a frame or
    few. Lap 1 then: 8 frames over 20 ms, 59.9 new frames a second; pops in the race 73, 4.5 frames late on average,
    none at its start.
  - Lag far from the start line, checked over a whole race (2 laps of Silverstone, AUTODRIVE, FRAME RATE 60): 57-60
    new frames a second throughout, the second lap as smooth as the first; to confirm in a race driven by hand.
- **Frame rate.** Done (2026-11-04): FRAME RATE 30 / 60 / 120 / 144 draws real frames at real-time speed (see
  "Frame rate" below), and frame generation is gone. Above ~65 in a race needs the emulated GPU off the guest lock.
  Replays keep each step's length, so they play right at any rate; a hot lap's ghost behaves at 144 as at 30 (see
  Coverage).
  144 at the player's settings (2026-11-05: 1080i, render scale 4, DLSS, 5120x1440 monitor; an Arcade race with
  AUTODRIVE, FPSLOG, build/prof.now, GILLOG): ~78 new frames a second. No thread was busy (game 21%, GPU thread 58%);
  the DPC thread held the guest lock 35% of the time without running, inside render_present, whose Present waited
  2-3 ms on an RTX 4090 at 91-99% (nvidia-smi). Render scale 2 gave 115-130. The card drew, per frame:
  - The AA'd back buffer at 15360x4320: D3D draws the screen twice as wide for its anti-aliasing (1280 or 3840
    columns), and render scale multiplied that too. From scale 2 an AA'd screen target leaves that half out
    (render.c target(): 7680x4320 at 1080i x4, 1280 wide at 480p x2; everything reads a target's own width, as
    for ULTRAWIDE): ~99 new frames a second, Present ~1 ms.
  - DLAA at 7680x4320 (DLSS runs at the frame's size when that is past the window's), then one bilinear tap per
    window pixel, which used 4 of the ~9 pixels behind it. Now a frame at least twice the window each way skips the
    upscaler, its motion vectors and the jitter (g_supersampled), and every shrink averages all the pixels under
    each (downsample(): an n x n grid of taps, n the frame's pixels a window pixel spans, at most 8): 137-144 new
    frames a second, Present 0.06 ms, the picture as clean (fences, crowds, small text against the DLAA frame).
  At 32:9 (ULTRAWIDE) the wider view draws more and the GPU thread is the limit (88%, push buffer ~6.9 ms a frame):
  95-142. Two things help there (AI races at 144, 32:9, means over the race): the index bursts (GPU thread, above)
  118.3 -> 126.9; and no millisecond polls on the way back from the GPU, 119.0 -> 126.7 (worst second 50 -> 68):
  D3D makes room in the push buffer by a software method, the GPU thread stops on it and raises an interrupt, the
  DPC thread ran the driver's handler at its next millisecond and the game thread saw the handler's event at its
  next millisecond Sleep. Now the interrupt wakes the DPC thread (dpc_kick) and a wait on one kernel object sleeps on
  its signal word, which its setters wake (thread.c wake; several objects still poll). profile.c also reports each
  sampled thread's own inclusive list now. Checked with all of it: the sweep's 20 tracks (track 8 once lost a key
  press on the pause menu, the script's; fine run again), Select Car and a race at 480p x2 and at 1080i x4, 32:9.
  Then at 32:9 the GPU thread's own profile: 28% of it was the HUD placement (hud_side ran the vertex program for up
  to 512 vertices of every depth-test-off draw, the sky and glare too; ui_squeeze compared every vertex of big
  flat-coloured draws for its fill test). Now hud_side looks at 8 vertices spread over a draw and stops at the first
  if it is not 2D (w not 1 or 0); the fill test takes draws of up to 64 indices. GPU thread 88% -> 77%.
  Measuring trap: autorace.ps1's screens (SHOTEVERY 30: a 5120x1417 read back and a 21 MB BMP each, ~5 a second at
  144) made 15-140 ms presents, the race's only hitches; the frame-rate runs now pass -Every 0. Whole AI races at
  144, 1080i x4, DLSS chosen, no screens: 141.7 new frames a second at 32:9 and 141.6 at 16:9 (median 144 both);
  the seconds with a gap over 30 ms are all menus and loading (Select Car, the race loading, the countdown's 120
  textures). The hitch lines now say how long their textures took to decode.
- **Hardware.** Real pads (Xbox, PlayStation: buttons, rumble, hot-plug) still to try. Another GPU: this PC's AMD
  Radeon integrated GPU (RENDER_GPU=low picks the power-saving GPU) draws the race correctly (and the hot lap's
  ghost car: ghosts work), at ~25 new frames a second whatever the render scale (13-18 at 4x with the texture
  pack): draw-bound on a 2-compute-unit iGPU, not a fault; a real graphics card is what the port needs. Also:
  first-run defaults checked: render scale 2x, FSR 1, 60 fps, 8x texture filter (xbox.h DEF_*, the launcher's table).
  Surround on real speakers still to try.
- **A logo** (requested). Done (2026-11-04): the emblem, ours alone: a gauge's dial (135-405 degrees) whose last
  fifth is the redline, a needle swept from behind a bold italic R (Segoe UI Black Italic) into it; below 48 px the
  dial and a red hub without the R. tools/make_icon.py draws launcher/redline.ico (16-256) and launcher/emblem.png;
  launcher/redline.rc puts it in both RedlineLauncher.exe and redline.exe (build.py: rc, build/redline.res), and both
  windows' classes use it. The launcher draws the same emblem with Direct2D (emblem()): large and faint (9%) in the
  background in place of the old tachometer, small beside the wordmark. The launcher's summary line now looks its
  settings up by key (adding Frame rate had shifted them: "Textures 480p") and shows the frame rate.
- Texture pack and upscaled videos: an advanced extra in the README (done, 2026-11-05).
- **Package** (2026-11-05): tools/package.py -> dist/RedlineRecompiled-0.1.0.zip (2.8 MB, 348 files). Checked as a
  player: unzipped into an empty folder, launcher.ini pointed at the ISO, Build: extracted, analysed, lifted and
  built in ~3.5 min on this PC; Play started the intro. Not exercised: the SDK download (third_party/fsr and dlss
  were copied in), a PC without Python or the build tools (the launcher's messages for those), another disc.
  The port's own code is MIT (LICENSE, "the Redline Recompiled contributors"; third_party/ keeps its licences).
Not for 0.1: Xbox Live (never), System Link, DES.

### To do for 0.2 (requested 2026-11-08; not started)
- **Achievement pop-up like the Xbox 360's**, in Redline Recompiled's style: the 360 toast's shape and motion (a
  rounded bar sliding in, the icon in a circle on the left, "Achievement unlocked" over the name), our colours and
  emblem, and an unlock sound of our own (made for the port, not the 360's). Today: render.c achieve_toast.
  **Done 2026-11-12** (not released): achieve_toast draws it with a pixel shader over one GDI texture (the emblem in
  the circle's place, "Achievement unlocked" and the name beside it): at the bottom middle, a red ring pops up (0.25 s,
  a little past full size), a dark pill opens out of it (0.2-0.55 s), the text fades in; from 4.3 s backwards; 5 s
  each, one at a time; as wide as the text, 1/12 of the screen's height. The sound, after the owner turned down a
  synthesized chime and 130 CC0 sounds from Kenney's packs: the game's own FanFare (media\Audio\UISounds.xwb, entry
  31, Xbox ADPCM 44.1 kHz stereo), read from the player's disc at the first pop-up (audio.c chime_make; nothing of
  the game ships): its first 3.5 s, faded out over the last 0.8 s, x0.55 (-6 dBFS peak), mixed into the front pair.
  Checked: a 20 s recording has it at every pop-up (correlation 1.0 with the approved cut). The wave bank format (v3:
  regions, 24-byte entries, 64-byte names) is read in audio.c; ACHTEST=n earns achievement n in the test file
  (build\achievements_test), for trying it.
- **Players get errors building the game** (reported by many). Collect their launcher logs and fix the causes;
  the next item would remove most of them.
- **Simpler requirements, like other recomps:** no Visual Studio build tools or Python to play; those only for
  making the texture pack. Decided (owner, 2026-11-08): **nothing of the game ever ships** (no prebuilt exe with
  the translated code: the owners could take the project down), so the player's PC still translates and compiles
  it, with tools that come with the download instead of installs. Direction: ship our own runtime prebuilt (an exe
  with no game code) and build only the game code, into a DLL it loads (plain C, no C runtime, calling the exe's
  exports), with a portable compiler and linker (e.g. clang + lld, or zig cc; fetched pinned and checksummed like
  tools/fetch_sdks.py, or bundled) and the Python tools frozen into an exe (lift, analysis; capstone inside).
  **Chosen 2026-11-10** (the owner: no release until a fresh PC builds and plays with nothing installed by hand):
  1. **Done (3a786a7):** build/redline.exe is the runtime only (1.75 MB, no game code; /MT, so no VC++
     redistributable); the lifted code is build/forza_recomp.dll (x86.h RT_API / GAME_API). The DLL imports the
     runtime's 13 helpers, g_gil_waiters and the 97 hle_ functions (build/redline.def from hle.txt; redline.lib, 30 KB,
     its import library) and exports g_func_table, g_func_count, its own g_mem (set once by runtime.c game_load) and
     the o_ originals the hle code calls (9, resolved by name). Checked in a race.
  2. **The player's build needs only:** capstone (find_functions, lift; Pillow and numpy are for the texture and
     video tools, not the build), a C preprocessor (oovpa_pp.bat: cl /EP) and a compiler and linker for the DLL.
     Plan: Python 3.12.10's embeddable zip with capstone 5.0.7's wheel unpacked beside it (the versions here, so the
     lifted code is the same), and zig 0.14.1 (clang 19, lld, mingw-w64 headers, the OS's UCRT: no VC++ runtime) for
     `zig cc -E` and the DLL, which links redline.lib. Downloads waiting for the owner's OK (2026-11-10): zig
     82.2 MB (ziglang.org), Python 11.1 MB (python.org), capstone 1.3 MB (PyPI). To measure first: clang's time and
     memory on recomp/ (MSVC: 3.1 GB at 16 files at once, ~2 min here); MSVC-only things in the lifted code.
  3. **Corrected by the owner (2026-11-10): only the launcher (and the source) is uploaded, never a built exe, not
     even the runtime without game code.** So the player's PC builds the runtime too, and the launcher fetches the
     tools at build time (pinned and checksummed from their official sources, as fetch_sdks.py does), not the zip.
     The exe / DLL split (step 1) is no longer needed for that; it stays unless it gets in the way (the runtime and
     the game code build and fail separately).
  4. **Done (a1ee1ef, 1a3ecea), 2026-11-10: the whole build runs with nothing installed.** zig compiles all of it:
     recomp/ in 53 s here (8 at a time; funcs_030.c 24 s and 1.4 GB, funcs_039.c 0.8 GB), the runtime in 3 s, all
     25 runtime files unchanged but for: THREAD_LOCAL (xbox.h; clang's mingw target ignores __declspec(thread), which
     made t_ctx and the other per-thread variables shared: a crash in gil_yield) and runtime/msvc_shim.c, the 12 MSVC
     runtime pieces NGX uses (operator new / delete and two STL errors under their MSVC names, the /GS cookie and its
     unwind handlers, thread-safe statics, _fltused), with empty stand-ins for the libraries NGX's .drectve asks for
     (LIBCMT, libcpmt, OLDNAMES, uuid). DLSS works (a race with "[dlss] ready"). The exe needs only Windows' DLLs,
     the Universal CRT and SDL3; onecore and synchronization by the API sets (api-ms-win-core-memory-l1-1-6,
     -synch-l1-2-0); zig rc builds the icon; zig dlltool makes redline.lib from redline.def. build.py --zig builds both
     in ~1.5 min (with PDBs: crash reports name the guest instruction). oovpa_pp.py (zig's preprocessor, else cl /EP)
     gives find_symbols.py the same 395 symbols; lift.py with the fetched Python writes byte-identical recomp/.
     The launcher fetches Python 3.12.10 (embeddable, writing its ._pth: its own folder for capstone, ../../../tools),
     capstone 5.0.7 and zig 0.14.1 into build\tools (pinned SHA-256; URLDownloadToFile, Windows' own tar.exe), and
     stops if the port's folder path is over 120 characters (zig's deepest file is 114 below build\tools\zig-0.14.1).
     Checked from the release zip in an empty folder: the launcher fetched the tools and built in 2 min (zig's cache
     warm), and the game reached the main menu.
  5. **Fresh-PC test: PASS (2026-11-10).** Windows Sandbox (no Python, no Visual Studio, nothing installed), the
     release folder plus a driver script (the launcher's file dialog given C:\Map\Forza.iso, Build, then Play): the
     launcher fetched its tools, built in 7 min (zig's cache cold, 2 compiles at a time in the sandbox's memory), and
     the game ran, still showing its attract movies 33 min later; no error windows, no crash, Defender clean.
     Left: Cancel cannot stop a download in progress (URLDownloadToFile blocks); fetch_sdks.py under the fetched
     Python not yet run (the DLSS and FSR files were copied in for the tests, to download nothing but the three
     approved tools). Visual Studio stays the development build (python tools/build.py).
- **A how-to for making the texture pack** (tools/texextract.py, tools/texpack.py, Real-ESRGAN; and the videos).
  **The textures, done another way (2026-11-12, not released): Make 4x textures in the launcher** (the owner asked for
  it). Ready page, while build\textures\redline4x has no aliases.txt (texpack.py writes it last, once whole): a page
  saying what it does (time, ~12 GB, Vulkan, ~67 MB of tools), then a job like Build's (g_job 1, pack_thread): free
  space (12 GB, 2 when picking up), fetch_tools and then g_pack_tools through the same fetch() (pinned SHA-256:
  Real-ESRGAN ncnn v0.2.5.0's zip, which publishes none, checked against the copy this port's pack was made with;
  numpy 2.5.4 and Pillow 12.3.0, the versions the owner's own Python made the pack with, into the build's Python
  folder as capstone is), python tools\texextract.py <game> build\textures\dump, tools\texpack.py. Its bar is weighted
  2 / 8 / 90 and moved by "progress: a/b unit" lines the tools print, which run() keeps out of the log and shows as
  "320 of 902 files" (texpack.py streams Real-ESRGAN's -v "done" lines, so it moves inside a batch of 400). Cancel
  ends the job's process tree; both tools skip what they did, so "Carry on" picks up; texpack.py writes each texture
  under .tmp and renames it (a Cancel while writing had left nothing half-written by luck) and clears .tmp files at its
  start. A failure stays on the Ready page: its last log line in amber, "4x textures: try again", the details;
  texpack.py ends a Real-ESRGAN failure with a sentence about Vulkan as the last line. texextract.py builds its hash
  DLL with zig when the build's zig is there (players have no Visual Studio; same keys as MSVC's). A tool whose
  checked file is gone is fetched again (not only by its .ok). Checked in a test copy under build\ (the dump and pack
  hard-linked, aliases.txt / nopack.txt copied, zig by a junction, the launcher driven by posted keys and pictured with
  PrintWindow): the real downloads, 30 textures made to the end (the button gone), 445 with a Cancel at 92% and
  "Carry on" to the end, a hidden Real-ESRGAN failing as above. Not checked: a whole run from nothing (~90 minutes
  here), another vendor's GPU, a PC without Vulkan. The videos (vidpack.py) stay on the command line.
- From GitHub (DivxPlays/redline-recompiled, read 2026-11-08 and 2026-11-10; issues #1-3, #5-19, PR #4, none
  answered yet; the 0.2.0 alpha release commit closed #1, #2, #5, #6 and #8-#18, and left #3 open for its hitches and
  buttons):
  - **#19 "can't change language"** (opened 2026-11-10, no text): Xbox games take their language from the
    console's settings (XGetLanguage); not checked yet what the port answers. Find what kernel.c returns for it and
    add a Language choice to Recomp Settings and the launcher; ask the reporter which language they wanted.
    **Fixed 2026-11-12** (not released): kernel.c answered setting 7 (XC_LANGUAGE) with 1, English. The game has
    eight, by the Xbox's numbers (its table at 0x52C7B0: 1 EN, 2 JA, 3 DE, 4 FR, 5 ES, 6 IT, 7 KO, 8 ZH, folders
    media\Loc, Fonts and FrontEnd; no Portuguese, though Redump's name for the disc lists Pt). Setting 7 is now the
    `language` setting (recomp.ini; GAMELANG for scripts, as LANGUAGE is Wine's and Linux's locale list), a row in
    Recomp Settings (English names) and the launcher (each language's own name), at the next start. Checked: French,
    Japanese, Korean and Chinese menus; the row steps through all eight in the game and the launcher. Not checked: a
    race or the career in another language.
    Until one is chosen, the launcher (at its start) and kernel.c (at the game's) save Windows' display language's
    number (GetUserDefaultUILanguage; English if the game lacks it). Checked only on an English Windows (it saves 1).
  - **#20 the update checker never sees pre-releases** (found 2026-11-10 after 0.2.0): launcher.cpp asks for
    /releases/latest, which skips pre-releases, and every alpha is one. Ask for /releases?per_page=1 instead; and
    publish the next version as a full release (0.2.0 launchers read only /releases/latest). 0.2.0 alpha itself
    was switched to a full release on 2026-11-10 (the repo page still showed 0.1.0 as Latest); CLAUDE.md now says
    every release goes up as a full one.
  - **#21 the loading screen into a race looks wrong** (the owner, 2026-11-10, at 32:9 with DLSS; no details yet):
    CUILoadGame and the pre-race screen (CPreEventOverlay). Capture both at 16:9 and 32:9 (rt.now, SHOTEVERY) and
    compare with how the game should show them. Rule out: since the stutter fix the pre-race screen skips draws whose
    shader is still compiling (forza_throwaway_view; first time only, or after an update changes the generators: the
    MV epilogue change recompiled every MV vertex shader), and ULTRAWIDE keeps 2D screens in the middle 16:9 (the
    menus to-do item).
    **Fixed 2026-11-10** (not released): neither. CUILoadGame draws the track's loading picture (its photo and logo,
    map, time, temperature, wind, turns), a 1024x512 DXT5 texture (format 0F, coordinates 0..0.62 x 0..0.94: 640x480
    of it) the game loads at 04B2C000, which was the car-select screen's render target. texture() took any 2D texture
    at a target's address for that target, so it showed the stale car-select picture's top-left 62%: magnified at
    16:9, off centre and cut off at 32:9, a black box where the car had been. texture_target() now never matches a
    compressed or palettised format (I8, DXT1/3/5: no render target is one); render_draw's rt_read uses it too, so
    the picture is a 2D draw (the middle 16:9 at ultrawide). Checked at 32:9 and 16:9.
  - **#22 "fails to download python"** (Mungr3ss, 2026-11-11, on 0.2.1; the #18 reporter, whose Visual Studio
    error 0.2.0 fixed): Build stops at the first tool, "The download failed (error 800C0008)" (INET_E_DOWNLOAD_FAILURE),
    while the same python.org file downloads in their Firefox. fetch_tools() uses URLDownloadToFile, which goes
    through Internet Explorer's settings (proxy, security zones, TLS options) and its download cache; Firefox uses
    none of them. Plan: when it fails, fetch with Windows' own curl.exe (System32 since Windows 10 1803; its own HTTP
    stack and no cache), and accept a download.zip the player saved by hand when its SHA-256 matches, with the
    failure message saying where to put it. The message's "check the internet connection" misleads here.
    **Fixed 2026-11-11** (0.2.2): fetch_tools() first uses a copy the player saved by hand (the URL's own
    file name in Downloads or the port's folder, when its SHA-256 matches; it stays there); else URLDownloadToFile,
    and when that fails, %SystemRoot%\System32\curl.exe -fsSL --retry 2 --ssl-no-revoke (curl_cmd(), through run(),
    so Cancel stops it). If curl fails too, the log gives the URL and the headline "Download <file> in your browser
    (the link is in the details), then press Build again". The update check falls back to curl too (silent, 30 s at
    most), so such a PC still hears of the release that fixes it. Tested in Windows Sandbox with WinINet broken by a
    refusing proxy in Internet Settings (error 800C0005; 800C0008 not reproduced): with curl broken too (HTTPS_PROXY)
    the message; with Python saved in Downloads, used from there and kept; with curl working, all three tools through
    curl and the update check's answer (v0.2.1-alpha).
    Slow connections (sandbox, a proxy slowing what servers send, 2026-11-11): through curl at ~350 KB/s the tools
    took 4.9 min (zig 4 min) and finished; the card showed only the tool's name then, so watch_size() now shows
    "zig 0.14.1 (the C compiler), 5 of 82 MB" from the file's size every 0.5 s. Cancel during a slow download stops in
    1.1 s, curl's or Windows' (its partial download.zip deleted). The update check's answer is ~10 KB with TLS: 11 s at
    1 KB/s; at 150 B/s curl is stopped at 30 s, silently, the launcher responsive throughout (it needs ~350 B/s).
  - **#23 "Unknown error"** (tfundeca, 2026-11-11, Portuguese Windows, 18 GB free on C:, which is enough): two logs.
    - C1002 on funcs_039.c in `C:\Games\RedlineRecompiled-0.1.0`, which they took for the new version. The log runs
      `py -3` and Visual Studio's cl, which only 0.1.0's launcher does (0.2.x runs build\tools' Python and
      `build.py --zig`), so 0.1.0's launcher and lift.py ran: the 0.2.2 zip didn't replace them (unzipped elsewhere,
      or "Skip" at Windows' replace prompt). Answer: a fresh folder, and the footer must say 0.2.2.
    - A Redump image (`Forza Motorsport (USA) (En,Ja,Fr,De,Es,It,Pt,Zh,Ko) (Rev 1).iso`) stops at "not an XDVDFS
      image (magic missing at sector 32)". xiso_extract.py reads only plain XISOs. A full-disc dump has the video
      partition first and the game's XDVDFS volume at 0x18300000 (XGD1; XGD2 0xFD90000, XGD3 0x2080000, as
      extract-xiso), with directory sectors relative to it. Still so in 0.2.2. Their extracted folder got past it.
      **Fixed 2026-11-11** (0.2.3): xiso_extract.py looks for the volume at those offsets (0 first) and adds
      its start to every seek. Tested on made-up images (an XISO; a full-disc one at 0x18300000, which the old code
      failed with the player's message; junk: "This .iso is not an Xbox disc image") and on the owner's XISO
      (identical to game\, Forza.xbe's SHA-1 right). Not tried on a real full-disc dump of Forza.
  - **#24 "Square Shadow Under Car"** (markmepage-ui, 2026-11-12, on 0.2.2): night tracks (Tokyo, Pacific Shipyards;
    not Sunset Peninsula), texture pack on or off; the owner also saw the road's light wrong on Tokyo. Already in
    0.1.0's sweep. At night a race lights its road through a 640x480 light buffer (one of the three screen targets,
    drawn first each frame): cleared to 0x3F3F3F, the road pieces, the car's flattened silhouette multiplied in
    (stencil NOTEQUAL 1 / REPLACE, so once), each road piece again with the headlights added (ONE/ONE: a 128x128x8 DXT5
    volume, PROJECT3D, all three axes clamped to a black border; texcoords from the car's frame: s lateral x0.1+0.5,
    t forward x-0.025+0.9, r height x0.25+0.25), then a 2.5 x 4.5 m quad under the car multiplying 1 - 0.875 x a soft
    32x64 L8 blob. The road samples it (16 draws; none also samples the shadow map). Daytime tracks draw the same
    buffer with the sun's shadow map in it, and the same blob (0.81).
    The headlights' road pieces wholly outside the beam (s 1.5-2.8) added full white: render.c's sampler cache keyed
    on address mode and filter only, so the volume got the sampler of an earlier texture with a light border. The
    right of the road and its barrier came out white with a straight edge, and the blob next to it read as a dark box.
    **Fixed 2026-11-12** (not released): the border colour is in the sampler's key. Tokyo's grid: an evenly dim road,
    the beam ahead, a soft contact shadow behind the bumper. Pacific Shipyards is locked in the test profile, so not
    seen. The dark patch under the car plus the sun's shadow by day is the game's own pair (the owner called it a
    double shadow on Laguna Seca): unchanged.
    Tools from it: a frame dump (build/dump.now) now starts with the state it inherits ("s <method> <value>" lines);
    build/targets.now saves every colour target once (as SCREENSHOT); build/hotlap.ps1 counts tracks from the list's
    start (the list starts at the track chosen last and does not wrap) and checks Tokyo's name (build/tokyo_list.png).
  - **#25 "Version mismatch (label)"** (2026-11-12): the launcher says 0.2.3, the game's corner 0.2.1. Cause: the
    launcher's built() only checks that build\forza_recomp.dll exists, so after a new version is unzipped over an old
    one it offers Play and runs the old build (only Rebuild makes a new one). 0.2.2 and 0.2.3 changed only the launcher
    and version.h, so nothing was missed yet, but the next release changes the game itself (#19, #24, the pop-up,
    Shift on Shoulders): players who update would get none of it. Fix before that release: remember the version the
    game was built with and offer Build again when it differs.
    **Fixed 2026-11-12** (not released): build.py writes build\built_version.txt (version.h's RECOMP_VERSION) after a
    whole build, and the launcher offers Play only when it is its own version; otherwise the home page says "Update
    your build" and its button (Update) runs the build again (the disc and tools kept). Checked: no stamp gave that
    page; Update built in 29 s here and Play came back.
  - **#26 "Mitsubishi Lancer VIII thumbnail not showing in garage"** (2026-11-12): the 2004 Lancer Evolution VIII
    FQ-330 (B1, so upgraded or won) in the Free Run garage's car bar: its slot a grey box with a black band down its
    right fifth, no car. The comment about the white, shiny logo is the car's own chrome badge: not a fault.
    How icons work: media\CarIcons1-5.xpr are 128x4096 DXT5 strips of 64 slots of 128x64, one per car (231; strip 1
    slot 56 is a solid yellow placeholder on the disc). The game copies them to the profile (T:\CarIcons*.xpr and
    CarIcons.sig) and, for every car the player owns, draws the car into a 128x64 target (with a 128x64 depth),
    locks it to read it back, compresses it into slots 0, 1, 2... of strip 1 and saves it ("Updating Thumbnail Icon",
    with the SaveLoadHandler save, f_00390820). The FQ-330 shows fine in Buy Cars (its disc icon).
    Cause, reproduced by making the icon's target 160x240 at its creation (as a bigger surface at the same address
    earlier leaves it: targets only grow): D3D refuses a draw whose colour and depth targets differ in size, so the
    car never drew on the cleared grey; and render_readback_ squeezed all of the target into the 128 columns read
    (grey in the left 80%, the rest black: the player's picture at 160 wide). **Fixed 2026-11-12** (not released):
    a colour target bigger than the surface drawn, with its depth exactly the surface's size, is made that size again
    in its slot, its corner (the background drawn so far) copied over; and a read back takes the last surface drawn
    into the target (sw x sh, its top-left corner), which also keeps the anti-aliased screen's 2:1 squeeze. Checked:
    the same test now saves the car's icon; a bought car's icon at 1080i/2x/DLSS/ultrawide and 480p/2x/FSR 1; a race
    at 1080i unchanged, with no target made again in normal play (the screens' 1920 colour with the 3840-wide AA
    depth, the game's own mismatch, is left alone: growing those to match shrank the race into a corner). An icon
    saved broken stays so until the game draws that car again: repainting it does (Garage > Customize Paint: the
    Acura's slot 0 was drawn again, brown). Not checked: the player's own profile and situation (what put a bigger
    surface at that address first).
  - **#28 "Various visual bugs"** (tfundeca, 2026-11-12, on 0.2.3; six screenshots), three things:
    1) In a race, the HUD's car status box (tyres and damage, right of the lap times) shows a grey box with part of a
       car's side view where its drivetrain diagram belongs (engine, steering wheel, axles: what it shows here).
       Mechanism found, not the player's exact case: a target the game reads back (a car's icon, a paint job) hands
       its memory back to the game, which reuses it for new textures; texture_target still answered with the old
       host target at that address, so a texture there showed the old picture (an icon: a car on grey). Seen here:
       after a repaint, a race's load put a 421x8 LIN_A8R8G8B8 texture where the icon's target (009B1000) had been.
       **Fixed 2026-11-12** (not released): a read back records the hash of the guest memory it wrote (mem_hash);
       until the target is drawn into or cleared again, texture_target compares it once a frame when the target is
       sampled, and once the game has written something else there, textures read that ("[render] target ...: the
       game wrote over it since it was read back"). Also seen: the race's quarter-size glare source (480x270, read
       back at each start) is written over before its next draw: one frame each. Races and menus look the same.
    2) Tokyo: barriers and buildings glowing white. That is #24, fixed (not released).
    3) A car painted in Customize Paint loses its shine and gets grey patches (a red Mustang Cobra R in My Cars).
       Reproduced: after painting, the Acura's grille and badge surround turned paint-coloured grey. How a paint job
       is made: the game clears a 1024x1024 target to 00FFFFFF and draws one quad that reads that same target (t0)
       and the livery as an 8-bit indexed image (t1, LIN 0x1B, the index in alpha) looked up in a 256x1 palette with
       the new paint (t2, texture mode 15 DEPENDENT_AR: t2 = tex2(t1.a, t1.r)), r0 = t2 over t0 by t2.a; then a 512
       copy of it and a 512 dirt layer the same way, each read back and compressed. Two gaps made all three blank
       (white, alpha 0), so painted cars lost every detail printed on them: shaders.c had no DEPENDENT_AR / _GB
       (t2 = 0), and a draw reading its own target got an empty unit. **Fixed 2026-11-12** (not released): mode 15/16
       sample the stage's texture at the stage before's (a, r) / (g, b); a colour target read by its own draw is read
       from a copy (target_copy). (A first try that read the guest memory of a target not drawn yet was wrong: the
       game clears it first, and that memory held heap leftovers: camouflage in the paint shop's preview.) Checked:
       the three layers now hold the car's lights, badges, lettering and interior (and the dirt); the paint shop,
       My Cars and the new icon show the painted Acura with its dark grille. Not compared side by side: the shine
       itself. A car painted before keeps its blank layers until painted again. READBACKDUMP=1 saves each target
       read back as build/rb_<address>.bmp.
  - **#27 "Steering wheel not working"** (2026-11-12, a Fanatec CSWB V2; the owner answered that wheels are planned):
    input is XInput only (hle_xinput.c), so DirectInput wheels are not seen. On the roadmap: "Steering wheels" below.
  - **C1002, compiler out of heap in pass 2** (#2, #3, #8, #16; Visual Studio 2026 and Build Tools 2022, 16 GB RAM,
    16 CPUs): recomp/funcs_039.c's f_002E5DC0, f_002E6930, f_002E74E0, f_002E8050 (~4,000 lines each), then D8040.
    **Fixed 2026-11-10** (not released): measured alone, funcs_039.c took 65.4 GB of commit to compile (it built here
    only because this PC has 61.5 GB and a page file), so fewer parallel cl would not have helped. Those four are
    long SSE blocks (~1,200 addps/subps/mulps each, an FFT called from 0x2DB922/0x2DBC18), and lift.py wrote every
    packed op as a loop over 4 lanes. It now writes one host SSE intrinsic each (x86.h XL/XS; same lane math,
    rsqrtps/rcpps still exact): funcs_039.c 0.24 GB in 8 s (was 40 s), funcs_030.c 0.44 GB (was 1.71), all 59 files
    16 at a time 3.1 GB peak (was 48.7 GB 8 at a time). PR #4 (opt off for the 67 functions of 3,000+ lines) is not
    needed and would have slowed those; thank them and close it when 0.2 goes out. The race's sound after the
    change (20 s recorded) sounded right to the owner (2026-11-10).
  - **#1: "does not build"** after ~2 hours, extra installs, "several different image files": the launcher takes
    only Forza Motorsport (USA) (Rev 1) (SHA-1 check). Say plainly which disc is needed, and why another fails.
    **Fixed 2026-11-12** (not released): the launcher reads Forza.xbe and default.xbe off an .iso itself
    (launcher.cpp iso_top_files: the XDVDFS volume where xiso_extract.py looks, the top folder's tree) and checks them
    (source_check / disc_check) when the disc is chosen (and at the start), saying it under the path, and again first
    thing in Build, before the tools' 95 MB and the 3.6 GB unpack (an ISO unpacked before is checked in game\, as the
    image may be gone). A folder is checked the same way. The headline says what it is, from the XBE certificate:
    another release of Forza (its version; USA Rev 1's Forza.xbe is 70), another game (its title, from default.xbe),
    the right version with other bytes (damaged or modified), or no Xbox image. Not the region: Rev 1's Forza.xbe is
    marked for every region (7), so editions cannot be named by it. Tested on made-up XISOs and a made-up full-disc
    image (each case at choosing; Build with a wrong one, and with the right one, which went on to the unpack and step
    4); not on a real disc of another edition.
  - **Hitches** (#3, build\redline.log, RX 6600): 5,027 ms at CPreEventOverlay (96 shaders compiled, 353 textures
    decoded in one frame: first run, nothing cached), 2,329 ms SelectCarView, 3,000 / 2,017 / 1,884 / 1,233 ms unnamed,
    1,200 ms CUIPlayerProfile, 1,083 ms CUIOptionsMenu twice with no shaders or textures (CPU-side), 635 ms
    CUIScoreboardUpdating; menus 60-170 ms typically. Shaders could be compiled ahead (at build time, or warmed from
    a list) so a first race does not stall.
    **Fixed 2026-11-10** (not released), measured on the owner's settings (1080i, scale 2, DLSS, ultrawide, 144 fps)
    with FPSLOG and the [hitch] lines, the test's own screenshots off (SHOTEVERY's 21 MB BMPs every 20 frames had made
    the race look stuttery: measure with scratchpad pace.ps1, not camtest):
    - The ~2 s freeze at car select (or, in other runs, the race's countdown) was DLSS starting on the first frame
      that needed it: the D3D12 device 81 ms, NGX's NVSDK_NGX_D3D12_Init 1,815 ms, the feature 51 ms. FSR 3 starts
      the same way (its library; its context per size). up12.cpp upscale_warm now starts both on a thread of their
      own when the device is made (render_fsr_ too, when the setting changes); until it is done, temporal() scales
      the plain way. Car select: 2,102 ms -> 92 ms. The start-up lines now give each step's ms.
    - First run, empty shader cache: 220 shaders at ~30 ms each (fxcbench.py on the game's 224 sources: optimisation
      L0/L1/L3 all ~30 ms; skip-optimisation 21 ms with twice the DXBC: no flag fixes it). Car select 1.4 s, the
      pre-race screen 3.3 s (101), the race's start 1.3 s (43), race windows with 44-56 compiles and 20+ late frames.
      render.c now compiles a new shader on worker threads (cores - 2, at most 8) while forza_throwaway_view() (the
      race, its countdown, CPreEventOverlay: pictures the game never keeps); its draws are skipped the few frames
      that takes. Elsewhere the draw waits as before: a paint job is rendered into a texture and saved, a car's icon
      copied off the screen. After: the pre-race screen's 94 compiles cost a 69 ms frame, the race none over 20 ms;
      car select still 1.2 s once per PC (sync). Checked: a cold-cache race looks complete (cars, track, mirror, HUD).
    - Left, warm cache: CUIPlayerProfile ~0.54 s and CPreEventOverlay ~1 s (no shaders: the game's own loading),
      the countdown ~0.3 s (the texture pack's deliberate wait, g_texwait_ms 100, for a new scene's replacements),
      and three 2-3 s gaps at boot behind the black screen.
  - **Controls vs the game's diagram** (#3): Controller Options with "Shift on A/B Buttons" shows X e-brake, A gear
    up, B gear down; the port's table (hle_xinput.c g_bind) has A e-brake, B up, X down, and the keyboard's E/Q send
    B/X. Check in the game which pad buttons shift under each Controller Options layout; match it or document it.
    **Fixed 2026-11-12** (not released): pads were right all along (read_gamepad passes buttons by position, and the
    game applies its own layout); the keyboard's driving keys pressed the Default Layout's buttons whatever the
    layout. The game has 7 layouts (Default, Shift on Triggers, Accel on Thumbstick, Shift on Thumbstick, Shift on A/B
    Buttons, MadCatz Wheel, Speedster Wheel) and keeps a record per controller at 0x51C0D4 + 12 * n {?, layout,
    vibration} (found by RAM dumps across layouts; the game indexes it so at 0x723AE). hle_xinput.c driving_in() now
    presses each driving key's action as controller 1's layout has it, from its diagrams (g_layout), the right
    stick's up / down too (Shift on Thumbstick). Checked in a race with manual shifting: E/Q shift 1-2-3-2 with Shift
    on A/B Buttons and with Shift on Thumbstick. Not checked: the wheel layouts and Shift on Triggers in a race.
  - **Pad buttons cannot be rebound** (#3): Recomp Settings rebinds keyboard keys only; asked for rebinding driving
    actions on a pad, or shifting on the shoulders (Black/White).
    **Shifting on the shoulders done 2026-11-12** (not released): Recomp Settings SHIFT ON SHOULDERS (recomp.ini
    shoulders; SHOULDERS for scripts), in a race only (forza_racing): a pad's RB / LB (the Xbox's Black / White)
    press what that controller's layout has for gear up / down (driving_in, by port), and camera / HUD move to the
    right / left stick clicks. Checked in a race on the Default layout with a script-held RB / LB (TESTKEYS 8193-8196:
    RB, LB, R3, L3 on port 0, over a real pad's): 1-2-3-2, the guest seeing B and X. Full pad rebinding: still to do.
    Seen while testing: on Shift on Thumbstick, loaded from the profile with an Xbox Series X pad connected, neither
    the keyboard's E nor the shoulder's right-stick-up shifted, though the keyboard's had in an earlier session (the
    layout set in Controller Options, maybe no pad then). **Not reproduced (2026-11-12)**, so not a fault of the input
    code: with the pad connected, E shifted 1-2-3-2 on Shift on Thumbstick both set in Controller Options and loaded
    from the profile after a restart (record 0's layout, 0x51C0D8, read 3 at the main menu; the game finds a player's
    record by its index at player+0x314, f_000723A0). The likely cause of that run: Free Run's transmission had come
    back as Automatic (it did here after the restart, though set to Manual before it), where nothing shifts.
    Controller Options' Save writes the profile at once (UserProfile.xml: the four 12-byte records {?, layout,
    vibration} from 0x504, controller 1's layout at 0x508); the assists' transmission is saved only by a later save
    (a race's end), so a test that kills the game keeps the layout and loses Manual. A test that changes either puts
    the profile back (scratchpad backup) afterwards.
    **Full pad rebinding done 2026-11-12** (not released): Recomp Settings > CONTROLLER BUTTONS (hle_xinput.c; the
    page after MENU KEYS), ten racing actions (accelerate, brake, e-brake, gears, look back / left / right, camera,
    HUD), choose a row and press a button on any controller. Until one is changed it shows and does what controller
    1's layout has (with SHIFT ON SHOULDERS); the first change saves all ten (recomp.ini pad_custom = 1,
    pad_<action> = source: 0-9 A B X Y RB LB RT LT RS-click LS-click, 10-13 right stick up / down / left / right,
    14-18 d-pad up / down / left / right, Back; -1 none). In a race (forza_racing) each action's source then presses
    what that controller's layout has for the action (driving_in, as the keyboard does, analog values kept: a trigger
    on a face button's pressure), so the choice holds in every layout; the face buttons, shoulders, triggers and
    stick clicks do only what is bound, the d-pad, Back and right stick axes keep their job unless bound; SHIFT ON
    SHOULDERS is then ignored. A button bound to a second action leaves the first. The capture: the game sees nothing
    of the pads while it waits; buttons held when it starts or ends count only once released (the menu calls the
    row's factory a moment after A); Start, 5 s or any keyboard key gives up. Menus keep buttons by position. The
    LAYOUT row goes back to the game's (pad_custom = 0). Names follow the pad last used (CROSS, R1... on a
    PlayStation pad; not checked, none here). Labels over ~20 characters wrap, and the menu does not redraw a row's
    description while it is selected. Checked with script-held pad buttons (TESTKEYS 8193-8203 now RB LB R3 L3 A B
    X Y RT LT Start; port 0 counts as a pad in test mode): GEAR UP -> RB, GEAR DOWN -> LB took camera and HUD off
    them; in an Arcade race on the Default layout RB gave the game B, LB X, B and X nothing, A A; Start cancels;
    LAYOUT back to the game's made RB the camera again. Then on Shift on Triggers (Controller Options, saved; the
    profile put back after): the page started from that layout (accelerate A, brake X, e-brake B, gear up RT, gear
    down LT), GEAR UP -> RB, and in a race RB gave the game RT, RT nothing, A A, X X. Not checked: a real pad
    (INPUTLOG now also prints the right stick). Controller Options' controller 1 screen opens on its layout row
    (rows: vibration, layout, d-pad), left / right change it, A saves.
  - **RUMBLE STRENGTH** (2026-11-12, not released; recomp.ini rumble, RUMBLE: 100 / 75 / 50 / 25, both settings
    lists, at once): hle_XInputSetState scales the game's two motor speeds by it. INPUTLOG logs each change with the
    game's own values; a race at 50 gave exactly half (0 5734 for 0 11468...).
  - **ACHIEVEMENT SOUND** (2026-11-12, not released): Recomp Settings ON/OFF (recomp.ini achsound), read at each
    pop-up (audio.c audio_chime). Checked: off, a 20 s recording of four pop-ups has no fanfare (best match 0.44).
  - **The launcher's Settings in two tabs** (2026-11-12, not released): its page held 13 rows, so the four newest
    settings were in the game only. Now Picture (11: scale, upscaling, frame rate, filter, fullscreen, widescreen,
    ultrawide, video mode, texture pack, videos, texture dump) and Game & sound (6: language, button prompts, shift on
    shoulders, achievement sound, enhanced audio, speakers): every row of the game's Recomp Settings (Setting.tab).
    LB / RB on a pad, Tab on the keyboard, switch them. Checked: both tabs, Tab, a click on Shift on shoulders writes
    shoulders=1 and back. Not with a real pad's LB / RB (the same handler as Tab).
  - **Kernel settings 0x11 and 0xA read as 0** at startup (kernel.c "[k] setting ... reads as 0"): check what the
    game reads them for. **Nothing to do (2026-11-12):** they are the EEPROM's XC_P_CONTROL_GAMES (0xA, the
    parental rating for games: 0 = all allowed) and XC_MISC (0x11, flags: auto power-down, no daylight saving: 0 =
    neither), so 0 is the right answer for both, as on a console with no parental lock.
  - **#5 "Game won't install"** (PsxMicha, Windows 11, opened 2026-11-08): find_functions.py stops with
    FileNotFoundError on analysis\Forza.functions.json, i.e. the analysis folder is not there. **#9** is the same.
    **Fixed 2026-11-10** (not released): choosing the ISO with the launcher's file dialog made the ISO's folder the
    launcher's current one, so CreateDirectoryA("analysis") made it there while the steps run in the root. The dialog
    now has OFN_NOCHANGEDIR, and find_functions.py makes its output folder.
  - **Crash loading a race at 720p: "guest heap exhausted"** (#17, RX 9060 XT; after one or several races, always on
    a race selection screen): "want 1000 at 08000000; 757 free ranges, 2ACA3D0 bytes" (43 MB free, 111 MB live:
    00266BE5 55 MB, 002FB568 34 MB, 0026BC1E 15 MB). The player found 1080i does not crash: only 1080 gets 192 MiB
    of guest RAM (main.c g_ram_end). **Fixed 2026-11-10** (not released). The 1,011 4 KiB blocks by 00267332 are
    XAPI's VirtualAlloc (NtAllocateVirtualMemory, through [0x4863F0]), which kernel.c aligned to 64 KiB, so each one
    left a 60 KiB gap; UILOG's screen lines now print the heap (live, top, free, largest free range), and at 720p
    the free space below the top was ~33 MB in pieces of at most F000 by the first race. The failing "want 1000"
    was one of those VirtualAllocs with no 64 KiB boundary left in any piece. The fix: 720p gets 192 MiB like 1080
    (the player's 1080i workaround), and the "exhausted" line now gives the alignment and the largest free range.
    Measured at 720p / 128 MiB, the North American Open: the live heap grows ~330 KB a race and the top ~320 KB
    (results screen 4CC80E0 -> 4D1A1F0 over races 1-3): maybe a slow leak, or caches filling; watch longer runs.
    **Lead for a real fix of the fragmentation** (480p keeps 128 MiB): VirtualAlloc page-aligned instead of 64 KiB
    put a 720p race load's heap top at 74.8 MB instead of 99 MB (free pieces 0.5 MB instead of 26 MB), but crashed
    while loading the first race: a garbage pointer (34F0DB50) pushed on the free list of the small-block allocator
    at 0027D540 (4 KiB pages, link at +FFC). Something relies on the 64 KiB alignment (or on blocks not being
    reused so soon); find it before changing the alignment. Squeezing 720p to 118 MiB failed sooner, on a 2 MB block
    (align 1000) with 35 MB free in pieces of at most 1.2 MB, so the shredding hurts more than VirtualAlloc.
  - **#16** is C1002 again; it asks whether the toolchain is 32- or 64-bit (build.py uses vcvars64, the 64-bit one).
    **#8**'s screenshot is C1002 too (in Portuguese); it asks why 16 GB of RAM to build a 2005 game: see above.
  - **#18 "cl not found on the MSVC PATH"** (screenshot): with cl.exe added to PATH by hand it said include files
    were missing, so vcvars64 was not applied. build.py kept vcvars64's environment in build\vcvars.json forever,
    even after Visual Studio was installed, updated or repaired. **Fixed 2026-11-10** (not released): the cache is
    used only while vcvars64.bat is the same and its PATH has cl.exe, and when vcvars64 does not set up cl the build
    stops with what vcvars64 printed (it was sent to nul). If #18 still fails, that text says why.
  - **#15 "UNPLAYABLE"**: the install always stops ("worst launcher for recomps"), asks for a simpler install. The
    fixes above and the simpler-requirements item answer it.
  - **"RAM usage"** (#8, #15, the owner 2026-11-10): the build's, fixed above (the compile peaks at 3.1 GB with
    16 files at once, zig compiles as many as free memory allows at ~1.5 GB each). The game's, measured 2026-11-10
    at 480p / scale 2 / FSR 1 in an Arcade race at Alpine Ring with the camera views cycled: working set 0.3-0.44 GB
    throughout; private (commit) 0.82 GB at most without the texture pack, with the 4x pack 1.6 GB at the grid
    rising to 2.29 GB by 100 s as new textures come into view, then flat for the last minute (no leak); GPU
    dedicated 1.75 GB (2.09 GB at 1080i / DLSS / ultrawide). The commit is mostly Windows backing the video memory;
    texpack.c frees each decoded texture after its upload and holds the GPU at a third of its memory (LRU). Nothing
    to cut there now.
  - **Launcher Settings: ‹ steps forward** (#6, found in Stilwater): draw_settings registers the arrow's hit box
    before the value box that covers it, and hit_test takes the last; register the arrow after the value box.
    **Fixed 2026-11-10** (not released), checked by clicking it in a scratch copy (2x -> 1x; 0.1.0's went to 3x).
  - **Launcher features every kit launcher has** (#10-14): say it's made with the help of AI (#10), the update
    checker (#11), "What's new" once after an update (#12), works with a controller alone (#13), a "Report this
    crash" button (#14).
  - **Mouse steering / camera** (#7, from Stilwater): if added, drive the game's own steering or camera update from
    the mouse's movement, not the stick (a stick's dead zone and acceleration make it feel like one). The camera:
    on the roadmap ("Mouse camera look", below).
- From Reddit (r/decomps 1xp5sxc, 33K views, 26 comments; r/recomps 1xp61gv; read from the owner's PDFs
  2026-11-08, a few comments cut at page breaks):
  - **The build tools** (moesengott_, Turbulent-Pomelo8480, MaleficentBasket): "The Visual Studio C++ build tools
    were not found..." read as "the launcher fails to download some build tools; never had this with any other
    recomp"; "need to do as other recomps, that don't ask for Visual Studio desktop development C++, almost 7 GB
    download, Python etc." The simpler-requirements item above; until then the launcher's message should say it
    downloads nothing itself and what to install.
  - **Asked for a built copy** ("mine gave errors, can you upload it and send it to me"): never (nothing of the game
    ships); the C1002 fix and the simpler build are the answer.
  - **Import saves from xRevive** (Ill_Excitement_8052, in Portuguese: wants their xRevive save, "a bit different"):
    xRevive is another, closed-source PC version of Forza Motorsport; its save format would have to be studied
    (a player's own save files only).
  - **Micro-stutter in races**: the owner replied he'd fix it in the next update; with the hitches above.
    Measured 2026-11-12 (scratchpad pace_aff.ps1: pace.ps1, an Arcade race at the owner's settings with FPSLOG, and
    -Mask to hold redline.exe to fewer cores; this PC a Ryzen 7 9800X3D, 8 cores). 8 cores: 144 new frames a
    second, the worst gap under 13 ms after the countdown's (~300 ms, the texture pack's planned wait and ~120
    decodes). 2 cores: 63 and 109 ms frames mid-race as scenery came in, with 8 and 29 replacements loading, and new
    frames down to ~124 a second: the pack's loaders (half the PC's cores, 2-8, whatever the affinity) and the shader
    workers (cores - 2) ran at the game's priority. **Changed** (not released): both run below normal (as video.c's
    decoder already did). After, 2 cores: the pack-time hitch gone; 56 and 68 ms ones with no loads in them remain
    (the game's own work; not found). 4 cores: one 34-46 ms frame either way. One run each, so a direction, not a
    measurement. The ~2,000 "decoded" a second in a race are the ~32 1x1 colour textures the game rewrites each
    frame, updated in place (cheap).
    **The rest, looked into (2026-11-12)** with new tools: HITCHLOG=1 adds, after each [hitch] line, every thread's
    CPU ms in the gap (QueryThreadCycleTime; threads named by thread_name: guest <start>, gpu (push buffer),
    vblank / DPC, texture pack loader, shader compiler, audio, input, window...), and a stall watcher that, when a
    race goes 35 ms without a new frame, prints each guest thread's function and stack, the GPU's push buffer and
    flip queue (nv2a_brief) and the host stacks of the gpu, texture pack and shader threads (profile_stacks).
    Found, two cores: the 55-68 ms frames had the CPU mostly idle (~20 of 110 core-ms), so waits, not shortage; in
    every stall the game's main thread waits for push buffer room (D3D_MakeRequestedSpace -> D3D_BlockOnTime) with
    the GPU's commands waiting and no flip stall, and the GPU thread was at a different place each time: hashing
    textures, uploading vertices, waiting for the GPU lock while the vblank thread presented, waiting for a pack
    replacement (the countdown's planned wait), or in the D3D driver; the pack loaders were idle each time (no
    priority inversion). At 60 frames a second on two cores, driving is smooth (worst 17-18 ms) but for one 46-67 ms
    frame at each camera change (pace.ps1 presses V every 4 s; presenting time up those windows; no DLSS re-create),
    which 4 and 8 cores show none of. So: no single cause left; two cores at 144 is more work than they can do.
    Not done: making a camera change cheaper. Frame pacing against the display (the port presents at the emulated
    vblank, sync interval 0: on a fixed 144 Hz screen without G-Sync/FreeSync, 60 a second shows 2 and 3 refreshes
    in turn, a judder however smooth the game runs): FRAME RATE AUTO and pacing against the screen, below.
    **FRAME RATE AUTO** (2026-11-12, not released; the new default, value 0): the primary screen's refresh rate
    (EnumDisplaySettings; 59 -> 60, 143 -> 144 and the like, halved past 165, at least 60), resolved in forza_init
    (forza_fps(); hle_bink.c asks it too, so the track previews keep their own 29.97). Checked: 144 here, a race at
    144 new frames a second (worst gap 7.8-10 ms), the previews at 2997/100. Auto reads the screen the window opens
    on (render_screen), not the primary.
    **Pacing against the screen** (2026-11-12, not released). FPSLOG now says what the screen made of the presents
    (GetFrameStatistics: "refreshes repeated", a refresh that showed the last present again, and "presents
    dropped", one replaced before any refresh showed it) and where in the screen's refresh each present came
    ("phase", eighths after its vblank). Measured windowed on the 144 Hz screen at the main menu: presents at 75-87%
    of the refresh (about 1-1.7 ms before the vblank, when the compositor takes its frame) miss it half the time, ~16
    repeat/drop pairs a second; anywhere else about one pair in 20 s. On its own clock the emulated vblank drifted
    through the refresh (here one cycle in ~6 minutes: the screen measures 143.98 Hz), so for ~45 s of each cycle the
    picture stuttered that way; how often depends on the screen. Now: a "display vblank" thread waits for the
    screen's vblanks (IDXGIOutput::WaitForVBlank on the output the window is on, followed when it moves, any
    adapter's) and keeps the last one's time and the refresh period; when the frame rate is the refresh rate or a
    whole fraction of it (within 1%: this PC's 60 Hz screen runs at 60.27), dpc_proc moves each emulated vblank a
    tenth of the way to a quarter of a refresh after the screen's. A quarter, as presents only ever come late: far
    from the compositor's edge, and borderless fullscreen (checked on the 60 Hz screen) showed no edge at all, even
    at 85-100%. Checked: windowed 144 Hz, menu 2 pairs a minute, a race like before without the bursts; fullscreen
    60 Hz (FRAMERATE=60) none; Auto on the 240 Hz screen 120 locked to every second refresh, none in 24 s. The game's
    clock counts vblanks, so it now runs at the screen's real rate (0.45% fast on a 60.27 Hz screen). VSYNCLOCK=0 turns
    it off, VSYNCPHASE=<0-1> moves the aim (testing). Not checked: a G-Sync / FreeSync screen in its variable mode.
  - Not to act on: wishes (more threads, a graphics overhaul, more tracks, DLSS 5, path tracing, "do FM8 next"),
    comparisons with xRevive and Relive, AI criticism.
- **Linux / Steam Deck support** (requested 2026-11-08). First step: run the Windows build under Proton/Wine (the
  Deck's way): D3D11 through DXVK, FSR 3's D3D12 through vkd3d, SDL3 input as is; DLSS is NVIDIA-only (not on the
  Deck). The build on the player's PC is the hard part there (MSVC under Wine): the simpler-requirements work
  (a portable compiler) would fix it for Linux too. Then the Deck itself: 1280x800 (16:10), its controls, and
  defaults for its APU (render scale 1, FSR 1, 60 fps). A native Linux build (no Wine: Vulkan or GL instead of
  D3D11, SDL audio instead of WASAPI, the Win32 calls in the runtime) would be a much bigger step.
- **Mod support** (requested 2026-11-08; possible, in steps). 1) File mods: file.c already maps D:\ to the game
  folder, so a mods\<name>\ folder whose files take the place of the disc's (cars, tracks, menus, strings, sounds)
  is a small change, with an order between mods and a Recomp Settings page to switch them on. The texture pack and
  upscaled videos already replace assets this way. 2) Code mods: hle.txt replaces guest functions with host code at
  build time; a mod DLL that registers its own replacements or hooks at load (as Zelda64Recomp's mods do) needs a
  small API and a stable list of hookable functions. Mods are made and shared by players (their own files; nothing
  of the game ships with ours).
  **Step 1 done 2026-11-12** (not released): file.c host_path gives a disc path the file of the first mod (by
  folder name) that has it, from <game>\..\mods\<mod>\<the disc's path>; every folder there is a mod (a name
  starting with "." is skipped, at most 24), on unless recomp.ini mod_<name> = 0, which Recomp Settings > MODS (after
  CONTROLLER BUTTONS) switches. The folders and their files are read once at startup into a sorted list (no file
  system look per open); the log names each mod and each path a mod serves ([mods]). The game makes the mods folder
  (gitignored; never in the zip, which is git archive). Only whole files: the menus' pictures are inside the
  archives in media\cabs, so changing one means rebuilding its archive (texture packs replace single textures
  instead); car folders, event pictures, fonts and the start-up screens are loose files. A mod adding a file the
  disc lacks is opened by name but not listed in a directory search. Checked: a mod putting the team splash in
  place of FrontEnd\EN\DisclaimerScrn.xpr showed it at start (the disclaimer's text without it); MODS turned it off
  (mod_test=0) and the next start left it out. Not checked: a car or track mod, files the game copies to the hard
  disk's cache (the installer compares times, so a mod's file should be copied again).
- **HDR** (requested 2026-11-08). The game draws in 8-bit SDR, so HDR means expanding its picture: an HDR10 swap
  chain (R10G10B10A2, ST.2084) or scRGB (FP16) when Windows reports an HDR display (IDXGIOutput6), the final
  shrink/upscale pass mapping SDR to HDR (paper white for the HUD and menus, a peak brightness, highlights such as
  the sun, lights and bloom lifted), and the scene's own targets kept at more than 8 bits where that helps.
  Settings: HDR on/off, peak brightness, paper white. DLSS / FSR 3 told their input's range. Check: a picture
  that matches SDR at paper white, nothing clipped or washed out, screenshots tone-mapped back to SDR.
- **Camera views** (players report the other views don't work, 2026-11-08). **Reproduced and fixed 2026-11-10**
  (not released): a race has four views (V / Black): two chase views were fine, and the bumper and hood views
  showed only the HUD and the rear-view mirror on black, at 16:9 and 4:3 alike (not the widescreen code, not the
  w-buffer). Their frame draws the mirror's scene, clears depth to far over the whole target, then to 0 in the
  mirror's rect only (so the main scene leaves the mirror alone), then the main scene; render_clear cleared depth
  with ClearDepthStencilView, which ignores the rect, so all of the target was 0 and every draw failed. A partial
  depth/stencil clear of part of the *surface* is now drawn through a scissor (render.c depth_rect_clear); a clear of
  the whole surface still clears the whole buffer. The first version compared the rect with the depth *target*,
  which the 2x-AA scene makes twice a 640-wide pass's width: the first-person frames' light pass (a 640x480 grey
  picture every scene draw samples on screen, cleared to 3F3F3F) then cleared half its depth, its triangles failed on
  the other half, and the road's right half came out darker with a seam at the exact centre. Checked: all four views
  in a race at 16:9 / 4:3 / the owner's settings (ultrawide, 1080i, DLSS), no seam, the showroom and menus the same
  as before (pixel for pixel at 1080i). Not checked: replays, 32:9 exactly.
  **The mirror at 21:9 / 32:9** (the owner, 2026-11-10: "the cars in there are super wide"): ULTRAWIDE widens the
  game's camera aspects (hle_forza.c), not the mirror's camera, and its view (a clip of part of the AA scene, x 230+179
  of 640) was stretched across the wide target: twice as wide at 32:9. Fixed (not released): ui_squeeze squeezes a
  race's AA 3D draw clipped to part of the width whole to its 16:9 shape (as the track map), clear_rect its rect
  clears, and the MV vertex epilogue the previous position too. Only the AA scene: the mirror's light pass is drawn
  first into x 0+179 of a plain target and looked up by the mirror's draws at coordinates from their unsqueezed
  positions; squeezing it too split the mirror's road down the middle (the owner saw it: "the shadows look wrong").
  Seen on the way: with ultrawide the menus fill only the middle 16:9 (next item).
- **Menus that fill 16:9, 21:9 and 32:9 screens** (requested 2026-11-10, "for the future"). Today, by design
  (render.c ui_squeeze): with ULTRAWIDE the game's 2D (menus, dialogs, loading screens) goes into the middle 16:9 of
  the picture, as wide as it was meant, and only full-width fills (the menus' green rules, fades) and post-processing
  span the screen; on the owner's 32:9 monitor that leaves the menus half the width with black sides (480p and 1080i
  the same; checked 2026-11-10 in the car class, showroom and assists screens). 16:9 screens are already filled.
  Stretching the menus' 640 columns across would distort them (2x at 32:9). Ways, best looking first:
  1) keep each menu's content 16:9 and fill the sides: its backdrop (the white-grey panels, the showroom's grey,
     the menu movies) stretched or extended to the edges, or a blurred, darkened copy of the screen behind them;
  2) move what sits at a menu's edges to the screen's edges, as the race HUD does (hud_side): the title and "My
     Profile" at the top, the button prompts at the bottom;
  3) a Recomp Setting for menus: 16:9 (now) / filled.
  Check every screen kind (main menu, lists, showroom, garage, dialogs over a race, loading screens, Bink movies)
  at 21:9 (ULTRAWIDE=233) and 32:9 (356), and that 16:9 does not change.
  **1) and 3) done 2026-11-12** (not released): render.c menu_sides, MENU SIDES (menusides, MENUSIDES; FILLED by
  default, BLACK as before; in both settings lists). In present(), with ULTRAWIDE, on a frame with no 3D across a wide
  target for the last 8 presents (ui_squeeze marks g_wide3d_frame when a depth-tested draw goes to one: the
  showroom, the grid, a race and its pause menu keep their own sides) and not the race view: the middle 16:9 is
  shrunk to an eighth and blurred there (9 x 5 taps), and each side gets the middle's edge mirrored outwards and
  stretched (its outer edge shows 1/16 of the way in; at 1/6 the menus' titles and text came out as streaks), one tap
  a pixel, 0.6 to 0.2 as bright outwards, blended by MAX so the full-width bars, rules and black bands the game
  draws stay. Checked at 32:9 (Profile Select, main menu, achievements, Arcade's event list; the grid untouched)
  and 21:9 (main menu); 16:9 never runs it. Then at 32:9: the start-up splashes and the intro movie (a soft glow
  either side), Select Car Class, the loading screen (filled); Select Car and its Assists dialog keep black sides
  (3D car). Not checked: the garage, dialogs over a race. 2) (moving the menus' edges to the screen's) not done.
- **Steering wheels** (#27, a Fanatec CSWB V2; the owner, 2026-11-12: on the roadmap "for future addition"). Input
  is XInput only (hle_xinput.c), and most wheels are DirectInput devices, so the game never sees them. SDL3 is
  already linked and reads them (SDL_Joystick: axes, buttons, force feedback through SDL_Haptic). Steps: 1) a wheel
  as controller 1: steering on the left stick's X, the pedals on the triggers (separate or combined axes), its
  buttons on the pad's, so every layout of the game's Controller Options works; 2) a Recomp Settings page to bind
  axes and buttons, with ranges and dead zones (pedals differ by make and are often inverted), kept per device;
  3) force feedback from the game's own vibration (XInputSetState's motors) or better, from the car's steering
  forces, if the game exposes them; 4) the game's own MadCatz and Speedster wheel layouts for their steering
  ratio. Check with a real wheel (none here): ask a player with one to try a test build.
- **Mouse camera look** (the owner, 2026-11-12: on the roadmap "for the future"). In a race the right stick looks
  around and the keyboard has Z / X / C (look left, right, back); the mouse does nothing. Steps: 1) find what the
  right stick drives: the race camera's look angles in each view (two chase, bumper, hood), following the stick's
  value with MEMWATCH to where the camera reads it (every camera goes through 0x1DA620); 2) move those angles from
  the mouse directly, not through the stick: a stick's dead zone and response curve make a mouse feel like a stick
  (#7: Saints Row ignored the first 30% of tilt in Stilwater); only if the angles cannot be reached, a mouse-as-stick
  driver like Stilwater's mouse_look, starting past Forza's measured dead zone; 3) when the mouse rests, the view
  eases back to the front as letting go of the stick does (check what the stick does), or stays (a setting);
  4) the mouse is captured (hidden, raw input, so high-DPI mice count in their own units) only while driving
  (forza_racing) and released in menus, the pause menu and when the window loses focus; 5) settings in Recomp
  Settings and the launcher: Mouse look on/off, sensitivity, invert up/down. Check every view, replays, 16:9 and
  ultrawide, Alt+Tab and the pause menu letting the cursor go.
- **Nothing of 0.2 goes to GitHub until the owner says it is ready to release.**

### Next steps
0. **Draws work** (branch feat/nv2a-draws): pgraph.c (Kelvin state, vertex assembly), shaders.c (vertex programs
   and combiners -> HLSL), texture.c (swizzled/linear/DXT), render.c (D3D11, render-to-texture, AA resolve).
   `SCREENSHOT=<n>` saves build/frame.bmp + every target; `NV2A_DUMP=<n>` dumps a frame's methods;
   `SHADER_DUMP=1` prints generated HLSL. Write-combined RAM alias at 0xF0000000 added.
   The dirty-disc screen was a chain of file-layer bugs, all fixed: write-combined RAM alias (0xF0000000);
   FileBasicInformation times now applied (the installer stamps copies with the disc file's time and checks
   them within 4 s); FileAllocationInformation no longer moves end-of-file; FATX unbuffered writes past EOF
   inside the last cluster keep the title's EOF (installer presizes exactly, then writes whole sectors); and NT's
   rule that a synchronously failing request does not touch the I/O status block / event / APC (XAPI pre-sets
   OVERLAPPED.Internal = STATUS_PENDING, so a prefetch past EOF completes as 0 bytes, not as an error).
   Also added: KPROCESS for XSetProcessQuantumLength, NtUserIoApcDispatcher, Xc* crypto (SHA-1, HMAC, RC4,
   DES parity; DES itself is a pass-through shortcut), MMX in the lifter (99.94% of instructions lifted),
   zeroed APU/AC97/NIC register windows with a running APU sample counter, and a discovery fix for real
   functions hidden under bogus walks (int3/nop padding before a pointer target).
   DirectSound is a silent HLE (hle_dsound.c, 73 entries in hle.txt): buffers are guest memory with a play
   clock, streams complete packets in DirectSoundDoWork. Bink presents through the video overlay, so
   D3DDevice_EnableOverlay/UpdateOverlay are HLE'd and render.c composites the YUY2 surface (BT.601) over the
   back buffer; SCREENSHOT's frame.bmp is now the composited back buffer.
   Video runs at full rate: the guest lock has a 1 ms quantum, and the CRTC start is presented once per vblank.
   **Input** (hle_xinput.c): XPP's XInitDevices/XGetDevices/XGetDeviceChanges/XInputOpen (0x47EB26, found by
   hand)/GetState/SetState/GetCapabilities/Close are host functions. Guest port n = host XInput pad n; the
   keyboard is merged into port 0: arrows = d-pad + steering, W/S = throttle/brake (RT/LT), Enter = Start,
   Backspace = Back, Space/Esc/X/Y = A/B/X/Y, Q/E = White/Black. Each press skips an intro movie.
   Fixed on the way: stream packets are copied at Process (callers pass them on their stack); method 0x310 on
   subchannel 5 is latched in PGRAPH 0x400B10, which D3D_BlockOnTime polls; discovery now also seeds data
   pointers that follow a ret (not jump-table targets), which found 140 missed functions (vtable methods);
   HANGDUMP no longer reads exited threads.
   **Now stops after the intro movies**: the screen stays black, the frames have no draws, and the main
   thread alternates between kernel waits and D3D_BlockOnTime. Next: find what the front end waits for.
   Since then: rcl/rcr are lifted (the front end used `rcr`). The game now reaches the title loop: fonts,
   strings and car media load, and AttractVideo.bik / IntroVideo.bik play through the overlay at about
   30 fps. Late frames, though, hold only surface setup, clears and D3D's anti-aliasing blit: the front end
   issues no draws.
   Update: it does draw, and the front end now renders. The draws reached the
   renderer but produced nothing because of three translation gaps:
   - The ILU result must go to R1 whenever a MAC op runs (D3D's `rcc r1.x, r12.w` prologue).
   - Palettised textures (format 0x0B) were unsupported. The glyph cache is a P8 texture.
   - Texture-shader mode 4 (PASS_THROUGH) was unsupported. Glyph draws pick a colour channel of the cache with
     dot(t0, t1), where t1 is the passed-through texcoord.
   Also added: SET_VERTEX3F/4F, MmCreate/DeleteKernelStack, and a fixed-function transform (composite matrix only;
   not yet exercised). NV2A_DUMP=<seconds> now dumps the first whole frame after that time. SHOTEVERY=<n> saves
   the screen every n frames.
   The game reaches PROFILE SELECT and shows "CAN'T CREATE REPLAY THEATRE" (its replay storage setup on the HDD
   failed).
   Cause, now fixed: open_file reported STATUS_OBJECT_NAME_COLLISION for a directory that the same FILE_CREATE call
   had just made. XCreateSaveGame therefore took every new save folder for an existing one, read an empty
   SaveMeta.xbx and failed. Saves (leaderboards, ghosts, the "Replays" theatre) are now created and the dialog is
   gone. Save folders with an empty SaveMeta.xbx left by older builds must be deleted (two are kept in
   build/save_backup). FILELOG=1 gives the [file] trace without KTRACE. Since then, the game creates a profile and reaches the MAIN MENU:
   - NtQueryDirectoryFile with RestartScan restarted on every pass of its skip-"."/".." loop and hung on any
     subdirectory. It now restarts only once.
   - XAPI fibers (CreateFiber/SwitchToFiber/ConvertThreadToFiber/DeleteFiber) are HLE'd onto host fibers
     (runtime/hle_fiber.c). A lifted SwitchToFiber cannot move the host stack, and the profile code runs on a
     fiber. FIBERLOG=1 traces it.
   - lift.py: a jump into an HLE'd function is always a tail call to it. Discovery had merged
     `jmp SwitchToFiber` targets into their callers, inlining the original code.
   - Overlay: UpdateOverlay's colour key is honoured. The movies use none, so they cover the screen, as on hardware.
     OVLOG=1 logs overlay changes.
   - Saves came back "damaged" after every restart (scoreboards, profiles). XAPI signs non-roamable saves with
     XboxHDKey. When that is all zeros it reads the EEPROM (ExQueryNonVolatileSetting 0xFFFF, 256 bytes) into a
     stack buffer and keys off offset 0x36. We wrote only 4 bytes, so the key was stack garbage, different at save
     and at load. Now XboxHDKey has a fixed value (never change it, or every save turns invalid) and settings
     queries fill the whole buffer. Saves from older builds are invalid; they are kept in build/save_backup.
   - The title only takes A after its loading is done (about 40 s after the press). build/drive.ps1, look.ps1,
     observe.ps1 and to_title.ps1 drive the game interactively: keys go in build/keys.txt, screens come from
     SHOTEVERY.
   - Menus now render fully (title backdrop, Profile Select list and art, MAIN MENU options and engine art). Two
     vertex-program gaps had made every D3D "standard transform" draw vanish:
     - NV097_SET_VIEWPORT_SCALE/OFFSET (0x0AF0/0x0A20) must land in transform constants 58/59 (XF context
       VPSCL/VPOFF, D3D's c[-38]/c[-37]). Left at zero, every vertex collapsed to one point.
     - ILU MOV (and LIT) take the whole C vector. Only RCP/RCC/RSQ/EXP/LOG are scalar. `mov oD0, v3` copied v3.x.
   - NV2A_DUMP frames now carry a `# draw prim/verts/bbox` line per draw.
   **Races run** (2026-11-02): Arcade Race → Series 1 Race 1 → Civic Si → Starting Grid → Start Race. The track,
   cars, HUD, AI, gears and lap timing work; the car drives with W. The fixes on the way:
   - MmAllocateContiguousMemory(Ex) returns PHYS_ALIAS + address, as the kernel does. Forza's 30 MiB texture pool
     checks `(Data | 0x80000000) - base < size` when a screen unloads; with a low base every texture went to
     XPhysicalFree instead, and the race load found the pool empty.
   - DirectSound stream packets complete at playback pace. Completed at once, the WMA music thread decoded ~70x
     real time holding the guest lock, and the race loader starved.
   - rdtsc ticks at 733 MHz: the loader works in slices of 335,000 cycles (0.46 ms on the Xbox).
   - I/O completion APCs queue per thread and run at its next alertable wait. Run inside the read, ReadFileEx
     callbacks found the track-preview loader half set up.
   - guest_alloc reuses freed memory (first fit, coalescing; NtFreeVirtualMemory release, MmFreeContiguousMemory,
     ExFreePool, ...). The bump heap ran out of 120 MiB loading cars. On exhaustion it prints bytes per function.
   - IdexChannelObject's IRP queue (+0x28) is an empty list: XAPI's CancelIo walks it when the menu music stops.
   - Discovery seeds the CRT's x87 transcendental table (fmod/exp/sinh/cosh/tanh handlers at +0x10 of unaligned
     32-byte entries); the race hit a missing tanh handler 13 s in.
   Debugging: `build/dump.now` dumps the next frame, `build/rt.now` saves it and every colour target as SCREENSHOT
   does (build/frame.bmp, build/rt_*.bmp), `build/hang.now` prints every thread and carries on;
   `lift.py --trace runtime/trace.txt` logs calls (capped per function); `MEMWATCH=<addr>` finds writers.
   **Races run at full speed** (2026-11-02): the race clock now keeps wall time (20.0 s per 20.1 s; it was 1.5 s per
   6 s). Creating `build/prof.now` samples every host thread for 5 s (runtime/profile.c) and prints self and
   inclusive host functions (lifted code is `f_<address>`), each thread's CPU use, flips/s and vblanks/s.
   - The game thread spent 97% of its time translating the push buffer: 60% re-hashing every texture's bytes at
     each draw, 20% in sprintf regenerating shader source per draw. Textures are now verified once per presented
     frame with an 8-byte-word hash (hash_bytes), and shaders are looked up by a hash of the generator inputs
     (vsh_key/psh_key) before any source is generated. 16 -> 60 fps; the game thread now uses ~17% of a core.
   - The race clock counts vblanks, and the DPC thread's 60 Hz vblank ran at 48 Hz: the runtime never called
     timeBeginPeriod(1), so each Sleep(1) lasted a 15.6 ms timer tick (also every polling kernel wait).
   - Discovery took a switch's jump table embedded in code (0x241420) for a function start; its walk ran into the
     real vtable method at 0x241430, which then had no entry, and the race faulted when the car first moved.
     Pointer seeds inside known jump tables are now skipped (79 false functions gone, 17 real ones found).
   **Car paint renders** (2026-11-02). A car body draw samples four kinds of texture the renderer lacked:
   - Unit 0, the livery: a 512x512x2 DXT5 *volume* (clean and dirty slices, blended by r). NV2A volumes keep the
     DXT blocks of up to 4 slices together per block position; decoded as 2D, the slices alternated every 4x4
     block (the dotted paint). texture.c now decodes volumes (and 3D swizzle) into a Texture3D.
   - Unit 1, a shadow map: a linear depth texture (LIN_DEPTH_X8_Y24) under PROJECT3D, compared as
     `depth FUNC r/q` (NV097_SET_SHADOW_DEPTH_FUNC, GL order) in depth-buffer units. Depth targets are now
     R24G8_TYPELESS so shaders can read them. The shadow pass sets polygon offset (fill enable 0x0338, factor
     0x0384, bias 0x0388); the bias must push depth away from the light or every lit surface fails the test.
   - Unit 3, a cube map the game renders itself, three faces per frame at base + face * 0x10000: render.c copies
     the face targets into a host TextureCube once per frame. Cube maps in guest memory decode too (faces 128-byte
     aligned). Texture-shader mode 3 samples with the direction xyz.
   psh_generate declares each unit as the view kind that is bound (tex_kind in pgraph.h); a unit that cannot be
   decoded gets a black dummy of its kind.
   **Sound plays** (2026-11-02). hle_dsound.c is now a software mixer: every buffer and stream is a voice read from
   guest memory (16-bit/8-bit PCM, Xbox ADPCM: 36-byte IMA blocks per channel, 4-byte channel interleave, 65
   samples: the header's sample plays too, the only reading under which the game's wave banks run on smoothly
   across blocks; with 64 every boundary jumped, a buzz at the block rate), linearly resampled to 48 kHz with
   SetVolume (1/100 dB), SetPitch (1/4096 octave from 48 kHz) / SetFrequency, play and loop regions, mix bins
   (SetMixBins/SetMixBinVolumes and the descriptors' lpMixBins; each stereo side takes its loudest bin, centre 0.707,
   LFE 0.5, effect sends dropped), and the volumes of the mixin buffers it feeds through SetOutputBuffer. A soft
   limiter above 0.6 keeps peaks under full scale. The title's "distortion" was a 5.1 ambience of eight looping
   buffers the game mutes with SetMixBinVolumes -6400, played at full volume; the WMA music itself decodes
   bit-exactly (checked against Media Foundation's decode of UIMusic.wmapack's "Main title loop mix").
   Then it was still "glitchy": XACT plays a wave-bank entry as SetBufferData(whole bank), SetPlayRegion(entry),
   SetCurrentPosition(0), Play, so cursors and loop offsets count from the play region's start, which is also where
   the entry's ADPCM blocks begin (stereo entries can start half a 72-byte block into the bank). With positions
   from the buffer start, every UI, tyre and collision sound played the bank from its beginning (9 s of other
   sounds) and stereo entries decoded half a block off. Creating `build/voices.now` prints the five loudest voices
   every 250 ms for 10 s (format, rate, position, regions, gains, data); that is what found it.
   audio.c plays the mix through WASAPI (shared mode, 40 ms), or consumes it on a timer without a device. Buffer
   positions and stream packet completion follow what has been mixed. `DSLOG=1` logs formats and setter counts
   (2: every Play and setter value); creating `build/wav.now` records 20 s of output to `build/audio.wav`.
   What a race uses: ~1,100 buffers, mostly mono Xbox ADPCM at 8-44.1 kHz, per-frame SetPitch/SetVolume (engine),
   and full 3D: SetPosition/Velocity/Mode per source and Calculate3D + Set3DVoiceData (~3,000 a second).
   3D sources are all DS3DMODE_HEADRELATIVE (positions in metres from the listener), min distance 1-2, max 50-175,
   with one-point rolloff curves (0 or 0.00063): the curve's points sit evenly up to the max distance after an
   implied 1.0 at the min distance (read as a constant, it muted every 3D voice). The mixer attenuates by that curve
   (else min/distance) and pans by x; volumes multiply down the SetOutputBuffer chain of mixin buffers.
   **Audio effects** (2026-11-02), from the first 200 calls of each setter in a race (DSLOG=1 logs them):
   - SetI3DL2Source (54k calls): lDirect -600 on most sources (they were 6 dB too loud), lDirectHF -116/-52,
     obstruction -300 at LF ratio 0.25; lRoom -1200 sends. Direct gain = lDirect + obstruction/occlusion x LF ratio;
     the HF part (lDirectHF + the rest) is a first-order high shelf (bilinear one-pole low-pass at 2.5 kHz): unity
     at DC, exactly the loss at the top, -4 dB at 5 kHz for -6. A plain low-pass matched at 5 kHz kept falling and
     cut a race's energy above 8 kHz five-fold.
   - Doppler factor 0 or 0.5 per source; velocities in m/s, head-relative: f' = f 343.3 / (343.3 + v_r factor).
   - Cones: inside 110/220/360 degrees, outside 360, -300 outside: linear in dB between the half-angles.
   - All of it, like distance and mix bins, multiplies down the SetOutputBuffer chain (3D is set on the mixins).
   Not modelled: reverb (SetI3DL2Listener is never called, so there is no room to send to), SetFilter (38k calls,
   modes 2/3 = ParamEQ/"multi" with four 16-bit hardware coefficients of unknown format, e.g. BAB9 0999 A828 CB4B),
   envelopes (menus only: near-instant attack/release), LFOs and headroom (never set).
   **Select Car showroom** (2026-11-02): the 3D car rendered after the texture work, but floated on black. The
   screen draws the car mirrored (the reflection, culling the other face), a full-screen fade and the floor quads
   (shadow), then NV097_CLEAR_SURFACE with flags 0x83 (depth, stencil and only alpha) before the real car.
   render_clear cleared all of RGBA, wiping the backdrop, floor and reflection; a clear of some colour channels is
   now a target-filling triangle through a write mask (masked_clear). Stencil is written there but only ever tested
   with ALWAYS, so it stays unimplemented. `build/select_dump.ps1` drives to the screen and dumps a frame.
   **Other tracks** (2026-11-02): Silverstone Circuit Short (Series 1 Race 2) and Tsukuba (Race 3) render and sound
   right, like Maple Valley. `build/event_screen.ps1` drives to the Arcade event list; `build/race_at.ps1 -Rights n
   -Name x` then races the event n steps right (screens build/x_<k>.png, audio build/x.wav, frame build/x.frame.txt).
   One fix: a 32x2 DXT5 menu texture (6 levels) failed to upload 11,024 times, since D3D11 wants a block-compressed
   top level a multiple of 4 each way; such textures are now decompressed to BGRA8 (dxt_block in texture.c).
   **Enhancements: widescreen** (2026-11-02). Forza renders native anamorphic 16:9: its display-mode picker
   (0x1DA960) walks a table at 0x4C3390 {width, height, progressive, widescreen} - 1920x1080i, 1280x720p, 720x480p/i
   wide, 640x480p/i - accepting modes through 0x1D96C0 against XGetVideoFlags (EEPROM setting 8 >> 16: 1 widescreen,
   2 720p, 4 1080i, 8 480p; HD bits kept only on the HDTV AV pack, HalBootSMCVideoMode 1). The chosen mode sets the
   widescreen byte 0x5DA7EA and the camera aspect 0x554F00 (4/3, 16/9, or 8/3 / 32/9 split-screen): MEMWATCH showed
   16/9 once the kernel reported the widescreen flag (now the default; WIDESCREEN=0 for 4:3). The frame is composed at
   RENDER_SCALE and stretched to a 16:9 window; 2D menus stretch as on a widescreen TV of the time.
   (A first check compared c[96..98], which turned out to be the shadow map's light matrix, not the camera.)
   Off by default: VIDEOMODE=720 / 1080. Every caller caps the mode width at 640 (push 0x280), so retail Forza never
   leaves 480 lines; hle_forza.c replaces 0x1D96C0 with the same check and the cap raised. The game then creates a
   1280x720 (or 1920x1080) back buffer with 2x horizontal AA, and presentation follows the framebuffer's size. Only
   the post-processing is HD-aware (bloom chain 640x360, 640x180, 320x180, a 1280x720 composite; the dialog backdrop
   blur 2560x360 -> 1280x360). Menus, HUD and the race itself still set viewports, which D3D makes the surface clip,
   inside 640x480 (over 150 sites push 640.0f), so they drew into the top-left corner. pgraph_surface stretches
   such draws over the HD back buffer (`Surface.ui_w/ui_h`: the extent they were drawn in; the shader maps that to
   the whole surface), except viewports of 1/2 and 1/4 the HD size (hd_part). It also stretches the bloom's
   480-line 2x2-AA pass (clip 640x240), which then covers the whole buffer as it does at 480 lines. A front-end draw
   sampling an HD-sized target addresses it in 480-line texels: the Assists dialog draws its blurred backdrop
   (1280x360) with texture coordinates 640x240, so texture() scales such targets for ui draws. D3D pads tiled
   pitches (1920 x 2 AA x 4 = 15360 -> 16384), so the back buffer is recognised by a pitch up to 1/4 over its row.
   Checked at 720p: Profile Select, main menu, event list (with its flyover), car class, Select Car showroom,
   Assists, Starting Grid, a race at Silverstone (HUD, track map, bloom); the same at 1080.
   1080 needs more memory: its back buffers take ~25 MB more than 720p's, and loading a race exhausted the 128 MiB
   guest heap ("want 1000", 25 MB free in 423 fragments; 88 MB of it XPhysicalAlloc'd pools). With VIDEOMODE 1080
   guest RAM is 192 MiB (GUEST_RAM_END is now g_ram_end; the write-combined alias must end below the NV2A
   registers, 0xF0000000 + RAM <= 0xFD000000). D3D hard-codes its memory DMA contexts for a 128 MiB devkit (limit
   0x07FFAFFF at 0x3C1393..0x3C14A9), though its offsets go higher ("bad DMA access: context 3 offset 085E1000"), so
   hle_CMiniport_CreateCtxDmaObject raises that limit to the RAM's end when there is more. A 1080 race keeps real
   time (race clock 20.5 s over 20.8 s of wall time, ~59 frames presented a second; the profiler's "90 frames/s
   flipped" counts display start writes, which 1080i makes more of).
   (`build/hd_path.ps1`, `build/hd_grid.ps1 -Mode n`.)
   **QUIT GAME** (2026-11-05, requested): an eighth main menu item after RECOMP SETTINGS opens a screen of the same
   kind titled QUIT GAME with one row, YES, QUIT (ExitProcess, as closing the window); Back returns to the main menu.
   Its title and the main menu item are one row of the settings' list that no screen lists (screen -1).
   **Enhancements: Recomp Settings menu** (2026-11-03). A seventh main menu item, RECOMP SETTINGS, opens a screen of
   this port's settings; A cycles a row's value, saved to recomp.ini [recomp] next to the exe (environment variables
   still win, for scripts); the game keeps the values it started with, so they apply at the next start. How
   Forza's menus work (hle_forza.c):
   - A menu screen's constructor adds items with 0x199580(label, description, icon, factory, 0) (or 0x198AA0 with an
     icon object): label and description are LocStrings {own text, -, string table 0x5BB308, u16 id}, the icon is an
     index into the screen's icon bundle (ico_Main.xpr, ico_main_options.xpr), and the factory is a function
     returning the next screen. Selecting the item calls it (holder 0x197BA0); no screen counts as coming back.
   - The main menu is 0x35B040 (items 218 ARCADE RACE .. 232 OPTIONS, ids held in u16 globals 0x4AFE98..), Options
     0x15A870 (0x2160 bytes, made by 0x35AE60). The list shows six rows and scrolls for a seventh.
   - Text is resolved whenever drawn by StringTable::Lookup (0x340C0); hle_ForzaStringLookup answers ids from
     0xF000 up (the game's tables stop at 10010) with the current values, so a row's label changes as it is cycled.
   - The settings screen is built like Options: 0x199250(0, title, ico_main_options.xpr, bkgd_OP_main.xpr, empty,
     "UIFullScreenMenu"), the Options vtables, then one item per setting whose factory cycles it and returns 0.
   - Descriptions get three lines of about 37 characters; longer ones lose their start.
   Found with two new debugging aids: `build/ram.now` writes guest RAM to build/ram.bin (file offset = address), and
   `MEMWATCH=r<address>` logs reads of a dword (armed by `build/watch.now`, since a file read into the locked page
   fails) with the code addresses on the guest stack. `build/menu_settings.ps1` walks the menu.
   The window is 16:9 in the HD modes even with widescreen off: 720p and 1080i have no 4:3 form.
   **Display options** (same day), three more rows:
   - RENDER SCALE 1X-4X (renderscale, RENDERSCALE; default 2): RENDER_SCALE is now the variable g_scale, read at
     startup, since every host target is made at that scale.
   - FULLSCREEN (fullscreen, FULLSCREEN): borderless, on the window's monitor (style without WS_OVERLAPPEDWINDOW,
     sized to the monitor; back with the saved WINDOWPLACEMENT). Applies at once (WM_APP to the window thread);
     Alt+Enter does the same and saves it (DXGI's own Alt+Enter is off: MakeWindowAssociation NO_ALT_ENTER).
   - TEXTURE FILTER OFF/2X-16X (anisotropy, ANISOTROPY; default 8): D3D11_FILTER_ANISOTROPIC for the textures the
     NV2A filters linearly. Applies at once: render_anisotropy drops the cached samplers (raster table, key bit 62),
     since a full state cache is fatal.
   - The window is resizable: the swap chain follows its client size (ResizeBuffers in render_present, skipped while
     minimised) and the frame is fitted at the picture's aspect (16:9 widescreen or HD, else 4:3) with black bars;
     the size dragged to is saved (window_w, window_h, on WM_EXITSIZEMOVE) for the next start.
   - The window's place is saved too (window_x, window_y; used while it is on a screen).
   - A BORDERLESS WINDOW row (frameless, movable, its own size) went (2026-11-05): the player turned it on to fill
     the screen, which FULLSCREEN, borderless already, does. The launcher calls FULLSCREEN "Borderless fullscreen";
     the game's menu keeps FULLSCREEN (the longer label wraps there) and says "Borderless" in its description.
     Checked on the 5120x1440 monitor: starting with fullscreen=1, the window 0,0-5120x1440, WS_POPUP; the menu row
     off, on (5120x1440, the picture edge to edge), off (the 1974x953 framed window back in its place).
   Checked: starting fullscreen at scale 1 on a 5120x1440 monitor (16:9 picture with side bars), FULLSCREEN and
   Alt+Enter both ways, a 1200x900 window (bars above and below). Test scripts expect a 16:9 window: to_title's
   title detector misses on a 32:9 fullscreen frame, so they run without recomp.ini.
   **Ultrawide** (2026-11-04; ULTRAWIDE, recomp.ini ultrawide, a row in both settings lists; a 16:9 picture:
   widescreen, or VIDEOMODE 720 / 1080): the picture takes the primary monitor's aspect (ULTRAWIDE=n >= 100: n / 100, for tests). Three
   parts, each in one place:
   - The view: the camera's aspect 0x554F00 is set from constants 0x495CDC (16:9) and 0x495CE0 (32:9, split screen)
     at 0x95A54 and 0x1DA9C0; forza_init scales both. The game keeps its vertical view (45-degree FOV at 0x4911A4)
     and sees more to the sides; it culls by the same frustum, so cars at the edges are drawn.
   - The screen's targets (480 lines, 640 or 1280 wide) are made g_widen times wider on the host (Target.hw, .hh);
     the game still draws them in 640 columns, so 3D lands right. Every guest-to-host pixel step for a target uses
     its own width (viewport, scissor, clears, readback); the frame is fitted at the wide aspect; the video overlay
     goes into the middle 16:9.
   - 2D (ui_squeeze): with the depth test off, on a wide target, positions with w 1 (or 0, as the text program
     leaves it) are squeezed into the middle 16:9 about the screen's middle (Constants.ui; the scissor too). Not
     squeezed: post-processing (reads a render target over all of its clip: the composite, bloom, a dialog's blurred
     backdrop) and full-width fills whose vertices differ only in position (fades, the pause menu's darkening, the
     menus' bars), which span the screen. The HUD's track map is a 3D view in a viewport of its own (clip 59+128):
     a draw clipped to part of the screen is squeezed whole and gets the screen's viewport, else D3D cuts it off.
   - Lock2DSurface of a wide target reads back its middle 16:9 (the car icon is copied off the screen).
   Checked at 21:9 (237) and 32:9: main menu, Select Car, Starting Grid, a race (HUD and track map), the pause menu,
   the intro video (pillarboxed), DLSS at 32:9 (60 frames a second at render scale 4), and off (as before). Menus'
   full-screen pictures (Select Car's backdrop) are pillarboxed; the 3D behind the grid and pause menus fills the
   screen. Not checked: a painted car's icon, split screen.
   VIDEOMODE 720 / 1080 too (same day): their back buffers (the HD size, AA included) are widened as the 480-line
   ones; their front-end draws (ui_w: 480-line code stretched over the HD surface) are squeezed about x 320, and the
   full-width test takes their clip back to 480-line units. A host target is at most 16384 wide (D3D11): 720p's
   2560 (AA) at render scale 4 and 32:9 would be 20480, and CreateTexture2D failed at the first frame; a wide
   target past it is narrowed to 16384 (everything takes its own width, the jitter included). Checked at 720p and
   1080i, 32:9: main menu, Select Car, the grid, a race with its HUD and track map.
   Played on a 5120x1440 monitor (2026-11-05), two faults and a wish:
   - Select Car's car was twice as wide at 32:9. Every camera goes through 0x1DA620 (a struct copied to 0x5DA8B0:
     +0 FOV, +8 aspect, +0x10/0x14 near/far, +0x28 0 by FOV, 1 width x height, 2 off-centre); a PROJLOG of its
     callers showed the race's at 3.556 and the showroom's (0x33AC0F) at 2.03: 0x33AB9E makes it its viewport's
     width / height x 0x495CD4 (4/3, how much wider a 16:9 TV shows a pixel than 640x480 does). forza_init scales
     that constant too; its other readers (0x95A92, 0x1DA9E7) set a 4:3 picture's camera, which ULTRAWIDE leaves.
   - The race HUD now goes to the sides (hud_side): while CGameWindow or CCountDownWindow is on top
     (forza_racing), a squeezed draw whose middle is in the left third of the 640 columns is squeezed about the left
     edge (keeps its distance from it), in the right third about the right edge, else about the middle. Where a
     draw lands is its vertex program's oPos, run on the CPU for its vertices (shaders.c vsh_position, the HLSL
     generator's instruction set; vsh_selftest at startup): the positions it is given are not always the screen's
     (PLACE came at x -73, a piece at x 4488). The track map goes by its viewport. Map and PLACE land at the left,
     lap times and the speedometer at the right; menus and the pause menu stay in the middle 16:9.
     Replays too (2026-11-05, reported: the HUD stayed 16:9 in replays, seen in the third-person camera): a replay
     has CReplayOverlay (vtable 0x4BEF3C) over the race, so hud_side asks forza_race_view (race, countdown or
     replay); the keyboard still asks forza_racing, so a replay keeps menu keys. The replay's control bar (any
     button shows it: pause, rewind, Change Camera, exit, ...) is drawn by the same screen and was torn apart by
     the thirds; its top (overlay +0x2A4, 480-line units, found by diffing the overlay with the bar up and away)
     slides to 385 when shown and rests at 481, so the replay counts only while it is >= 480 (the HUD is hidden
     while the bar shows anyway). Checked: bar whole in the middle; third-person camera, map and PLACE left,
     times and speedometer right.
   - Popups over a race or replay (PopupMenu, "Exit replay?") showed the frame in three panels: their backdrop,
     the frame copied and darkened, is a quad from -0.5 to 639.5, half a pixel short of the post-processing test
     (all of the clip), so it was squeezed into the middle over the live picture. The test allows a pixel now.
   - Shadows "stop at 16:9" at the speedway's start: seen once, not again (dropped). The car's shadow is drawn in
     a patch about the car (its map's projection, 0x1C2FCB: 5.47 x 5.47 orthographic), whose curved edge the
     countdown camera shows near the 16:9 line at 32:9, where 16:9 has the screen's edge; the likely sight.
   **Redline Recompiled 0.1.0** (2026-11-03): the port's name and version (runtime/version.h) in the window title,
   the startup log, the launcher and the bottom-right corner of the window (render.c version_text: Segoe UI drawn
   with GDI into a texture whenever the window's height changes, white over a 1-pixel shadow, 80% opaque, a 60th of
   the height). The exe is build\redline.exe, its window class RedlineRecompiled.
   **Controllers** (hle_xinput.c): host pads through SDL3 (third_party/SDL3, 3.4.18, zlib licence; SDL3.dll is
   copied next to the exe) instead of XInput, so PlayStation, Switch and generic pads work as well as Xbox ones,
   with hot-plugging. SDL runs on its own thread (pumping events, copying each pad's state under an SRW lock; rumble
   is resent every 500 ms, as SDL rumble has a duration and the Xbox's lasts until changed). Buttons go by position
   (Cross = A); the shoulders are Black (right) and White (left). Port n is the n-th pad; the keyboard is merged into
   port 0.
   **Keyboard: two sets of keys, rebindable.** Forza's own layout (Controller Options): RT accelerate, LT brake, left
   stick steer, A e-brake, B gear up, X gear down, Y look back, right stick look, Black camera, White HUD, Start
   pause. Escape should pause a race but mean Back in menus, so the keyboard has driving keys (W/S/A/D, arrows too,
   Space e-brake, E/Q gears, C look back, Z/X look left/right, V camera, H HUD, Esc pause) and menu keys (Enter or
   Space select, Esc back, arrows or WASD, X, Y, E Black, Q White, PgUp/PgDn triggers, Tab Start, Backspace Back).
   Which set applies: the UI manager (hooked at 0x98230, Switch(old, new), thiscall; the screen on top at +4)
   shows CGameWindow (vtable 0x4958E4) during a race, CCountDownWindow (0x4BCC04) over it at the start; any other
   screen on top (the pause menu is 0x4BD88C, the pre-race card CPreEventOverlay 0x4BDD1C) means menu keys. The
   vtables' RTTI (COL at vtable-4, type descriptor name) names the classes. A key held when the set changes counts
   only once released, so the Esc that pauses does not also resume. Recomp Settings has DRIVING KEYS and MENU KEYS
   pages: choose a row, press a key (recomp.ini key_<name> = virtual-key code); each binding also has a fixed second
   key (arrows beside WASD). Menu rows are told apart by their holder: the factory runs with ecx = the item, whose
   address AddItem leaves last in the screen's item vector (+0x2008 vector; end at +0x2010). hle_thunk now hands
   out one address per host function.
   **Button prompts** (icons.c): every prompt glyph comes from one 256x64 DXT5 atlas of 18x18 cells (texture key
   BEB57C2ECC2E8E2F: A B X Y, triggers L R, Back, Start, Black, White, d-pad arrows). Its replacement is the atlas
   at 4x with the cells redrawn by GDI (4x4 supersampled) for the device last used: key caps with the menu keys'
   names for the keyboard (rebinding redraws them), Cross/Circle/Square/Triangle, L1/R1/L2/R2, OPTIONS and SHARE
   for PlayStation pads, LB/RB/LT/RT/menu/view for Xbox One/Series pads. BUTTON PROMPTS (prompts: AUTO, KEYBOARD,
   XBOX, PLAYSTATION, ORIGINAL) can fix the set.
   **Upscaling: FSR 1** (render.c fsr): AMD FidelityFX Super Resolution 1 (MIT), EASU then RCAS (sharpness 0.2
   stops), written from ffx_fsr1.h with loads instead of gathers, when the picture in the window is larger than the
   composed frame (with RENDER SCALE 1, or a window bigger than scale x frame); else bilinear. UPSCALING
   (fsr) switches it. DLSS (and FSR 2/3, XeSS) need per-pixel motion vectors, which a fixed 2005 renderer never
   makes; they could come from running each NV2A vertex program again with the previous frame's constants, a
   project of its own.
   **Texture packs** (texpack.c, tools/texpack.py): a texture's key is a hash of its guest bytes (palette included),
   colour format and size. Every folder under build\textures\ except dump\ and icons\ is a pack of
   <key>.png/.jpg files of any size; textures\icons\<keyboard|xbox|playstation|other>\ hold per-device ones.
   Replacements load on a worker thread (WIC, box-filtered mips; the D3D11 device is free-threaded) and replace the
   original once ready; unnormalised (rect) textures keep the original's coordinate scale. TEXTURE DUMP (texdump,
   TEXDUMP) writes each texture's top level once to textures\dump\<key>.dds as decoded (DXT blocks or BGRA); a
   texture whose contents change within 60 frames is animated and only its first picture is dumped. TEXLOG=1 names
   each replacement as it is first used. tools/texpack.py makes the 4x pack (textures\redline4x) from a dump with
   Real-ESRGAN (realesrgan-ncnn-vulkan, x4plus model, build\realesrgan): colour and (if any) alpha separately, wrap-
   padded 16 px for seamless tiling, then one step of back-projection, because the model smooths grass, asphalt and
   gravel to mush (the original minus the upscale shrunk back, enlarged and added; RMSE of the plain upscale against
   the original was 21 for grass, 3 for a building texture). At most 2048 a side; opaque ones as JPEG 95 (4:4:4),
   the rest PNG. Left out: under 16 px, one colour, 90%+ transparent (the font caches text is drawn into), and one
   colour shaped by alpha (font sheets and masks): the UI fills boxes from single texels of the font sheet, and the
   upscaled sheet made the main menu's black bars translucent. The first dump (title, menus, Silverstone with 8
   cars) gave 512 textures, 436 in the pack, 99 MB, in 80 s on an RTX 4090. The pack grows by playing with the dump
   on and running texpack.py again (it only does new ones).
   **The whole disc** (tools/texextract.py): every texture is read from the game files with the renderer's key, so
   the pack needs no playing. Three containers: XPR0 (resource headers, then data at header size + Data); Forza's
   resource bundles, magic AAAAC0C0 (track .xb2 as a chain of 4 MiB zlib streams, car.carbin...): 0x12E bytes of
   header (word 2 count, word 6 name-table size, word 10 header-table size), entries {hash, offset, stored, size,
   name offset, header offset} of 24 bytes, a word, the names, then the D3D headers, 20 bytes per texture (Format
   with the border bit 0x8 set) and 12 per vertex buffer; a texture entry's bytes are the texture. The same
   container without a header table is an archive (.cab/.ca2), entries zlib'd when stored < size. The hash is the
   runtime's own (tools/texhash.c includes pgraph.h, built into build/texhash.dll). 902 files, 39,411 textures,
   31,117 different, in 30 s. Every runtime-dumped texture of 64 px or more was reproduced (all but two the game
   makes itself); the misses were 32x32 and smaller, which turned out to be lower mip levels: the game draws a
   texture from a level down (Silverstone's 128x128 concrete as 32x32 from level 2, the key matching exactly), for
   distance or while streaming. Those keys (137,950, every level of every texture) go to dump/aliases.txt and the
   pack's aliases.txt, and the loader gives them the whole texture's picture: full detail where the game drew less.
   The loader keeps at most 4 GB of replacements, dropping the least recently used (a track at 4x is ~1.7 GB);
   texpack_get hands the renderer a reference of its own, since the worker may drop its own any time.
   Font files are left out by source (dump/nopack.txt, from texextract): Arial_16.xpr is the sheet the UI fills its
   boxes from. The full pack: 32,937 textures, 9.7 GB, ~90 minutes on an RTX 4090. At Tsukuba, a track never
   dumped in play, 427 replacements were used (250 through aliases).
   Fixed later: Maple Valley's start straight came out black with coloured specks. Its asphalt (1EC1A9FA7615EE10,
   DXT5 1024x256) uses alpha as a blend weight, not coverage, and Pillow premultiplies RGBA to resize it, so the step
   down from 4096 to the 2048 cap turned the colour black wherever alpha was 0. texpack.py now resizes channel by
   channel; the 1,255 textures that had been scaled down with alpha were made again. Found with build/texkeys.now:
   it limits replacements to the keys listed in it (or "all") while the game runs, so a script parked on the glitch
   (build/texmin.ps1, delta debugging over the keys TEXLOG names) narrowed 333 keys to one in minutes, where
   rebooting per try with key-bit selections gave contradictory answers (the car never stopped in the same place).
   **Upscaled cutscenes** (tools/vidpack.py, video.c, hle_bink.c): 37 Bink videos (35,192 frames; the intro 113 s at
   640x480, attract 10 s, the rest 486x364 event and track previews in the menus). vidpack decodes each with ffmpeg
   (build/ffmpeg, BtbN's LGPL 9.0 build: Bink decoder, NVENC), upscales with Real-ESRGAN and the same back-projection,
   scales to 1920x1440 and encodes H.264 (NVENC, cq 19) to build/videos/<path under media>.mp4, as TV-range BT.709:
   Media Foundation decodes H.264 as TV range whatever the stream signals, and the full-range files ffmpeg made from
   the JPEG frames by default came out too contrasty (matched against the Bink frames: in-game frames then fit the
   upscale at 1.1 mean difference, the original at 2.6). The game keeps
   playing the Bink files (timing, sound): CBinkWrapper::Open (0x3A99B0, handle at +0x48) and BinkCopyToBuffer
   (0x3ACF90; 0x3AD880 looked like BinkOpen by its two arguments but loads the pixel converter sections) tell
   video.c which video is open and which frame (handle +0xC, 1-based) went to which buffer: Forza alternates two
   linear 640x480 textures. Each video decodes on its own thread with Media Foundation (RGB32 through its video
   processor; COM must be initialised on that thread or reads silently stall; its buffers have no IMF2DBuffer, so rows
   go by the media type's default stride), a few frames ahead, into a ring of 8;
   the renderer draws the newest decoded frame not past the one Bink copied to the buffer being sampled, from a
   texture per buffer (texture() and the overlay). A buffer counts as the video's only while its first and last
   4 KiB hash as Bink left them: after the intro its two textures' memory went to the event screen, which then
   showed the intro's last frame (the Forza logo) until that check. The menu previews copy to one buffer. Opening the reader takes ~2 s, so a video's first frames show
   the original. 1920x1440 decodes at the video's 30 fps on the CPU. UPSCALED VIDEOS (videos) switches it;
   VIDEOLOG=1 and BINKLOG=1 log it.
   **Launcher** (launcher/launcher.cpp, `python tools/build.py --launcher` -> RedlineLauncher.exe at the root): pick an
   ISO or an extracted folder, Build, Play. Build runs the pipeline above from the launcher's folder (extracting an
   ISO to game\, checking Forza.xbe's SHA-1 e60a968e..., the analysis steps once, then lift and build), with the
   output in its log; it checks for Python 3 (offering pip install capstone pillow numpy) and the Visual Studio C++
   tools (vswhere; build.py and oovpa_pp.bat now find vcvars64.bat with vswhere too, not a fixed path). Play starts
   build\redline.exe with the game folder. Nothing of the game ships with the port: it is recompiled on the
   player's PC from their own copy.
   Redrawn after Pinyon Shift's launcher (2026-11-03): Direct2D and DirectWrite (so C++, written C-style), a dark
   page with a tachometer whose redline is the accent, one main action at a time: "Build your copy" (drop zone,
   choose disc image or folder), building (step, percentage, a bar with a light running along it, details and
   Cancel, which ends the step's process), "The build stopped" (the log's last real line as the reason, the log
   shown), "Ready to race" (Play, Settings, the settings in one line). Settings edits build\recomp.ini, the same
   keys as the game's Recomp Settings. The footer opens the saves (<game>\..\hdd\Partition1\UDATA), the game's
   log (Play now starts it with its output in build\redline.log), the game folder, and rebuilds. Per-monitor DPI,
   a dark title bar, drag and drop. build/launcher_states.ps1 pictures each page with PrintWindow (no focus).
   **Stability pass** (2026-11-03). UILOG=1 logs each screen as it comes to the top with its class from RTTI
   ("[ui] top screen 0364FFD0 CGameWindow"), so test scripts wait for screens by name instead of sleeping:
   build/ui.ps1 (Tap, WaitUi, Screen, Grab), build/hotlap.ps1 (boot to Free Run > Hot Lap on track n: every track is
   unlocked there, where Arcade has three), build/sweep.ps1 (every track in one session: race, pause > Quit > OK
   back to the list, step right). The main menu does not wrap, so scripts reach Free Run as eight downs and two ups.
   Found and fixed: the render target table (32) filled up on the second race of a session ("too many render
   targets"), as each race puts its surfaces somewhere new and targets were only ever replaced when resized; a full
   table now gives up the target unused longest (64 slots). Maple Valley's road (see Texture packs).
   **Stutter** (2026-11-03, "lags a lot, especially in a race"). FPSLOG=1 prints every 2 s the presents per second,
   their spacing (average, worst, how many over 20 ms), time presenting, push buffer and texture hashing per frame
   and shaders compiled (FPSLOG=2: every spacing). GILLOG=1 prints how busy the guest lock was and, per thread, time
   held, longest hold and waits. The game averaged 60 but a fifth of the frames came over 20 ms after the last
   (worst 40-60 ms), menus and races alike, at any render scale. Four causes, all fixed:
   - Upscaled videos left on a menu kept their Media Foundation decoders, ~8 threads busy each, 3-4 of the 8 cores
     gone during a race: a video not copied for 1 s now closes its decoder (a copy restarts it, seeking).
   - The videos decoded in software: they now decode and convert on the GPU (D3D manager, hardware transforms,
     advanced video processing; frames stay textures, the renderer samples the ring texture itself; the device is
     multithread-protected for it). Software decoding remains the fallback.
   - The DPC thread slept 1 ms between looks at the clock and then queued for the lock behind every thread, so a
     vblank came up to a few ms late, and the game paces its frames by it: it now waits on a high-resolution timer
     for the vblank's time, at TIME_CRITICAL, and while it waits for the lock at a vblank, holders hand over at their
     next safepoint instead of finishing their quantum.
   - Shaders compiled during play (~35 ms each; 53 at a race's start): compiled bytecode is kept in
     build\shadercache\<source hash>.vs/.ps, so each compiles once ever.
   After: 0-1 frames over 20 ms per 2 s in menus, cutscenes and races (Maple Valley, Silverstone). SHOTEVERY's
   screenshots read the GPU back and stall it, so measure without them (to_title/hotlap -Every 0). The device is
   now created on the high-performance GPU (IDXGIFactory6 by GPU preference), not adapter 0, and logged.
   Test input no longer needs the focus: TESTKEYS=<file> makes the game read held keys (virtual-key codes, one a
   line) from the file and ignore the keyboard, with the default bindings; drive.ps1 and ui.ps1 (Tap, Keys, Held)
   replace the file whole, and the window opens without activating, so tests leave the desktop alone. INPUTLOG=1
   logs those keys and every pad state change. Menu taps are 80 ms: 150 ms held long enough for the game's own
   repeat now and then, moving two rows.
   **Enhanced audio** (audiofx.c; ENHANCED AUDIO / enhaudio, on by default; OFF mixes exactly as before):
   - Buffers resample with an 8-tap Kaiser-windowed sinc (256 phases) instead of linear interpolation, the cutoff
     following the pitch (6 tables, 0.95 down to 0.28 of the source's Nyquist) so pitched-up engines do not alias.
     The frames a 256-frame stretch reads are gathered once (loops wrapped, the end silent); ADPCM keeps its last
     two blocks decoded, since taps straddle a block edge. Streams (music, speech) stay linear: they play at 48 kHz.
   - The game sends 3D sources to I3DL2's room at -12 dB (lRoom, occlusion applied) but never sets a listener, so
     the Xbox's reverb ran on nothing; the sends now feed a Freeverb-style stereo reverb (8 combs and 4 allpasses a
     side at 48 kHz lengths, feedback 0.80, damping 0.35).
   - The mix then gets a +3 dB low shelf at 100 Hz, +1.5 dB high shelf at 10 kHz, side signal x1.25.
   In a 20 s race recording: 8-16 kHz +3.8 dB, above 16 kHz -1 dB (less aliasing), side/mid -13.3 -> -7.4 dB,
   level +1 dB; no new clicks (the loudest highs, a wall scrape, are the same event in both).
   **DLSS and FSR 3** (UPSCALING: FSR 1, DLSS, FSR 3, BILINEAR; fsr = 1, 2, 3, 0). Both rebuild each frame from
   earlier ones, so the scene now has what they need:
   - Motion vectors. While one runs, the scene's draws (depth-tested, into a target at least the presented frame's
     size: not the shadow maps, reflections, post-processing or menus) use variants of their shaders (#define MV):
     the vertex program's body runs a second time with cp[] (b1) in place of c[], the constants that draw had a
     frame ago, and the pixel shader writes a second target, R16G16F, the screen-UV motion (a frame ago minus now,
     from interpolated clip x, y, w, divided per pixel). A draw finds its last-frame self by a key: program, vertex
     and index counts, topology, its first vertex's bytes, and its count among draws of that key this frame (99% of
     a race's ~430 scene draws match; MVLOG=1 counts them). Skinned or CPU-animated vertices move without their
     constants changing, so they show the camera's motion only. MVSHOW=1 shows the motion instead of the picture.
   - Jitter: the scene's positions move by a Halton (2, 3) point inside the input pixel, 16 a cycle.
   - The game runs a frame or two ahead of the screen, so motion and depth are kept with their frame: when a draw
     reads the scene target into another (the anti-aliasing resolve into the frame that will be shown, the first
     read after the scene's draws), both are copied off under that frame's address, with the frame's jitter, and the
     next frame's jitter and constants begin. render_present finds them by the address it shows.
   - At present the motion and depth are taken to the frame's size (the scene is drawn twice as wide, for the game's
     anti-aliasing) and handed, with the composed frame (HUD included), to up12.cpp. Never smaller than the input: a
     frame larger than the window is anti-aliased at its size (DLAA) and then shrunk. Once per game frame; a frame
     shown again reuses its result. Frames without a scene (menus) keep FSR 1.
   Both run on a D3D12 device on the same GPU (by LUID): AMD ships FSR for DirectX 12 and Vulkan only, and NGX's
   D3D11 path, which shares textures with CUDA, filled a 128x128 tile of the output with an RGBA8 target and showed
   a texture from the pack with RGBA16F. The devices share the frame, depth, motion and output textures (NT
   handles) and a fence: D3D11 copies the inputs and signals, the D3D12 queue waits, runs the upscaler between
   COMMON <-> read/UAV barriers and signals back, and D3D11's GPU waits before drawing the output. Two command
   allocators alternate, each reset only once the queue is past its last frame. DLSS: NGX D3D12 with a project id of
   our own, quality mode by the actual ratio (DLAA to Ultra Performance), motion at the input's size, unjittered,
   scaled by the input's size, LDR. FSR 3: the FSR 3.1 upscaler through AMD's signed loader
   (amd_fidelityfx_loader_dx12.dll, ffx API), non-linear colour space, a light sharpen (0.3), and a racing camera's
   planes and field of view (near 0.1, far 3000, 0.95 rad), which the game does not tell us. An upscaler that cannot
   start (not an RTX card, no D3D12) drops UPSCALING to FSR 1 without jitter. creating build/up.now saves the
   upscaler's input and output. Pacing is unchanged (0 frames over 20 ms per 2 s) with either.
   tools/fetch_sdks.py downloads the SDK files at pinned tags, checked by SHA-256 (AMD FSR SDK 2.4.0: ffx API, upscaler
   and frame generation headers in the SDK's own layout, which they include each other by, and the signed DLLs;
   NVIDIA DLSS SDK 310.9.16: NGX headers, nvsdk_ngx_s.lib, nvngx_dlss.dll). They are not committed: NVIDIA's licence
   allows shipping DLSS inside an application but not putting it under an open-source licence, and the DLLs are
   ~150 MB. tools/build.py fetches them when missing, compiles runtime/*.cpp (the vendors' headers are C++) and
   copies the DLLs next to the exe. DLSS is NVIDIA's, and its licence asks that it be credited: the setting says so.
   **Frame rate** (FRAME RATE / framerate: 60, 120, 144, 30; at the next start; hle_forza.c forza_init and
   forza_vblank). Real frames: the Xbox game draws 30 a second (a race's scene snapshots: 30 a second against 60
   presents), and Forza paces itself by three values: the refresh rate (0x554F30, 60; 50 for PAL), vblanks per frame
   (0x554F24, 2 in the image) and from them D3D's presentation interval (0x1D95C0 builds TWO_OR_IMMEDIATE,
   0x80000002, into 0x5DA300, which 0x3B84B0 copies to D3D's own 0x3C8F2C at every screen change; Swap encodes it in
   the flip's software method) and the time each frame steps the game (0x1D9D10: vblanks per frame / refresh, in the
   frame-time mode the game sets, 0 at 0x5188FC; the mode switch also has fixed 1/30 and 1/60, which Turn 10 kept).
   - 30 leaves them alone: the game as it was.
   - Above 30, forza_init sets vblanks per frame to 1 before the game reads it: every vblank may bring a frame
     (ONE_OR_IMMEDIATE) and a race draws 60 new frames a second (FPSLOG's "new": the frame on screen changed).
     Forcing only D3D's interval had made the race 1.73x fast, the step being still 2 vblanks.
   - The step follows the frames: each time a new frame is on screen, the refresh the step divides by becomes the
     vblank rate / the vblanks the last frame took (144 / 3 = 48: 1/48 s; whole numbers for 60, 120 and 144), so the
     game keeps real time at any rate it manages, also on a PC that drops below 60. (Adapting vblanks per frame
     instead locked a 144 race at 48: the game rebuilds its interval from that value.)
   - 120 and 144 run the emulated vblank at that rate (thread.c). Menus then draw 140-144 new frames a second; a race
     with 7 AI cars 67-94 since vertex assembly got faster (it was ~60-80 alone: the emulated GPU works under the
     guest lock).
   - Track flythroughs (reported 2026-11-05: "on 144 Hz the cutscenes in game go really fast", the event list's
     track preview): a Bink video, D:\MEDIA\tracks\<track>\Flythrough.bik (29.97 fps), opened with flags 1, which
     sets its rate to the game's own (refresh / vblanks per frame: 30 on the Xbox, 72 at 144 here) for a frame per
     game frame. Copied ~70 frames a second at 144 (VIDEOLOG). hle_bink.c BinkWrapperOpen now gives such a video its
     file's rate (BIK header +28/+32 into the handle's +0x14/+0x18, which BinkWait paces by) above 30: 28.8 a second
     over 10 s. The step mode (0x5188FC) stayed 0 from the title through a race (logged), so nothing else steps by a
     fixed 1/30 or 1/60 there. 0.2.0 (runtime/version.h) is the version being worked on.
   - Shadows further out (requested 2026-11-05: scenery shadows on the road and other cars' shadows vanish in the
     distance, most on city tracks). Ruled out so far: GeneralRender\Shadow's FadeOutRange / FadeOutAngle
     (0x555744 / 0x555740: a fade by sun angle, cos 80-85 degrees, 0x1E05F0); its Focus/NonFocus Hi/Lo LOD
     (0x555758-64: which car LOD casts, hi within 18 m (0x55574C), cars behind -10 m (0x555748) skipped, no far
     limit, 0x1E12B8); GeneralRender\Misc DistanceBias (0x554EEC, 1.0, copied to the view's 0x5DA984: the object
     test 0x1E33F3 culls at distance x bias > range when 0x5DA981 is set): 0.25 against 1.0 on New York's start
     line changed nothing but animated signs. The same test drops objects under 1/480 of the screen (0x48CD3C, one
     Xbox line; 0x3DF57 shares the constant), ~9 output pixels at 1080i x4, but that is km away for anything metres
     across. Track data: .xb2 bundles and a .pvs (precomputed visibility) per track.
   Measured: the race timer moves 10.000 s in 600 presents at 60 and 11.9 s in ~12 s at 144; the start countdown takes
   5.9 s at 60 and 6.1 s at 30 (UILOG lines now end "at <ms>"). Presents never wait for the display any more
   (DXGI_PRESENT_DO_NOT_WAIT, three buffers): this thread holds the guest lock, and at 144 a present that waited for
   the screen kept it ~99% of the time and the game stopped on the attract video; a frame the display has no room for
   is dropped (FPSLOG "shown" counts the rest).
   FSR 3 frame generation (FRAME RATE 120 before) is gone: it interpolates pictures, which ghosts, and between a
   frame and its repeat (the game drew 30) it had nothing to make. From its D3D11 <-> D3D12 handover two lessons stay
   in up12.cpp: D3D11 Flushes after signalling the shared fence, or the queue waits for its next submission, and the
   two command allocators take turns by a counter, not by the fence's parity (two signals a frame kept it even).
   **Surround** (SPEAKERS / speakers: AUTO, STEREO; at the next start). With AUTO and a device of 6 or more
   channels, the mixer makes 5.1 (FL FR C LFE BL BR): mix bins go to their speakers (the 3D bins 6-9 front and back
   pairs), 3D sources are panned by position front-back as well as left-right (constant power both ways; the LFE
   by distance only), and with enhanced audio the bass of all channels also feeds the LFE (120 Hz low-pass, x0.3).
   A 7.1 device gets the back pair on its sides and, at 0.6, its back. SPEAKERS=6 forces 5.1 for testing (Windows
   folds it to stereo); wav.now records surround folded to stereo.
   timing: `press.ps1 -Keys "18:13,30:13,40:32,46:32,55:13,62:32,68:13,80:13"` reaches it in about half the runs.
   The guest lock is no longer handed over at IRQL >= DISPATCH, and KeSynchronizeExecution runs at the
   interrupt's IRQL (DPCs and raised-IRQL code must not interleave).
   HANGDUMP also prints D3D's flip queue (ISR context +0x174, vblank count +0x1C0) and the push buffer words
   before GET. It reads these without the guest lock, so a "FLIP_STALL with read == write" in the dump is
   usually just a wait for the next vblank at 30 fps, not a deadlock.
   Flip registers start at read 0, write 1, modulo 3. The game pairs Swap(2) (write++, stall) with Swap(4)
   (flip trap).
1. **Graphics: draws.** Translate what the disclaimer screen uses first (per frame: ~1.3 BEGIN_END, INLINE_ARRAY
   vertices, 1 texture, a short vertex program, 1 combiner stage): inline-array draws, NV2A vertex programs ->
   HLSL, texture decode (swizzled/DXT), register combiners -> pixel shaders, then vertex arrays and depth.
2. **Input**: replace XPP's XInitDevices / XGetDevices / XInputOpen / XInputGetState with host input; then drop
   the zeroed-USB shortcut.
3. **Audio**: DSOUND/XACT at the API level; Bink via a host decoder.
