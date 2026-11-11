<p align="center"><img src="launcher/emblem.png" width="128" alt="Redline Recompiled emblem"></p>

<h1 align="center">Redline Recompiled 0.2.0 alpha</h1>

<p align="center">A PC port of <b>Forza Motorsport</b> (Xbox, 2005), made by static recompilation.</p>

> [!WARNING]
> **Alpha.** It plays from start to finish, but performance may be unstable, and expect bugs and crashes.

> [!NOTE]
> **MADE WITH THE HELP OF AI.** This port was made with the help of AI: Anthropic's Claude (in Claude Code) wrote
> and tested much of the code, the tools and these documents, working step by step with the project's author, who
> played and checked the results.

**Contents**

- [Screenshots](#screenshots)
- [What you need](#what-you-need)
- [Install and play](#install-and-play)
- [Controls](#controls)
- [Settings](#settings)
- [Achievements](#achievements)
- [Optional: 4x textures and videos](#optional-4x-textures-and-videos)
- [What does not work](#what-does-not-work)
- [Licence and credits](#licence-and-credits)

![A race at 32:9, the HUD at the screen's edges](docs/images/race-ultrawide.jpg)

The game's own code, translated from Xbox x86 to 64-bit Windows code, runs natively with a new graphics, sound and
input layer. It plays at up to 144 frames a second, at up to 4x resolution, in widescreen and ultrawide, with
keyboard or any controller.

**Not affiliated with Microsoft or Turn 10 Studios.** Nothing of the game comes with this download: the launcher
builds the port from **your own copy** of the disc, on your PC.

## Screenshots

| With the optional 4x texture pack | With the original Xbox textures |
|---|---|
| ![A race at 16:9 with the 4x texture pack](docs/images/race.jpg) | ![The same corner with the original textures](docs/images/race-original.jpg) |
| ![The car showroom](docs/images/showroom.jpg) | ![The grandstand straight with the original textures](docs/images/race-original-2.jpg) |

| The launcher | Its settings |
|---|---|
| ![The launcher](docs/images/launcher.png) | ![The launcher's settings](docs/images/launcher-settings.png) |

## What you need

- **Windows 10 or 11, 64-bit**, and a graphics card with DirectX 11.1 (a dedicated card; integrated graphics run it
  at about 25 frames a second). DLSS needs an NVIDIA RTX card; FSR 3 runs on any DirectX 12 card.
- **Your disc image of Forza Motorsport (USA) (Rev 1)**: an `.iso` file, or a folder with the disc's files
  (`Forza.xbe` and `media`). The launcher checks that it is this version; others are not supported.
- **Nothing to install.** The first build downloads its own tools (Python, the Capstone disassembler and the zig C
  compiler, about 95 MB, from their makers' sites) into the port's `build\tools` folder, and NVIDIA's DLSS and AMD's
  FSR files from their GitHub pages, each file checked against a known checksum. So it needs an internet connection
  the first time.
- **About 7 GB of disk space.**
- **Memory**: the build needs about 1.5 GB of free RAM for each file it compiles at once and runs as many as fit, so
  less memory only makes it slower. The game itself uses about 0.4 GB of RAM and 2 GB of video memory.

## Install and play

1. Download **RedlineRecompiled-0.2.0.zip** from the [Releases](../../releases) page and unzip it to a folder whose
   path is short, at most 120 characters (for example `C:\Games\RedlineRecompiled`). (The green Code button's
   download has no launcher in it: `python tools/build.py --launcher` makes one, with Visual Studio.)
2. Run **RedlineLauncher.exe**.
3. Drop your disc image (or the game folder) on the launcher, or choose it, then press **Build**. The build fetches
   its tools, extracts the disc, finds and translates the game's code, and compiles it; the first one takes about 5
   to 10 minutes, and the launcher shows each step. If it stops, the launcher shows why.
4. Press **Play**.

The game's files go to `game\`, your saves to `hdd\Partition1\UDATA` (the launcher's **Saves** link), and the game's
log to `build\redline.log` (**Logs**). The launcher says when a newer version is out. To update, unzip the new
version over this folder and press **Rebuild**: your saves and settings stay, and the launcher shows what's new.

## Controls

**Controllers**: Xbox, PlayStation, Switch and most others, plugged in at any time. Buttons go by position (the
bottom face button is A); the right shoulder is the Xbox's Black button, the left one White.

**Keyboard**, two sets that change by themselves between a race and the menus:

| Racing | | Menus | |
|---|---|---|---|
| W / Up | accelerate | Arrows or WASD | move |
| S / Down | brake | Enter or Space | select |
| A D / Left Right | steer | Esc | back |
| Space | handbrake | Tab / Backspace | Start / Back buttons |
| E / Q | gear up / down | X, Y | X, Y buttons |
| C | look back | E / Q | Black / White |
| Z / X | look left / right | Page Up / Down | triggers |
| V | camera | | |
| H | HUD | | |
| Esc | pause | | |

Both sets can be changed in the game: **Recomp Settings > Driving keys / Menu keys**.

**The launcher** works with a controller too (or the arrow keys): the d-pad or stick moves between its buttons, A
(Enter) chooses, B (Esc) goes back, and left and right change a setting. Choosing the disc opens Windows' own file
picker, which needs a mouse, touch or the keyboard.

## Settings

In the launcher (**Settings**) or in the game's main menu (**Recomp Settings**); they are the same settings.

- **Render scale** 1x-4x: draws each frame larger, for a sharper picture.
- **Upscaling**: FSR 1, NVIDIA DLSS, AMD FSR 3 or plain bilinear.
- **Frame rate** 30 (as on the Xbox), 60, 120 or 144.
- **Texture filter**, **Borderless fullscreen** (Alt+Enter also switches it), **Widescreen**, **Ultrawide** (a wider
  view on a 21:9 or 32:9 monitor, with the HUD at the screen's edges), **Video mode** (480p, 720p, 1080i).
- **Enhanced audio** and **Speakers** (5.1 and 7.1 get the game's surround mix).
- **Quit Game** is at the bottom of the main menu.

## Achievements

32 achievements that the original game never had: racing, career, garage, modes and tracks. Each one pops up when
earned, and **Achievements** on the main menu lists them all. See [docs/achievements.md](docs/achievements.md).

## Optional: 4x textures and videos

The settings **Texture pack** and **Upscaled videos** use replacement files you make yourself from your disc (they
come from the game, so they cannot be shared). This is for people comfortable with the command line, and it takes
hours and a fast GPU:

- Textures: `python tools/texextract.py` (every texture on the disc into `build\textures\dump`), then
  `python tools/texpack.py` with [Real-ESRGAN ncnn Vulkan](https://github.com/xinntao/Real-ESRGAN) unpacked to
  `build\realesrgan\` (makes `build\textures\redline4x`).
- Videos: `python tools/vidpack.py`, which also needs ffmpeg and an NVIDIA card for its video encoder (makes
  `build\videos`).

## What does not work

Xbox Live (shut down long ago), System Link and anything else online. Split-screen with two controllers has not been
tried. If the game stops with an error, it says so in a box, names the log and offers to report it: a new GitHub
issue opens in your browser, filled in with the version and the end of the log (your user name and folders taken
out). Nothing is sent unless you submit it.

## Licence and credits

Redline Recompiled's own code is under the [MIT licence](LICENSE); the files under `third_party/` keep their own.

It uses [SDL3](https://www.libsdl.org/) for controllers (zlib licence), function signatures from
[XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) (MIT licence code, ODbL data), the kernel export
list from [nxdk](https://github.com/XboxDev/nxdk) (CC0), AMD FidelityFX Super Resolution (MIT licence; the signed
FSR 3 libraries under AMD's licence), and NVIDIA DLSS (NVIDIA RTX SDK licence; DLSS is NVIDIA's). The build downloads
and runs [Python](https://www.python.org/) (PSF licence), [Capstone](https://www.capstone-engine.org/) (BSD licence)
and [zig](https://ziglang.org/) (MIT licence; it includes LLVM's clang and lld and mingw-w64), each with its own
licence files. Forza Motorsport is Microsoft's; this project contains none of it.
