# Changelog

<!-- /kit:release adds one section per version, newest first:

## 1.2.0 Beta (2026-11-10)

### New
- ...

### Fixed
- ...
-->

## 0.2.1 Alpha (2026-11-10)

### Fixed
- No more freeze of about 2 seconds the first time a 3D scene shows with DLSS or FSR 3 on (at car select, or a race's
  countdown): the upscaler now starts while the game boots.
- Far less stutter in races the first time new cars, tracks or effects appear: the graphics they need are now
  prepared in the background, several at once, instead of the race waiting for them (the first race's loading
  screen froze for about 3 seconds, its start for 1). (#3)
- With Ultrawide on a 21:9 or 32:9 screen, the rear-view mirror was stretched sideways, its cars twice as wide; it
  keeps its shape now.
- The loading screen before a race shows the track's picture, map and conditions. It showed a magnified, cut-off
  copy of the car select screen with a black box where the car had been. (#21)

## 0.2.0 Alpha (2026-11-10)

### New
- 32 achievements the original game never had (racing, career, garage, modes and tracks): each one pops up when
  earned, and Achievements on the main menu lists them all, earned or not.
- The launcher says when a newer version is out (it asks GitHub at most once a day, and says nothing when
  offline); a click shows its notes and offers its download page. It never downloads or installs it itself. (#11)
- The first start after an update shows what changed in that version, once. (#12)
- The launcher works with a gamepad alone (or the arrow keys): the d-pad or stick moves a ring over the buttons,
  A (Enter) chooses, B (Esc) goes back, and left and right change a setting. Choosing the disc image still opens
  Windows' own file picker, which needs a mouse, touch or the keyboard. (#13)
- When the game stops on an error, its message offers to report it: a new GitHub issue opens in the browser, filled
  in with the version, the Windows version, the video settings and the end of the log, with your user name, PC
  name and folders taken out. Nothing is sent unless you submit it. (#14)

### Changed
- Nothing to install: the launcher no longer needs Python or Visual Studio. The first build downloads what it needs
  (Python, capstone and the zig compiler, about 95 MB, each checked against a fixed checksum) into the port's
  build\tools folder, and builds with them. (#1, #8, #15)
- The launcher's footer says the port is made with the help of AI. (#10)
- The zip has no folder inside any more: unzip it into the port's folder, and a new version unzipped into the same
  folder updates it, keeping your saves and settings. (0.1.0's zip had a RedlineRecompiled-0.1.0 folder inside: to
  update from it, unzip this version into that folder.)

### Fixed
- The build no longer runs out of memory ("C1002: compiler is out of heap space") on PCs with 16 GB of RAM or
  less. One of the generated files needed 65 GB to compile; now the whole compile needs about 3 GB.
  (#2, #3, #8, #16)
- The build no longer stops with "No such file or directory: analysis\Forza.functions.json" after you choose the
  disc image with the launcher's file picker. (#5, #9)
- "cl not found on the MSVC PATH": the build notices when Visual Studio was installed, updated or repaired after a
  failed try. If Visual Studio's setup still fails, the log now shows its own message. (#18)
- In the launcher's Settings, the ‹ arrow now steps a setting back; it used to step it forward. (#6)
- At 720p the game no longer crashes to the desktop ("guest heap exhausted") while loading a race after one or
  more races. (#17)
- The bumper and hood camera views in a race showed only the rear-view mirror on a black screen; they now show the
  road.
- Cancel in the launcher stops the build at once; it used to let the step it was on (extracting the disc,
  translating, compiling) run to its end.
- Achievements are kept between starts (they were earned again, with their pop-ups, each time a profile loaded).
- The track previews in the event list play at their own speed when the frame rate is above 30 (they ran up to
  almost 5 times too fast).
