# Changelog

<!-- /kit:release adds one section per version, newest first:

## 1.2.0 Beta (2026-11-10)

### New
- ...

### Fixed
- ...
-->

## 0.3.0 Alpha (2026-11-12)

### New
- A Language setting, in the launcher's Settings and the game's Recomp Settings: English, German, French, Spanish,
  Italian, Japanese, Korean or Chinese, the eight the game came with. It used to always start in English. Until you
  choose one, it is Windows' display language when the game has it (else English). It takes effect the next time the
  game starts. (#19)
- Shift on Shoulders, in the launcher's Settings and the game's Recomp Settings: on a controller in a race, RB
  shifts up and LB down (in any controller layout), and the camera and HUD move to the right and left stick clicks.
  (#3)
- Achievement Sound, in the launcher's Settings and the game's Recomp Settings: turns the fanfare of the achievement
  pop-up off or on.
- Controller Buttons, in the game's Recomp Settings: choose a controller's button for each racing action
  (accelerate, brake, e-brake, gear up and down, look back, left and right, camera, HUD). Your buttons hold in every
  layout of the game's Controller Options, and the menus keep the usual ones. Until you change one, the controller
  works as before; Layout: The Game's goes back to that. (#3)
- Make 4x textures, on the launcher's Ready page: every texture on your disc made four times as detailed by
  Real-ESRGAN on your graphics card, with no command line. It takes about an hour and a half on a fast graphics
  card and longer on others, needs about 12 GB and a graphics card with Vulkan, and the first time downloads its
  tools (Real-ESRGAN and two Python packages, about 67 MB). Cancel stops it; the next time it picks up where it
  stopped.
- Rumble Strength, in the launcher's Settings and the game's Recomp Settings: 100%, 75%, 50% or 25% of the game's
  rumble, for controllers that rumble harder than the Xbox's did. The game's Controller Options still turn it off.
- Mods: a folder in the new mods folder (beside the game folder) whose files take the place of the game's own, at
  the same paths as in the game folder (media\cars\...). Each one can be turned off in Recomp Settings > Mods; when
  two mods have the same file, the one first by name wins. Changes take effect the next time the game starts. Mods
  in the launcher's footer opens the folder.

### Changed
- With Ultrawide, the menus' sides are no longer black: each menu's own picture carries on into them, blurred and
  darker towards the screen's edges, while the menu itself stays 16:9. Menu Sides (Settings and Recomp Settings)
  keeps them black. Screens with 3D across the screen, such as the showroom and the starting grid, already filled it.
- Frame rate has an Auto setting, now the default: the refresh rate of the screen the game opens on (halved above
  165 Hz), so each frame stays on screen for one refresh. At 60 frames a second, a 120, 144 or 165 Hz screen without
  G-Sync or FreeSync showed frames for 2 and 3 refreshes in turn, which looks like a stutter however smoothly the game
  runs. A frame rate you chose before stays as it was.
- The launcher's Settings has two tabs, Picture and Game & sound (LB / RB or Tab switch them), and every setting of
  the game's Recomp Settings: Button prompts and Texture dump were in the game's menu only.
- Achievements now pop up the way the Xbox 360 showed them, in Redline's colours: at the bottom of the screen a red
  ring with the emblem pops up, a bar opens out of it with "Achievement unlocked" and the name, and it closes back
  after 4 seconds. Each one plays the opening of the game's own fanfare, read from your disc.

### Fixed
- Night tracks such as Tokyo: half the road and its barriers no longer glow white with a hard straight edge, and the
  shadow under the car no longer shows as a dark box against them. At night only the headlights light up the road
  now. (#24, #28)
- A car painted in The Garage's Customize Paint keeps everything printed on it: its lights, badges, grille,
  lettering and interior used to go blank, the grille and other parts taking the paint's colour or grey, and the car
  looked flatter. A car painted with an earlier version gets them back when you paint it again. (#28)
- A picture in a race (such as the car status box's drivetrain diagram) no longer shows an old picture instead,
  like a car's picture from the garage. (#28)
- With the frame rate at the screen's refresh rate (as Auto sets it), frames now keep step with the screen. The game
  kept time by a clock of its own, which drifts against the screen's, and whenever the two lined up badly (for
  about 45 seconds every few minutes on a 144 Hz screen) a frame was shown twice and the next skipped many times a
  second: a stutter that came and went however smoothly the game ran.
- On PCs with few CPU cores, loading the texture pack and preparing graphics in the background no longer competes
  with the game itself, which caused some of the stutters in races as new scenery came in.
- The keyboard's racing keys now do what their names say in every controller layout of the game's Controller
  Options. With a layout other than the default, they used to do another layout's job: with "Shift on A/B Buttons",
  E (gear up) shifted down and Space (handbrake) shifted up. Controllers were already right.
- The launcher says at once whether a disc image or folder is the right one, as soon as you choose it ("the right
  disc", or what it is instead: another release of Forza Motorsport with its version, another game by name, a damaged
  or modified copy, or no Xbox disc at all). Build with a wrong one stops within seconds with the same words; it used
  to download the build tools and unpack all 3.6 GB first, then report only a checksum. (#1)
- After an update, the launcher builds the game again ("Update your build") before Play. It used to go on playing
  the game built by the earlier version, so the game's own fixes and features never arrived and its corner showed
  the old version number. (#25)
- A car you own could get a blank picture (a grey box with a black band) in the bar under the cars: the game draws
  each owned car's picture and saves it, and that drawing sometimes failed. A picture already saved blank comes back
  when you repaint that car once (The Garage > Customize Paint). (#26)

## 0.2.3 Alpha (2026-11-11)

### Fixed
- Build no longer stops at "not an XDVDFS image" with a full-disc dump of the game (the kind Redump makes, with the
  disc's video partition first): the launcher finds the game in it, as in the smaller XISO kind. (#23)

## 0.2.2 Alpha (2026-11-11)

### Fixed
- Build no longer stops with "The download failed (error 800C0008)" on PCs where the browser downloads fine: when
  Windows' download fails, the launcher tries again with curl (built into Windows). If that fails too, it names the
  file to download in your browser, and Build takes it from your Downloads folder. The update check uses curl the
  same way, so those PCs are told about new versions too. (#22)
- While the launcher downloads a build tool that way, the build card shows how far it has got ("5 of 82 MB").

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
