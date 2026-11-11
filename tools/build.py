"""Incremental build: runtime/*.c -> build/redline.exe (no game code), recomp/*.c -> build/forza_recomp.dll (the
lifted game, which the exe loads; x86.h).

A source is recompiled when its .obj is missing or older than the source or a header it depends on
(lifted code: runtime/x86.h and recomp/funcs.h; runtime code: runtime/*.h). Stale files compile in one parallel
cl /MP run.

With --zig it builds with zig (clang, lld, mingw-w64 and Windows' own C runtime: nothing to install), from
build/tools/ (the launcher fetches it); players' builds use it, Visual Studio is for development.

Usage: python tools/build.py              the game (build/redline.exe and build/forza_recomp.dll)
       python tools/build.py --runtime    only build/redline.exe and its exports (needs no game files)
       python tools/build.py --launcher   only the launcher (RedlineLauncher.exe at the root; needs no game files)
       python tools/build.py --zig        the game, with zig instead of Visual Studio
"""
import concurrent.futures
import ctypes
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

ZIG = os.path.join(ROOT, "build", "tools", "zig-0.14.1", "zig-x86_64-windows-0.14.1", "zig.exe")
ZIG_FLAGS = ["-target", "x86_64-windows-gnu", "-O2", "-g", "-gcodeview", "-D_CRT_SECURE_NO_WARNINGS", "-Iruntime",
             "-Ithird_party/SDL3/include", "-Ithird_party/dlss/include", "-Ithird_party/fsr/api/include",
             "-Ithird_party/fsr/upscalers/include", "-Wno-everything"]
# what the runtime's #pragma comment(lib) names, which zig's linker does not read (onecore and synchronization by the
# API sets that hold VirtualAlloc2, MapViewOfFile3 and WaitOnAddress)
ZIG_LIBS = ["-ld3d11", "-ld3d12", "-ld3dcompiler_47", "-ldbghelp", "-ldxgi", "-ldxguid", "-lgdi32", "-lmfplat",
            "-lmfreadwrite", "-lmfuuid", "-lole32", "-lshell32", "-lshlwapi", "-luser32", "-lwindowscodecs", "-lwinmm",
            "-lapi-ms-win-core-memory-l1-1-6", "-lapi-ms-win-core-synch-l1-2-0"]
# NGX's static library (DLSS) asks for Visual Studio's runtime libraries; what it uses is in runtime/msvc_shim.c
NGX_DEFAULTLIBS = ["libLIBCMT.a", "liblibcpmt.a", "libOLDNAMES.a", "libuuid.a"]


def vcvars():
    """vcvars64.bat of the newest Visual Studio with the C++ tools, as vswhere finds it."""
    if os.path.exists(VSWHERE):
        out = subprocess.run([VSWHERE, "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find",
                              r"VC\Auxiliary\Build\vcvars64.bat"], capture_output=True, text=True).stdout.split("\n")
        if out[0].strip():
            return out[0].strip()
    sys.exit("Visual Studio's C++ build tools were not found (https://visualstudio.microsoft.com/visual-cpp-build-tools/)")


def has_cl(env):
    return shutil.which("cl", path=env.get("PATH") or env.get("Path")) is not None


def msvc_env():
    """Environment after vcvars64.bat, cached in build/vcvars.json while it is the same vcvars64.bat and cl.exe is still
    there (a Visual Studio installed, updated or removed after a failed try made the old one wrong: GitHub #18)."""
    bat = vcvars()
    cache = os.path.join(ROOT, "build", "vcvars.json")
    if os.path.exists(cache):
        saved = json.load(open(cache))
        if saved.get("vcvars") == bat and has_cl(saved.get("env", {})):
            return saved["env"]
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    mark = "--- environment ---"  # /d: no cmd AutoRun scripts; what vcvars64 prints is kept to say why it failed
    out = subprocess.run(f'cmd /d /c ""{bat}" & echo {mark}& set"', capture_output=True, text=True, shell=True)
    said, _, sets = out.stdout.partition(mark)
    env = dict(line.split("=", 1) for line in sets.splitlines() if "=" in line)
    if "INCLUDE" not in env or not has_cl(env):
        sys.exit(f"{bat} did not set up the C++ compiler (cl.exe). It said:\n{said.strip()}\n{out.stderr.strip()}\n"
                 "Repair or reinstall \"Desktop development with C++\" in the Visual Studio Installer.")
    json.dump({"vcvars": bat, "env": env}, open(cache, "w"))
    return env


def stale(sources, obj_dir, deps, ext=".obj"):
    newest_dep = max((os.path.getmtime(d) for d in deps), default=0)
    out = []
    for src in sources:
        obj = os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ext)
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


def exports():
    """build/redline.def: what the game DLL imports from the runtime (x86.h's RT_API): its helpers, the guest lock's
    waiter count, and every hle_ function the lifter calls in place of a game function."""
    names = ["x86_call_indirect", "x86_unimpl", "trace_enter", "x86_fault", "x86_ld80", "x86_st80", "x86_rdtsc",
             "x86_out", "x86_in", "x86_cpuid", "mmio_read", "mmio_write", "gil_yield", "g_gil_waiters DATA"]
    for line in open("runtime/hle.txt"):  # the lifter's list: `Name` or `0xVA Name`, # comments
        if line.split("#")[0].split():
            names.append("hle_" + line.split("#")[0].split()[-1])
    text = "EXPORTS\n" + "".join(f"    {n}\n" for n in names)
    path = "build/redline.def"
    if not os.path.exists(path) or open(path).read() != text:
        open(path, "w").write(text)
    return path


def sdks():
    if not all(os.path.exists(f) for f in SDK_DLLS + ["third_party/dlss/lib/nvsdk_ngx_s.lib"]):
        subprocess.run([sys.executable, "tools/fetch_sdks.py"], check=True)


def copy_dlls():
    """the DLLs the game runs with, beside it"""
    shutil.copy2("third_party/SDL3/lib/SDL3.dll", "build/SDL3.dll")
    for dll in SDK_DLLS:
        dest = os.path.join("build", os.path.basename(dll))
        if not os.path.exists(dest) or os.path.getmtime(dest) < os.path.getmtime(dll):
            shutil.copy2(dll, dest)


def runtime(env):
    """build/redline.exe (the runtime, no game code) and build/redline.lib, its exports for the game DLL."""
    sdks()
    headers = glob.glob("runtime/*.h")
    todo = stale(sorted(glob.glob("runtime/*.c")), "build/runtime", headers)
    if todo:
        run(["cl", *CFLAGS, "/Zi", "/Fobuild\\runtime\\", "/Fdbuild\\runtime\\", *todo], env)
    cpp = sorted(glob.glob("runtime/*.cpp"))  # the parts the vendors' C++ headers need (dlss.cpp, fsr3.cpp)
    todo = stale(cpp, "build/runtime", headers)
    if todo:
        run(["cl", *CFLAGS, "/EHsc", "/Zi", "/Fobuild\\runtime\\", "/Fdbuild\\runtime\\", *todo], env)
    # a Windows program: no console window beside the game (its output goes to the launcher's log)
    run(["link", "/nologo", "/DEBUG", "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup", "/out:build\\redline.exe",
         f"/DEF:{exports()}", *glob.glob("build/runtime/*.obj"), "third_party/SDL3/lib/SDL3.lib",
         "third_party/dlss/lib/nvsdk_ngx_s.lib", icon(env)],
        env)
    copy_dlls()
    print("built build\\redline.exe")


def game(env):
    """build/forza_recomp.dll: the lifted code in recomp/, importing the runtime's exports (build/redline.lib)."""
    recomp = sorted(glob.glob("recomp/*.c"))
    todo = stale(recomp, "build/recomp", ["runtime/x86.h", "recomp/funcs.h"])  # all lifted code includes
    dll = "build/forza_recomp.dll"
    if todo:
        print(f"compiling {len(todo)} of {len(recomp)} recomp files")
        # /Zi: line info maps a host crash address back to the lifted line and its guest-address comment
        run(["cl", *CFLAGS, "/DGAME_DLL", "/Zi", "/FS", "/Fobuild\\recomp\\", "/Fdbuild\\recomp\\", *todo], env)
    if todo or not os.path.exists(dll) or os.path.getmtime(dll) < os.path.getmtime("build/redline.lib"):
        run(["link", "/nologo", "/DLL", "/DEBUG", f"/out:{dll}", *glob.glob("build/recomp/*.obj"),
             "build/redline.lib"], env)
    print("built build\\forza_recomp.dll")


def zig(args):
    """run zig; its output, and stop if it failed"""
    r = subprocess.run([ZIG, *args], cwd=ROOT, capture_output=True, text=True)
    if (r.stdout + r.stderr).strip():
        print((r.stdout + r.stderr).strip())
    if r.returncode:
        sys.exit(f"zig {args[0]} failed")


def jobs():
    """compiles at once: one a CPU, as free memory allows (the biggest lifted file took 1.4 GB with clang)"""
    class Mem(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + \
                   [(n, ctypes.c_ulonglong) for n in ("total", "avail", "tpage", "apage", "tvirt", "avirt", "ext")]
    m = Mem(length=ctypes.sizeof(Mem))
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return max(1, min(os.cpu_count() or 1, int(m.avail / 1.5e9)))


def zig_compile(sources, obj_dir, flags):
    """each source to obj_dir/<name>.o, several at once; stops with the first failing file's errors"""
    def one(src):
        obj = os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ".o")
        return src, subprocess.run([ZIG, "c++" if src.endswith(".cpp") else "cc", *ZIG_FLAGS, *flags, "-c", src,
                                    "-o", obj], cwd=ROOT, capture_output=True, text=True)
    with concurrent.futures.ThreadPoolExecutor(jobs()) as pool:
        failed = [(src, r) for src, r in pool.map(one, sources) if r.returncode]
    if failed:
        print((failed[0][1].stdout + failed[0][1].stderr).strip())
        sys.exit(f"zig failed on {failed[0][0]}")


def zig_runtime():
    """build/redline.exe and build/redline.lib with zig: no Visual Studio, and Windows' own C runtime"""
    sdks()
    for d in ("build/zig/runtime", "build/zig/stubs"):
        os.makedirs(d, exist_ok=True)
    todo = stale(sorted(glob.glob("runtime/*.c")) + sorted(glob.glob("runtime/*.cpp")), "build/zig/runtime",
                 glob.glob("runtime/*.h"), ".o")
    if todo:
        zig_compile(todo, "build/zig/runtime", [])
    for lib in NGX_DEFAULTLIBS:  # empty: what NGX takes from them is in msvc_shim.c
        if not os.path.exists(f"build/zig/stubs/{lib}"):
            zig(["ar", "rcs", f"build/zig/stubs/{lib}"])
    zig(["c++", "-target", "x86_64-windows-gnu", "-g", "-o", "build/redline.exe", *sorted(glob.glob("build/zig/runtime/*.o")),
         exports(), "launcher/redline.rc", "third_party/SDL3/lib/SDL3.lib", "third_party/dlss/lib/nvsdk_ngx_s.lib",
         "-Lbuild/zig/stubs", *ZIG_LIBS, "-Wl,--subsystem,windows"])
    zig(["dlltool", "-d", "build/redline.def", "-l", "build/redline.lib", "-D", "redline.exe", "-m", "i386:x86-64"])
    copy_dlls()
    print("built build\\redline.exe")


def zig_game():
    """build/forza_recomp.dll with zig"""
    os.makedirs("build/zig/recomp", exist_ok=True)
    recomp = sorted(glob.glob("recomp/*.c"))
    todo = stale(recomp, "build/zig/recomp", ["runtime/x86.h", "recomp/funcs.h"], ".o")
    dll = "build/forza_recomp.dll"
    if todo:
        print(f"compiling {len(todo)} of {len(recomp)} recomp files, {jobs()} at a time")
        zig_compile(todo, "build/zig/recomp", ["-DGAME_DLL"])
    if todo or not os.path.exists(dll) or os.path.getmtime(dll) < os.path.getmtime("build/redline.lib"):
        zig(["cc", "-target", "x86_64-windows-gnu", "-shared", "-g", "-o", dll,
             *sorted(glob.glob("build/zig/recomp/*.o")), "build/redline.lib"])
    print("built build\\forza_recomp.dll")


def main():
    os.chdir(ROOT)
    if "--zig" in sys.argv:
        if not os.path.exists(ZIG):
            sys.exit(f"{ZIG} was not found: the launcher fetches it (Build)")
        zig_runtime()
        if "--runtime" not in sys.argv:
            zig_game()
        return
    env = msvc_env()
    if "--launcher" in sys.argv:
        launcher(env)
        return
    for d in ("build/recomp", "build/runtime"):
        os.makedirs(d, exist_ok=True)
    runtime(env)
    if "--runtime" not in sys.argv:
        game(env)


if __name__ == "__main__":
    main()
