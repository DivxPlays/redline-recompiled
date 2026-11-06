"""Incremental build: recomp/*.c -> build/forza_recomp.lib, runtime/*.c + lib -> build/redline.exe.

A source is recompiled when its .obj is missing or older than the source or a header it depends on
(lifted code: runtime/x86.h and recomp/funcs.h; runtime code: runtime/*.h and funcs.h). Stale files compile in
one parallel cl /MP run.

Usage: python tools/build.py              the game (build/redline.exe)
       python tools/build.py --launcher   only the launcher (RedlineLauncher.exe at the root; needs no game files)
"""
import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VSWHERE = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
# 4101/4102 unused locals/labels, 4244/4146 narrowing and unsigned negation are inherent to lifted code
CFLAGS = ["/nologo", "/c", "/MP", "/O2", "/W3", "/wd4101", "/wd4102", "/wd4244", "/wd4146", "/wd4100",
          "/D_CRT_SECURE_NO_WARNINGS", "/Iruntime", "/Ithird_party/SDL3/include", "/Ithird_party/dlss/include",
          "/Ithird_party/fsr/api/include",  # the FSR SDK's own layout: its headers include each other by it
          "/Ithird_party/fsr/upscalers/include"]
# the upscalers' vendor files (tools/fetch_sdks.py downloads them: not in the repository) and the DLLs the game needs
SDK_DLLS = ["third_party/dlss/bin/nvngx_dlss.dll", "third_party/fsr/bin/amd_fidelityfx_loader_dx12.dll",
            "third_party/fsr/bin/amd_fidelityfx_upscaler_dx12.dll"]


def vcvars():
    """vcvars64.bat of the newest Visual Studio with the C++ tools, as vswhere finds it."""
    if os.path.exists(VSWHERE):
        out = subprocess.run([VSWHERE, "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find",
                              r"VC\Auxiliary\Build\vcvars64.bat"], capture_output=True, text=True).stdout.split("\n")
        if out[0].strip():
            return out[0].strip()
    sys.exit("Visual Studio's C++ build tools were not found (https://visualstudio.microsoft.com/visual-cpp-build-tools/)")


def msvc_env():
    """Environment after vcvars64.bat, cached in build/vcvars.json."""
    cache = os.path.join(ROOT, "build", "vcvars.json")
    if os.path.exists(cache):
        return json.load(open(cache))
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    out = subprocess.run(f'cmd /c ""{vcvars()}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    if "INCLUDE" not in env:
        sys.exit("vcvars64.bat did not set up MSVC")
    json.dump(env, open(cache, "w"))
    return env


def stale(sources, obj_dir, deps):
    newest_dep = max((os.path.getmtime(d) for d in deps), default=0)
    out = []
    for src in sources:
        obj = os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ".obj")
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), newest_dep):
            out.append(src)
    return out


def run(cmd, env):
    # resolve against MSVC's PATH: CreateProcess searches ours, where Git's coreutils `link` may come first
    exe = shutil.which(cmd[0], path=env.get("PATH") or env.get("Path"))
    if not exe:
        sys.exit(f"{cmd[0]} not found on the MSVC PATH")
    r = subprocess.run([exe, *cmd[1:]], cwd=ROOT, env=env, capture_output=True, text=True)
    lines = [l for l in (r.stdout + r.stderr).splitlines()
             if l.strip() and not l.strip().endswith(".c")]  # cl echoes every file name
    if lines:
        print("\n".join(lines))
    if r.returncode:
        sys.exit(f"{cmd[0]} failed")


def icon(env):
    """build/redline.res: the emblem (launcher/redline.ico, from tools/make_icon.py) for both programs' icons."""
    os.makedirs("build", exist_ok=True)
    run(["rc", "/nologo", "/fo", "build\\redline.res", "launcher\\redline.rc"], env)
    return "build\\redline.res"


def launcher(env):
    os.makedirs("build/launcher", exist_ok=True)
    run(["cl", "/nologo", "/O2", "/W3", "/D_CRT_SECURE_NO_WARNINGS", "/Fobuild\\launcher\\", "launcher/launcher.cpp",
         "/link", "/SUBSYSTEM:WINDOWS", "/out:RedlineLauncher.exe", icon(env)], env)
    print("built RedlineLauncher.exe")


def main():
    os.chdir(ROOT)
    env = msvc_env()
    if "--launcher" in sys.argv:
        launcher(env)
        return
    for d in ("build/recomp", "build/runtime"):
        os.makedirs(d, exist_ok=True)
    headers = glob.glob("runtime/*.h")

    recomp = sorted(glob.glob("recomp/*.c"))
    todo = stale(recomp, "build/recomp", ["runtime/x86.h", "recomp/funcs.h"])  # all lifted code includes
    lib = "build/forza_recomp.lib"
    if todo:
        print(f"compiling {len(todo)} of {len(recomp)} recomp files")
        # /Zi: line info maps a host crash address back to the lifted line and its guest-address comment
        run(["cl", *CFLAGS, "/Zi", "/FS", "/Fobuild\\recomp\\", "/Fdbuild\\recomp\\", *todo], env)
    objs = glob.glob("build/recomp/*.obj")
    if todo or not os.path.exists(lib):
        run(["lib", "/nologo", f"/out:{lib}", *objs], env)

    if not all(os.path.exists(f) for f in SDK_DLLS + ["third_party/dlss/lib/nvsdk_ngx_s.lib"]):
        subprocess.run([sys.executable, "tools/fetch_sdks.py"], check=True)
    runtime = sorted(glob.glob("runtime/*.c"))
    todo = stale(runtime, "build/runtime", headers + ["recomp/funcs.h"])
    if todo:
        run(["cl", *CFLAGS, "/Zi", "/Fobuild\\runtime\\", "/Fdbuild\\runtime\\", *todo], env)
    cpp = sorted(glob.glob("runtime/*.cpp"))  # the parts the vendors' C++ headers need (dlss.cpp, fsr3.cpp)
    todo = stale(cpp, "build/runtime", headers)
    if todo:
        run(["cl", *CFLAGS, "/EHsc", "/Zi", "/Fobuild\\runtime\\", "/Fdbuild\\runtime\\", *todo], env)
    # a Windows program: no console window beside the game (its output goes to the launcher's log)
    run(["link", "/nologo", "/DEBUG", "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup", "/out:build\\redline.exe",
         *glob.glob("build/runtime/*.obj"), lib, "third_party/SDL3/lib/SDL3.lib", "third_party/dlss/lib/nvsdk_ngx_s.lib",
         icon(env)],
        env)
    shutil.copy2("third_party/SDL3/lib/SDL3.dll", "build/SDL3.dll")
    for dll in SDK_DLLS:
        dest = os.path.join("build", os.path.basename(dll))
        if not os.path.exists(dest) or os.path.getmtime(dest) < os.path.getmtime(dll):
            shutil.copy2(dll, dest)
    print("built build\\redline.exe")


if __name__ == "__main__":
    main()
