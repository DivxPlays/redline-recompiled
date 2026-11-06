"""Dump an Xbox XBE: header, certificate, sections, XDK libraries, kernel imports.

Usage: python tools/xbe_info.py <file.xbe> [--json out.json]
"""
import json
import struct
import sys

# (entry point key, kernel thunk key)
KEYS = {"retail": (0xA8FC57AB, 0x5B6D40B6), "debug": (0x94859D4B, 0xEFB1F152)}
SECTION_FLAGS = {0x1: "W", 0x2: "preload", 0x4: "X", 0x8: "inserted", 0x10: "head_ro", 0x20: "tail_ro"}


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def cstr(b, o):
    return b[o:b.index(b"\0", o)].decode("latin-1")


def parse(path):
    b = open(path, "rb").read()
    assert b[:4] == b"XBEH", f"{path}: not an XBE"
    base, hdr_size, img_size = u32(b, 0x104), u32(b, 0x108), u32(b, 0x10C)

    def hdr(va):  # header-region VA -> file offset
        return va - base

    sections = []
    nsec, sec_va = u32(b, 0x11C), u32(b, 0x120)
    for i in range(nsec):
        o = hdr(sec_va) + i * 0x38
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", b, o)
        sections.append({
            "name": cstr(b, hdr(name_va)), "va": va, "vsize": vsize, "raw": raw, "rsize": rsize,
            "flags": [n for bit, n in SECTION_FLAGS.items() if flags & bit],
        })

    def file_off(va):
        for s in sections:
            if s["va"] <= va < s["va"] + s["rsize"]:
                return s["raw"] + va - s["va"]
        if base <= va < base + hdr_size:
            return hdr(va)
        raise ValueError(f"VA {va:#x} not backed by file data")

    for kind, (ep_key, thunk_key) in KEYS.items():
        entry, thunk = u32(b, 0x128) ^ ep_key, u32(b, 0x158) ^ thunk_key
        if base <= entry < base + img_size:
            break
    else:
        raise ValueError("entry point does not decode with retail or debug key")

    imports = []
    o = file_off(thunk)
    while (v := u32(b, o)):
        assert v & 0x80000000 and (v & 0x7FFFFFFF) < 400, f"bad kernel thunk entry {v:#x} - wrong XOR key?"
        imports.append(v & 0x7FFFFFFF)
        o += 4

    libs = []
    nlib, lib_va = u32(b, 0x160), u32(b, 0x164)
    for i in range(nlib):
        name, major, minor, build, flags = struct.unpack_from("<8sHHHH", b, hdr(lib_va) + i * 16)
        libs.append({"name": name.rstrip(b"\0").decode(), "version": f"{major}.{minor}.{build}",
                     "qfe": flags & 0x1FFF, "debug": bool(flags & 0x8000)})

    cert = hdr(u32(b, 0x118))
    return {
        "file": path, "kind": kind, "base": base, "image_size": img_size,
        "entry": entry, "kernel_thunk": thunk, "tls": u32(b, 0x12C),
        "title_id": f"{u32(b, cert + 8):08X}",
        "title": b[cert + 0x0C:cert + 0x5C].decode("utf-16-le").split("\0")[0],
        "region": u32(b, cert + 0xA0), "cert_version": u32(b, cert + 0xAC),
        "debug_path": cstr(b, hdr(u32(b, 0x14C))) if u32(b, 0x14C) else "",
        "sections": sections, "libraries": libs, "kernel_imports": imports,
    }


def report(x):
    print(f"{x['file']}: {x['title']!r} title_id={x['title_id']} ({x['kind']}) region={x['region']:#x}")
    print(f"  base={x['base']:#010x} size={x['image_size']:#x} entry={x['entry']:#010x} "
          f"kthunk={x['kernel_thunk']:#010x} tls={x['tls']:#x}")
    if x["debug_path"]:
        print(f"  pdb path: {x['debug_path']}")
    print(f"  {len(x['sections'])} sections:")
    for s in x["sections"]:
        print(f"    {s['name']:<12} va={s['va']:#010x} vsize={s['vsize']:#9x} raw={s['raw']:#9x} "
              f"rsize={s['rsize']:#9x} {','.join(s['flags'])}")
    print("  libraries: " + ", ".join(f"{l['name']} {l['version']}" for l in x["libraries"]))
    print(f"  {len(x['kernel_imports'])} kernel imports (ordinals): {sorted(x['kernel_imports'])}")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    info = parse(sys.argv[1])
    report(info)
    if len(sys.argv) == 4:
        json.dump(info, open(sys.argv[3], "w"), indent=1)
