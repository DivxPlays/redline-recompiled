"""The release zip, dist/RedlineRecompiled-<version>.zip: the committed tree (git archive HEAD) and the launcher, built
fresh, in a RedlineRecompiled-<version> folder. Nothing of the game, the generated code or the build is in it (all
git-ignored); players build those from their own disc with the launcher.

Usage: python tools/package.py

Stops if the tree has uncommitted changes (the zip would not have them), and fails if any file in the zip, unpacked,
holds the last commit's author address or this PC's home folder path (taken at run time, so they are written nowhere).
"""
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def main():
    if git("status", "--porcelain", "--untracked-files=no"):
        sys.exit("commit first: the zip is made from the last commit")
    version = re.search(r'RECOMP_VERSION "([^"]+)"', open(os.path.join(ROOT, "runtime", "version.h")).read()).group(1)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "build.py"), "--launcher"], cwd=ROOT, check=True)
    name = f"RedlineRecompiled-{version}"
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", name + ".zip")
    git("archive", "--format=zip", f"--prefix={name}/", "--add-file=RedlineLauncher.exe", "-o", out, "HEAD")

    private = [s for s in (git("log", "-1", "--format=%ae"), os.path.expanduser("~")) if s]
    with zipfile.ZipFile(out) as z:
        hits = [(i.filename, p) for i in z.infolist() for p in private
                if p.lower().encode() in z.read(i).lower() or p.lower().encode("utf-16-le") in z.read(i).lower()]
        hits += [("zip comment", p) for p in private if p.lower().encode() in z.comment.lower()]
        n = len(z.infolist())
    if hits:
        os.remove(out)
        sys.exit("removed the zip: private text in " + ", ".join(sorted({f for f, _ in hits})))
    print(f"{out}: {n} files, {os.path.getsize(out) / 1e6:.1f} MB, no author address or home folder path in it")


if __name__ == "__main__":
    main()
