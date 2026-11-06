"""Name XDK library functions in an XBE using XbSymbolDatabase signatures (MIT code, ODbL data).

Input is the database's *_OOVPA.c tables run through a C preprocessor (see tools/oovpa_pp.bat), so every macro
and #if is already resolved. Matching follows the reference scanner: per symbol take the highest signature
revision <= the library version; a match needs every (offset, byte) pair and every XREF to agree (an XREF is a
4-byte absolute or PC-relative reference to another symbol). XREFs marked "derive" are learned from the first
match. Passes repeat until nothing new is found.

Usage: python tools/find_symbols.py <file.xbe> <preprocessed dir> <kernel .def> <out.json>
"""
import json
import os
import re
import sys

from gen_kernel_exports import parse as parse_def
from xbe_image import Image

# library -> (preprocessed table, XBE library name, sections to scan); as in the reference SymbolDBList
LIBRARIES = {
    "D3D8": ("D3D8.i", "D3D8", [".text", "D3D"]),
    "DSound": ("DSound.i", "DSOUND", ["DSOUND", ".rdata"]),
    "Xapi": ("Xapi.i", "XAPILIB", [".text", "XPP"]),
    "XGraphic": ("XGraphic.i", "XGRAPHC", [".text", "XGRPH"]),
    "XActEng": ("XActEng.i", "XACTENG", ["XACTENG"]),
}
# xrefs the reference scanner derives from matches instead of checking (libXbSymbolDatabase.c), plus
# D3D_g_pDevice, which it derives in a manual pass
DERIVE = {
    "D3DTSS_TEXCOORDINDEX", "D3D_g_Stream", "D3D_g_Stream_i_pVertexBuffer", "D3D_g_pDevice",
    "D3DDevice__m_PixelShader_OFFSET", "D3DDevice__m_Textures_OFFSET", "D3DDevice__m_Palettes_OFFSET",
    "D3DDevice__m_RenderTarget_OFFSET", "D3DDevice__m_DepthStencil_OFFSET", "D3DDevice__m_VerticalBlankEvent_OFFSET",
    "D3DDevice__m_VertexShader_OFFSET", "XapiCurrentFiber_OFFSET", "XapiLastErrorCode_OFFSET",
    "XapiThreadFiberData_OFFSET", "XapiThreadNotifyRoutineList", "_tls_array", "_tls_index",
    "g_DeviceTypeInfoTableBegin", "g_DeviceTypeInfoTableEnd", "g_DeviceType_Gamepad", "g_DeviceType_IRDongle",
    "g_DeviceType_Keyboard", "g_XapiAltLett_MU", "g_XapiCurrentTopLevelFilter", "g_XapiMountedMUs",
}

ENTRY = re.compile(r"\{\s*([0-9xA-Fa-f+ ]+?)\s*,\s*(?:\.xref\.index\s*=\s*|\.value\s*=\s*)?(\w+)\s*\}")
SIG = re.compile(r"static LOOVPA (\w+?)_(\d+) = \{(.*?)\}\s*;", re.S)
REG = re.compile(r'XREF_(\w+), DB_ST_(\w+), symbol_(\w+), "(\w+)", call_(\w+),(.*?)\(OOVPARevision\[\]\) \{(.*?)\} \}',
                 re.S)
PARAM = re.compile(r'\{ param_(\w+), "(\w*)" \}')
REV = re.compile(r"&(\w+)\.Header, (\d+)")


def parse_tables(path):
    text = open(path).read()
    sigs = {}
    for name, ver, body in SIG.findall(text):
        if "/ sizeof(LOVP))" in body:  # new-style headers repeat the entry list inside a sizeof()
            body = body.split("/ sizeof(LOVP))", 1)[1]
        xrefs, values = [], []
        for off, val in ENTRY.findall(body):
            off = eval(off)  # offsets are literals like "0x01 + 1"
            if val.startswith("XREF_"):
                xrefs.append((off, val[5:]))
            else:
                values.append((off, int(val, 0)))
        sigs[f"{name}_{ver}"] = (xrefs, values)
    symbols = []
    for xref, scan, kind, name, call, middle, revs in REG.findall(text):
        params = PARAM.findall(middle)
        symbols.append({
            "xref": xref, "scan": scan, "kind": kind, "name": name, "call": call,
            "params": [p for _, p in params[:len(params) // 2]] if params else [],
            "revisions": [(sig, int(v)) for sig, v in REV.findall(revs)],
        })
    return sigs, symbols


def anchor(values):
    """Longest run of consecutive offsets: searched with bytes.find before checking the rest."""
    best, run = [], []
    for off, val in sorted(values):
        run = run + [(off, val)] if run and off == run[-1][0] + 1 else [(off, val)]
        if len(run) > len(best):
            best = run
    return best[0][0], bytes(v for _, v in best)


def main(xbe, pp_dir, def_path, out):
    img = Image(xbe)
    lib_versions = {}
    raw = open(xbe, "rb").read()
    base = img.info["base"]
    for lib in img.info["libraries"]:
        lib_versions[lib["name"]] = int(lib["version"].split(".")[-1])

    known = {}  # xref name -> value
    exports = parse_def(def_path)
    for i, ordinal in enumerate(img.info["kernel_imports"]):
        name, conv, _ = exports[ordinal]
        known[("KT_VAR_" if conv == "K_DATA" else "KT_FUNC_") + name] = img.kernel_thunk + 4 * i

    todo = []
    for lib, (table, xbe_lib, sections) in LIBRARIES.items():
        version = lib_versions.get(xbe_lib)
        if version is None:
            continue
        sigs, symbols = parse_tables(os.path.join(pp_dir, table))
        secs = [s for s in img.sections if s["name"] in sections]
        for sym in symbols:
            if sym["scan"] == "MANUAL":
                continue
            revs = [(sig, v) for sig, v in sym["revisions"] if v <= version and sig in sigs]
            if not revs:
                continue
            sig, rev = max(revs, key=lambda r: r[1])
            xrefs, values = sigs[sig]
            if not values:
                continue
            todo.append({**sym, "lib": lib, "rev": rev, "xrefs": xrefs, "values": values, "sections": secs,
                         "anchor": anchor(values)})

    found = {}
    progress = True
    passes = 0
    while progress:
        progress = False
        passes += 1
        for sym in todo:
            if sym["xref"] in found:
                continue
            if any(x not in known and x not in DERIVE for _, x in sym["xrefs"]):
                continue  # depends on a symbol not found yet
            a_off, a_bytes = sym["anchor"]
            hits = []
            for s in sym["sections"]:
                d = s["data"]
                pos = d.find(a_bytes)
                while pos != -1:
                    start = pos - a_off
                    if start >= 0 and all(start + o < len(d) and d[start + o] == v for o, v in sym["values"]):
                        va = s["va"] + start
                        derived, ok = {}, True
                        for o, x in sym["xrefs"]:
                            if start + o + 4 > len(d):
                                ok = False
                                break
                            val = int.from_bytes(d[start + o:start + o + 4], "little")
                            if x in known:
                                if val != known[x] and (val + va + o + 4) & 0xFFFFFFFF != known[x]:
                                    ok = False
                                    break
                            else:
                                derived[x] = val
                        if ok:
                            hits.append((va, derived))
                    pos = d.find(a_bytes, pos + 1)
            if hits:
                va, derived = hits[0]
                for x, val in derived.items():
                    known.setdefault(x, val)
                known[sym["xref"]] = va
                found[sym["xref"]] = {"name": sym["name"], "lib": sym["lib"], "va": va, "kind": sym["kind"],
                                      "call": sym["call"], "params": sym["params"], "rev": sym["rev"],
                                      "matches": len(hits)}
                progress = True

    result = sorted(found.values(), key=lambda f: f["va"])
    variables = {k: v for k, v in known.items() if k in DERIVE}
    json.dump({"symbols": result, "derived": variables}, open(out, "w"), indent=1)
    by_lib = {}
    for f in result:
        by_lib[f["lib"]] = by_lib.get(f["lib"], 0) + 1
    dup = [f["name"] for f in result if f["matches"] > 1]
    print(f"{len(result)} symbols in {passes} passes: {by_lib}; candidates {len(todo)}")
    print(f"derived: { {k: hex(v) for k, v in variables.items()} }")
    if dup:
        print(f"{len(dup)} with several matches (first taken): {dup[:12]}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
