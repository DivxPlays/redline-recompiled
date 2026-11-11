"""Preprocess XbSymbolDatabase's signature tables into build/oovpa/*.i for tools/find_symbols.py. Only the C
preprocessor runs (nothing from the database is compiled or executed): zig's clang when the launcher has fetched it
(build/tools), else Visual Studio's cl /EP. Both give find_symbols.py the same symbols.

Usage: python tools/oovpa_pp.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import build  # noqa: E402  (its zig path and Visual Studio environment)

DB = os.path.join(ROOT, "third_party", "XbSymbolDatabase")
INC = [os.path.join(DB, d) for d in ("include", "src", os.path.join("src", "lib"), os.path.join("src", "OOVPADatabase"))]


def main():
    out = os.path.join(ROOT, "build", "oovpa")
    os.makedirs(out, exist_ok=True)
    zig = os.path.exists(build.ZIG)
    env = None if zig else build.msvc_env()
    for lib in ("D3D8", "DSound", "Xapi", "XGraphic", "XActEng"):
        src = os.path.join(DB, "src", "OOVPADatabase", f"{lib}_OOVPA.c")
        if zig:
            cmd = [build.ZIG, "cc", "-E", "-P", "-target", "x86_64-windows-gnu", *(f"-I{d}" for d in INC), src]
        else:
            cmd = [build.shutil.which("cl", path=env.get("PATH") or env.get("Path")), "/nologo", "/EP", "/Zc:preprocessor",
                   *(f"/I{d}" for d in INC), src]
        with open(os.path.join(out, f"{lib}.i"), "w") as i, open(os.path.join(out, f"{lib}.err"), "w") as e:
            if subprocess.run(cmd, stdout=i, stderr=e, env=env).returncode:
                sys.exit(f"the preprocessor failed on {src}: see build/oovpa/{lib}.err")


if __name__ == "__main__":
    main()
