"""Upscaled cutscenes: every Bink video on the disc, upscaled 4x with Real-ESRGAN, as H.264 the game plays in its place.

Usage: python tools/vidpack.py [--game game] [--out build/videos] [--only name,...]
       [--ffmpeg build/ffmpeg/.../ffmpeg.exe] [--esrgan build/realesrgan/realesrgan-ncnn-vulkan.exe]

Each video (media\\<path>.bik, the PAL copies left out) is decoded with ffmpeg's Bink decoder, upscaled in chunks with
realesrgan-x4plus plus the one step of back-projection tools/texpack.py does (the model smooths grass and asphalt
away), scaled to 1920x1440 (the videos are 4:3; both sizes divisible by 16, as Media Foundation's decoder wants) and
encoded with NVENC to <out>\\<path>.mp4. The game keeps playing the Bink file for timing and sound; hle_bink.c shows
the frame of the .mp4 with the same number in place of each frame Bink copies out. The fullscreen ones (intro,
attract) go first. Videos already in <out> are skipped.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texpack import back_project  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 1920, 1440
CHUNK = 300  # frames upscaled at a time


def finish(job):
    """one frame: back-project the upscale against the original, scale to W x H, save for the encoder"""
    orig, up, out = job
    o = Image.open(orig).convert("RGB")
    u = Image.open(up).convert("RGB")
    back_project(o, u).resize((W, H), Image.LANCZOS).save(out, quality=95, subsampling=0)
    os.remove(up)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"{os.path.basename(cmd[0])} failed:\n{r.stderr[-2000:]}")
    return r


def video(args, bik, out, pool):
    name = os.path.relpath(bik, os.path.join(args.game, "media"))
    tmp = os.path.join(ROOT, "build", "vidtmp")
    shutil.rmtree(tmp, ignore_errors=True)
    src, up, done = (os.path.join(tmp, d) for d in ("src", "up", "done"))
    for d in (src, up, done):
        os.makedirs(d)
    probe = run([args.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=r_frame_rate",
                 "-of", "csv=p=0", bik]).stdout.strip()
    run([args.ffmpeg, "-v", "error", "-i", bik, "-fps_mode", "passthrough", os.path.join(src, "%05d.png")])
    frames = sorted(os.listdir(src))
    print(f"{name}: {len(frames)} frames at {probe} fps", flush=True)
    for i in range(0, len(frames), CHUNK):
        part = os.path.join(tmp, "part")
        shutil.rmtree(part, ignore_errors=True)
        os.makedirs(part)
        for f in frames[i:i + CHUNK]:
            os.link(os.path.join(src, f), os.path.join(part, f))
        # saving the 4x PNGs is the slow part at this size: eight threads for it
        cmd = [args.esrgan, "-i", part, "-o", up, "-n", "realesrgan-x4plus", "-f", "png", "-j", "2:2:8"]
        run(cmd, cwd=os.path.dirname(args.esrgan))
        list(pool.map(finish, [(os.path.join(src, f), os.path.join(up, f), os.path.join(done, f[:-4] + ".jpg"))
                               for f in frames[i:i + CHUNK]], chunksize=8))
        print(f"  {min(i + CHUNK, len(frames))}/{len(frames)}", flush=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    # TV-range BT.709: Media Foundation's decoder takes H.264 as TV range whatever the stream says, so the full-range
    # output ffmpeg makes from JPEGs by default came out too contrasty
    run([args.ffmpeg, "-v", "error", "-y", "-framerate", probe, "-i", os.path.join(done, "%05d.jpg"),
         "-vf", "scale=in_range=pc:out_range=tv:out_color_matrix=bt709", "-c:v", "h264_nvenc", "-preset", "p7",
         "-tune", "hq", "-rc", "vbr", "-cq", "19", "-b:v", "0", "-pix_fmt", "yuv420p", "-color_range", "tv",
         "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-g", "30", "-movflags", "+faststart",
         out])
    shutil.rmtree(tmp, ignore_errors=True)


def main():
    ff = glob.glob(os.path.join(ROOT, "build", "ffmpeg", "*", "bin"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.path.join(ROOT, "game"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "videos"))
    ap.add_argument("--only", default="")
    ap.add_argument("--ffmpeg", default=os.path.join(ff[0], "ffmpeg.exe") if ff else "ffmpeg")
    ap.add_argument("--esrgan", default=os.path.join(ROOT, "build", "realesrgan", "realesrgan-ncnn-vulkan.exe"))
    args = ap.parse_args()
    args.ffprobe = os.path.join(os.path.dirname(args.ffmpeg), "ffprobe.exe") if ff else "ffprobe"
    biks = [p for p in glob.glob(os.path.join(args.game, "media", "**", "*.bik"), recursive=True)
            if not p.lower().endswith("_pal.bik")]
    first = ("introvideo", "attractvideo")
    biks.sort(key=lambda p: (not any(f in p.lower() for f in first), p.lower()))
    only = [o.lower() for o in args.only.split(",") if o]
    with ProcessPoolExecutor(max(1, (os.cpu_count() or 4) - 2)) as pool:
        for bik in biks:
            rel = os.path.relpath(bik, os.path.join(args.game, "media"))
            out = os.path.join(args.out, os.path.splitext(rel)[0] + ".mp4")
            if os.path.exists(out) or (only and not any(o in rel.lower() for o in only)):
                continue
            video(args, bik, out, pool)
    print("done")


if __name__ == "__main__":
    main()
