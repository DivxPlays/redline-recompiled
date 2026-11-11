"""Record the game for its trailer: you play, this records the game's window, picture only (the trailer puts an
original beat under it, never the game's or your mic's sound).

Usage: python tools/record.py NAME [--seconds N]
Starts build\\redline.exe with game\\ (as the launcher's Play does) and records its window to build\\rec\\NAME.mkv
(1920x1080, the middle of a wider window) in the game's own colours, without the mouse pointer, until N seconds have
passed (default 600), you close the game, or build\\rec\\stop appears. The game's log goes to build\\rec\\NAME.log, ffmpeg's to build\\rec\\NAME.ffmpeg.log.
Needs the ffmpeg tools/vidpack.py uses (build\\ffmpeg\\*\\bin) and an NVIDIA card (NVENC).
The trailer itself is made by the Repo Trailers tool (its starter script), from the shots picked out of these files.
"""
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REC = os.path.join(ROOT, "build", "rec")
EXE = os.path.join(ROOT, "build", "redline.exe")


def capture(ffmpeg, out, log):
    """ffmpeg recording the game's window (Windows.Graphics.Capture), filled to 1920x1080 with its middle kept (the
    trailer is 16:9, and NVENC refuses odd sizes and anything over 4096 wide, like an ultrawide window), BT.709. The
    window only sends frames when it changes: variable frame rate, which the trailer's editor reads by timestamp."""
    return subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                             r"gfxcapture=window_exe=(?i)^redline\.exe$:capture_cursor=0:max_framerate=60",
                             "-vf", "hwdownload,format=bgra,scale=1920:1080:force_original_aspect_ratio=increase:"
                             "out_color_matrix=bt709:out_range=tv,crop=1920:1080,format=yuv420p",
                             "-fps_mode", "vfr", "-colorspace", "bt709", "-color_primaries", "bt709",
                             "-color_trc", "bt709", "-color_range", "tv",
                             "-c:v", "h264_nvenc", "-preset", "p6", "-rc", "vbr", "-cq", "16", "-b:v", "0", out],
                            stdin=subprocess.PIPE, stderr=log)


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        sys.exit(__doc__)
    name, seconds = args[0], float(args[args.index("--seconds") + 1]) if "--seconds" in args else 600.0
    found = glob.glob(os.path.join(ROOT, "build", "ffmpeg", "*", "bin", "ffmpeg.exe"))
    if not found:
        sys.exit("No ffmpeg in build\\ffmpeg: python tools/vidpack.py downloads it.")
    os.makedirs(REC, exist_ok=True)
    out, stop = os.path.join(REC, name + ".mkv"), os.path.join(REC, "stop")
    if os.path.exists(stop):
        os.remove(stop)
    with open(os.path.join(REC, name + ".log"), "w") as game_log, open(os.path.join(REC, name + ".ffmpeg.log"), "w") as log:
        game = subprocess.Popen([EXE, os.path.join(ROOT, "game")], cwd=os.path.dirname(EXE), stdout=game_log,
                                stderr=game_log)
        for _ in range(30):  # until its window is up: ffmpeg gives up at once while there is none
            ffmpeg = capture(found[0], out, log)
            try:
                ffmpeg.wait(timeout=2)
            except subprocess.TimeoutExpired:
                break
            if game.poll() is not None:
                sys.exit(f"The game closed before its window came up: see build\\rec\\{name}.log")
        else:
            game.kill()
            sys.exit(f"The game's window never came up for recording: see build\\rec\\{name}.ffmpeg.log")
        print(f"recording {name} (stop: close the game, or create {stop})", flush=True)
        t0 = time.time()
        while time.time() - t0 < seconds and not os.path.exists(stop) and game.poll() is None:
            time.sleep(0.2)
        ffmpeg.communicate(b"q", timeout=30)
        if game.poll() is None:
            game.kill()
    print(f"recorded {name}: {time.time() - t0:.0f} s -> {out}", flush=True)


if __name__ == "__main__":
    main()
