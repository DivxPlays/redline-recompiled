"""Make a texture pack from a dump: every texture the game wrote with TEXTURE DUMP on, upscaled 4x with Real-ESRGAN.

Usage: python tools/texpack.py [--dump build/textures/dump] [--out build/textures/redline4x]
                               [--esrgan build/realesrgan/realesrgan-ncnn-vulkan.exe] [--max 2048] [--gpu N]

Colour goes through the realesrgan-x4plus model; an alpha channel that is not all opaque goes through it too, as a
grey picture, so cut-out edges (fences, leaves, lettering) come out as sharp as the colour. The model smooths fine
grain (grass, asphalt, gravel) away, so one step of back-projection puts back what the upscale lost: the difference
between the original and the upscale shrunk to its size, enlarged and added. Each texture is padded by
wrapping around before upscaling and cropped after, so tiling textures (road, grass) stay seamless. Results larger
than --max on a side are scaled down to it. Opaque textures are written as JPEG (quality 95, no chroma subsampling),
the others as PNG. Textures already in the pack are skipped, so it can be run again after playing more with the dump
on. Tiny (under 16 pixels a side), single-colour and mostly transparent textures are left alone (see wanted()).
dump/nopack.txt (tools/texextract.py) names the fonts, which are left alone (the UI fills boxes from single texels of
its text font). dump/aliases.txt names lower mip levels the game draws as textures of their own; they are not
upscaled but listed in the pack's aliases.txt with the whole texture's key, which the game loads in their place.
Upscaling goes in batches (--batch, 400), so the temporary files stay small.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

PAD = 16  # pixels of wrap-around context on each side


def wanted(im):
    """not a lookup table or fill, nor a sparse sheet: 90% or more transparent, as the font caches the game draws text
    into are; a texture with no alpha at all (alpha 0 everywhere, so unused) is kept"""
    if min(im.size) < 16:
        return False
    a = np.asarray(im)
    clear = (a[..., 3] == 0).mean()
    if (a == a[0, 0]).all():
        return False
    if clear == 1:
        return a[..., :3].mean() > 20
    return clear < 0.9


def back_project(orig, up):
    """up plus the enlarged difference between orig and up shrunk to orig's size (both the same mode)"""
    o, u = np.asarray(orig, np.float32), np.asarray(up, np.float32)
    down = np.asarray(up.resize(orig.size, Image.BOX), np.float32)
    res = np.clip(o - down + 128, 0, 255).astype(np.uint8)
    u = u + np.asarray(Image.fromarray(res).resize(up.size, Image.BICUBIC), np.float32) - 128
    return Image.fromarray(np.clip(u + 0.5, 0, 255).astype(np.uint8))


def esrgan(exe, src, dst, gpu, on_file=None):
    """realesrgan on a folder; on_file() after each picture it finishes (its -v lines end "done")"""
    os.makedirs(dst, exist_ok=True)
    cmd = [exe, "-i", src, "-o", dst, "-n", "realesrgan-x4plus", "-f", "png", "-v"]
    if gpu is not None:
        cmd += ["-g", str(gpu)]
    p = subprocess.Popen(cmd, cwd=os.path.dirname(exe), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors="replace")
    tail = []
    for line in p.stdout:
        if line.rstrip().endswith(" done"):
            if on_file:
                on_file()
        elif not line.startswith("[") and line.strip() not in ("", "0.00%"):
            tail = (tail + [line])[-20:]
    if p.wait():  # its own words first: the launcher shows the last line as the reason
        print("".join(tail)[-2000:])
        sys.exit("Real-ESRGAN stopped: it needs a graphics card that runs Vulkan, with its driver up to date.")


def upscale(args, batch, progress=None):
    """batch: [(key, image)] -> the pack's files; progress(fraction of the batch done) as the pictures come back"""
    tmp = tempfile.mkdtemp(prefix="texpack")
    try:
        rgb_in, alpha_in = os.path.join(tmp, "rgb"), os.path.join(tmp, "alpha")
        os.makedirs(rgb_in), os.makedirs(alpha_in)
        jobs = {}
        for key, im in batch:
            a = np.asarray(im)
            padded = np.pad(a, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")
            Image.fromarray(padded[..., :3]).save(os.path.join(rgb_in, key + ".png"), compress_level=1)
            has_alpha = (a[..., 3] < 255).any()
            if has_alpha:
                Image.fromarray(np.repeat(padded[..., 3:], 3, axis=2)).save(os.path.join(alpha_in, key + ".png"),
                                                                            compress_level=1)
            jobs[key] = (im, has_alpha)
        pictures = len(jobs) + sum(1 for v in jobs.values() if v[1])
        done = [0]

        def one():
            done[0] += 1
            if progress and (done[0] % 10 == 0 or done[0] == pictures):
                progress(done[0] / pictures)
        esrgan(args.esrgan, rgb_in, os.path.join(tmp, "rgb4"), args.gpu, one)
        if any(v[1] for v in jobs.values()):
            esrgan(args.esrgan, alpha_in, os.path.join(tmp, "alpha4"), args.gpu, one)
        for key, (im, has_alpha) in jobs.items():
            w, h = im.size
            box = (PAD * 4, PAD * 4, PAD * 4 + w * 4, PAD * 4 + h * 4)
            rgb = Image.open(os.path.join(tmp, "rgb4", key + ".png")).convert("RGB").crop(box)
            out = back_project(im.convert("RGB"), rgb)
            if has_alpha:
                alpha = Image.open(os.path.join(tmp, "alpha4", key + ".png")).convert("L").crop(box)
                out = Image.merge("RGBA", (*out.split(), back_project(im.getchannel("A"), alpha)))
            if max(out.size) > args.max:
                k = args.max / max(out.size)
                size = (max(1, round(out.width * k)), max(1, round(out.height * k)))
                # channel by channel: Pillow premultiplies RGBA to resize it, which blackens the colour wherever alpha is
                # 0, and some textures use alpha as a blend weight, not coverage (Maple Valley's road speckled black)
                out = Image.merge(out.mode, [c.resize(size, Image.LANCZOS) for c in out.split()])
            # written whole under another name, then renamed: stopped part way (the launcher's Cancel ends the process),
            # a half-written file would count as done the next time
            path = os.path.join(args.out, key + (".png" if has_alpha else ".jpg"))
            if has_alpha:
                out.save(path + ".tmp", format="PNG")
            else:
                out.save(path + ".tmp", format="JPEG", quality=95, subsampling=0)
            os.replace(path + ".tmp", path)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default=os.path.join(root, "build", "textures", "dump"))
    ap.add_argument("--out", default=os.path.join(root, "build", "textures", "redline4x"))
    ap.add_argument("--esrgan", default=os.path.join(root, "build", "realesrgan", "realesrgan-ncnn-vulkan.exe"))
    ap.add_argument("--max", type=int, default=2048)
    ap.add_argument("--gpu", type=int)
    ap.add_argument("--batch", type=int, default=400)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):  # left by a run that was stopped while writing
        if f.endswith(".tmp"):
            os.remove(os.path.join(args.out, f))
    alias_file = os.path.join(args.dump, "aliases.txt")
    aliases = dict(line.split() for line in open(alias_file)) if os.path.exists(alias_file) else {}
    nopack_file = os.path.join(args.dump, "nopack.txt")  # fonts (tools/texextract.py)
    nopack = {line.strip() for line in open(nopack_file)} if os.path.exists(nopack_file) else set()
    if not nopack:
        print("no dump/nopack.txt: run tools/texextract.py first, or the fonts get upscaled and the UI's boxes blur")
    done = {os.path.splitext(f)[0].upper() for f in os.listdir(args.out) if f[:16] != "aliases."}
    todo = sorted(f for f in os.listdir(args.dump) if f.lower().endswith(".dds") and f[:16].upper() not in done
                  and f[:16].upper() not in aliases and f[:16].upper() not in nopack)  # a lower mip level of a whole texture gets that one's picture
    print(f"{len(todo)} new textures in {args.dump}", flush=True)
    batch, n, start = [], 0, 0  # start: the first of todo the batch covers

    def progress(at):  # "progress: <done>/<total>" lines are what the launcher's bar reads
        print(f"progress: {at}/{len(todo)} textures", flush=True)
    for i, f in enumerate(todo):
        try:
            im = Image.open(os.path.join(args.dump, f)).convert("RGBA")
        except Exception as e:  # a format PIL cannot read
            print(f"  skip {f}: {e}")
            im = None
        if im is not None and wanted(im):
            batch.append((f[:16].upper(), im))
        if len(batch) == args.batch or (i == len(todo) - 1 and batch):
            upscale(args, batch, lambda k, a=start, b=i + 1: progress(a + int((b - a) * k)))
            n += len(batch)
            print(f"  {i + 1}/{len(todo)} looked at, {n} upscaled", flush=True)
            batch, start = [], i + 1
    progress(len(todo))
    have = {os.path.splitext(f)[0].upper() for f in os.listdir(args.out)}
    with open(os.path.join(args.out, "aliases.txt"), "w") as f:
        f.writelines(f"{k} {v}\n" for k, v in sorted(aliases.items()) if v in have)
    print(f"wrote {n} textures to {args.out}; {sum(1 for v in aliases.values() if v in have)} aliases")


if __name__ == "__main__":
    main()
