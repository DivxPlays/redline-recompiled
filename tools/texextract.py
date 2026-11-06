"""Every texture on the disc, written to the texture dump the way the game writes it while playing (TEXTURE DUMP),
so a texture pack can cover all tracks, cars and menus without driving each one.

Usage: python tools/texextract.py [game dir] [dump dir]      (defaults: game, build/textures/dump)

Textures live in three kinds of file, all walked recursively (zlib streams are inflated first):
  XPR0 packed resources (menus, fonts, liveries): D3D resource headers {Common, Data, Lock, Format, Size} up to
      0xFFFFFFFF, each texture's bytes at header size + Data.
  Forza resource bundles (magic AAAAC0C0: track .xb2 files, car.carbin and the like): 0x12E bytes of header, then
      count entries {hash, offset, stored size, size, name offset, header offset}, a 4-byte word, the name table
      (header word 6 bytes) and the D3D headers (header word 10 bytes; 20 per texture, 12 per vertex buffer). A
      texture entry's bytes are its texture.
  Forza archives (the same container with no header table: .cab/.ca2): entries are files, zlib-compressed when the
      stored size is smaller.
A texture's key is the renderer's (render.c texture()): the runtime's hash (texhash.c, built here into a DLL) of its
guest bytes, then of {colour format, width, height} seeded with that. Keys already in the dump are skipped. Palette
(P8), cube and volume textures are left out, as the renderer never replaces them.
The game often draws a texture from a lower mip level down (distant objects, and while a track streams in: Silverstone
showed 128x128 textures as 32x32 from level 2), which is a texture of its own to the renderer. Those keys go to
dump/aliases.txt as "<key> <key of the whole texture>", so a pack gives them the whole texture's replacement.
The textures of media/Fonts are listed in dump/nopack.txt: the UI fills boxes from single texels of its text font
(Arial_16.xpr), which an upscale blurs (the main menu's black bars turned see-through), so packs leave fonts alone.
"""
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# colour format -> (kind, swizzled, bytes per pixel; 0 = DXT block size 8 or 16 given as -8/-16)
FORMATS = {0x00: ("L8", 1, 1), 0x02: ("1555", 1, 2), 0x03: ("X555", 1, 2), 0x04: ("4444", 1, 2), 0x05: ("565", 1, 2),
           0x06: ("ARGB8", 1, 4), 0x07: ("XRGB8", 1, 4), 0x0C: ("DXT1", 0, -8), 0x0E: ("DXT3", 0, -16),
           0x0F: ("DXT5", 0, -16), 0x10: ("1555", 0, 2), 0x11: ("565", 0, 2), 0x12: ("ARGB8", 0, 4),
           0x13: ("L8", 0, 1), 0x19: ("A8", 1, 1), 0x1A: ("A8L8", 1, 2), 0x1C: ("X555", 0, 2), 0x1D: ("4444", 0, 2),
           0x1E: ("XRGB8", 0, 4)}
FOURCC = {"DXT1": b"DXT1", "DXT3": b"DXT3", "DXT5": b"DXT5"}


def hash_dll():
    """build/texhash.dll from tools/texhash.c, with the MSVC environment tools/build.py uses"""
    dll = os.path.join(ROOT, "build", "texhash.dll")
    src = os.path.join(ROOT, "tools", "texhash.c")
    if not os.path.exists(dll) or os.path.getmtime(dll) < max(os.path.getmtime(src),
                                                               os.path.getmtime(os.path.join(ROOT, "runtime", "pgraph.h"))):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import build
        env = build.msvc_env()
        cl = shutil.which("cl", path=env.get("PATH") or env.get("Path"))
        os.makedirs(os.path.join(ROOT, "build", "texhash"), exist_ok=True)
        r = subprocess.run([cl, "/nologo", "/O2", "/LD", "/Iruntime", "/Fobuild\\texhash\\", "tools\\texhash.c",
                            "/link", "/out:build\\texhash.dll", "/implib:build\\texhash\\texhash.lib"],
                           cwd=ROOT, env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit("texhash.dll did not build:\n" + r.stdout + r.stderr)
    lib = ctypes.CDLL(dll)
    lib.texhash.restype = ctypes.c_uint64
    lib.texhash.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64]
    return lib.texhash


def max1(v):
    return v if v else 1


def describe(fmt, size):
    """(colour format, kind info, width, height, guest bytes) of a 2D texture header, or None"""
    color = (fmt >> 8) & 0xFF
    if color not in FORMATS or fmt & 4 or ((fmt >> 4) & 15) != 2:  # cube maps, volumes, palettes: never replaced
        return None
    kind, swizzled, bpp = FORMATS[color]
    if not swizzled and bpp > 0:  # linear: Size holds width-1, height-1, pitch/64-1
        w, h, pitch = (size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1, ((size >> 24) + 1) * 64
        return color, FORMATS[color], w, h, pitch * h, pitch
    w, h, n = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15), max1((fmt >> 16) & 15)
    total = 0
    for l in range(n):
        lw, lh = max1(w >> l), max1(h >> l)
        total += lw * lh * bpp if bpp > 0 else ((lw + 3) // 4) * ((lh + 3) // 4) * -bpp
    return color, FORMATS[color], w, h, total, 0


def swizzle_table(w, h):
    """NV2A swizzle offset of each (y, x): x and y bits interleaved, x first, while each still has bits"""
    off = np.zeros((h, w), np.int64)
    ys, xs = np.mgrid[0:h, 0:w]
    bit, m = 0, 1
    while m < w or m < h:
        if m < w:
            off |= ((xs & m) != 0).astype(np.int64) << bit
            bit += 1
        if m < h:
            off |= ((ys & m) != 0).astype(np.int64) << bit
            bit += 1
        m <<= 1
    return off


def to_bgra(kind, px):
    """pixels (n, bytes) of a kind -> uint32 BGRA"""
    p = px.astype(np.uint32)
    v16 = p[:, 0] | p[:, 1] << 8 if p.shape[1] >= 2 else None

    def ex(v, bits):
        return (v << (8 - bits)) | (v >> (2 * bits - 8)) if bits > 4 else v * 17 if bits == 4 else v
    a = np.full(len(p), 255, np.uint32)
    if kind in ("ARGB8", "XRGB8"):
        r, g, b = p[:, 2], p[:, 1], p[:, 0]
        if kind == "ARGB8":
            a = p[:, 3]
    elif kind == "565":
        r, g, b = ex(v16 >> 11, 5), ex((v16 >> 5) & 63, 6), ex(v16 & 31, 5)
    elif kind in ("1555", "X555"):
        r, g, b = ex((v16 >> 10) & 31, 5), ex((v16 >> 5) & 31, 5), ex(v16 & 31, 5)
        if kind == "1555":
            a = np.where(v16 >> 15, 255, 0).astype(np.uint32)
    elif kind == "4444":
        a, r, g, b = (v16 >> 12) * 17, ((v16 >> 8) & 15) * 17, ((v16 >> 4) & 15) * 17, (v16 & 15) * 17
    elif kind == "L8":
        r = g = b = p[:, 0]
    elif kind == "A8":
        r = g = b = np.zeros(len(p), np.uint32)
        a = p[:, 0]
    else:  # A8L8
        r = g = b = p[:, 0]
        a = p[:, 1]
    return (a << 24 | r << 16 | g << 8 | b).astype(np.uint32)


def write_dds(path, w, h, info, data, pitch):
    kind, swizzled, bpp = info
    hd = [0] * 32
    hd[0], hd[1], hd[2], hd[3], hd[4], hd[19], hd[27] = 0x20534444, 124, 0x1007, h, w, 32, 0x1000
    if bpp < 0 and w % 4 == 0 and h % 4 == 0:  # DXT blocks as they are
        n = (w // 4) * (h // 4) * -bpp
        hd[2] |= 0x80000
        hd[5], hd[20], hd[21] = n, 4, struct.unpack("<I", FOURCC[kind])[0]
        body = data[:n]
    else:
        if bpp < 0:  # a DXT texture D3D11 cannot take whole blocks of: decoded with PIL
            return  # shortcut: too small to matter for a pack (under 4 px a side), so not written
        if swizzled:
            px = np.frombuffer(data[:w * h * bpp], np.uint8).reshape(-1, bpp)[swizzle_table(w, h).ravel()]
        else:
            rows = np.frombuffer(data[:pitch * h], np.uint8).reshape(h, pitch)[:, :w * bpp]
            px = rows.reshape(-1, bpp)
        body = to_bgra(kind, px).tobytes()
        hd[2] |= 0x8
        hd[5], hd[20], hd[22], hd[23], hd[24], hd[25], hd[26] = w * 4, 0x41, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000
    with open(path, "wb") as f:
        f.write(struct.pack("<32I", *hd))
        f.write(body)


class Extractor:
    def __init__(self, dump):
        self.dump, self.hash = dump, hash_dll()
        self.have = {f[:16].upper() for f in os.listdir(dump)} if os.path.isdir(dump) else set()
        os.makedirs(dump, exist_ok=True)
        self.seen = self.written = 0
        self.aliases, self.full, self.fonts = {}, set(), set()
        self.font = False  # the file being read is a font

    def texture(self, fmt, size, data):
        d = describe(fmt, size)
        if not d:
            return
        color, info, w, h, n, pitch = d
        if len(data) < n or w > 4096 or h > 4096:
            return
        content = bytes(data[:n])
        hc = self.hash(content, n, 0)
        key = "%016X" % self.hash(struct.pack("<3I", color, w, h), 12, hc)
        self.seen += 1
        self.full.add(key)
        if self.font:
            self.fonts.add(key)
        kind, swizzled, bpp = info
        off, levels = 0, max1((fmt >> 16) & 15)
        for l in range(1, levels if pitch == 0 else 0):  # the chain from each lower level on
            lw, lh = max1(w >> (l - 1)), max1(h >> (l - 1))
            off += lw * lh * bpp if bpp > 0 else ((lw + 3) // 4) * ((lh + 3) // 4) * -bpp
            if max1(w >> l) < 4 or max1(h >> l) < 4:
                break
            tail = content[off:]
            sub = "%016X" % self.hash(struct.pack("<3I", color, max1(w >> l), max1(h >> l)), 12, self.hash(tail, len(tail), 0))
            self.aliases.setdefault(sub, key)
        if key in self.have:
            return
        self.have.add(key)
        write_dds(os.path.join(self.dump, key + ".dds"), w, h, info, content, pitch)
        self.written += 1

    def xpr(self, b):
        hsize = struct.unpack_from("<I", b, 8)[0]
        o = 12
        while o + 4 <= hsize:
            common = struct.unpack_from("<I", b, o)[0]
            if common == 0xFFFFFFFF:
                break
            kind = (common >> 16) & 7
            if kind == 4:  # texture
                data, _, fmt, size = struct.unpack_from("<4I", b, o + 4)
                self.texture(fmt, size, memoryview(b)[hsize + data:])
                o += 20
            elif kind in (0, 3):  # vertex buffer, palette
                o += 12
            else:
                break

    def bundle(self, b, name):
        count, names_size, headers_size = struct.unpack_from("<I", b, 8)[0], struct.unpack_from("<I", b, 24)[0], \
            struct.unpack_from("<I", b, 40)[0]
        ents = [struct.unpack_from("<6I", b, 0x12E + 24 * i) for i in range(count)]
        names_at = 0x12E + 24 * count + 4
        headers_at = names_at + names_size
        for _, off, stored, size, name_off, hdr in ents:
            entry = b[off:off + stored]
            if headers_size:  # a resource bundle: each entry is the data of the resource its header describes
                common = struct.unpack_from("<I", b, headers_at + hdr)[0]
                if (common >> 16) & 7 == 4:
                    _, _, fmt, tsize = struct.unpack_from("<4I", b, headers_at + hdr + 4)
                    self.texture(fmt, tsize, entry)
                continue
            if stored < size:
                try:
                    entry = zlib.decompress(entry)
                except zlib.error:
                    continue
            end = b.find(b"\0", names_at + name_off)
            self.blob(entry, name + "/" + b[names_at + name_off:end].decode("latin-1"))

    def blob(self, b, name):
        if b[:4] == b"XPR0":
            self.xpr(b)
        elif b[:4] == b"\xaa\xaa\xc0\xc0":
            self.bundle(b, name)
        elif b[:1] == b"\x78" and b[1:2] in (b"\xda", b"\x9c", b"\x01", b"\x5e"):
            out, rest = [], b
            while rest[:1] == b"\x78":  # a chain of zlib streams (track .xb2 files: 4 MiB each)
                z = zlib.decompressobj()
                try:
                    out.append(z.decompress(rest))
                except zlib.error:
                    break
                rest = z.unused_data.lstrip(b"\0")
            if out:
                self.blob(b"".join(out), name)


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "game")
    dump = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "textures", "dump")
    x = Extractor(dump)
    files = []
    for root, _, fs in os.walk(os.path.join(game, "media")):
        files += [os.path.join(root, f) for f in fs if not f.lower().endswith((".bik", ".wma", ".xwb", ".xsb", ".sxwad"))]
    for i, path in enumerate(sorted(files)):
        x.font = os.sep + "fonts" + os.sep in path.lower()
        with open(path, "rb") as f:
            x.blob(f.read(), os.path.relpath(path, game))
        if i % 50 == 0:
            print(f"  {i}/{len(files)} files, {x.seen} textures, {x.written} new", flush=True)
    path = os.path.join(dump, "aliases.txt")
    old = dict(line.split() for line in open(path)) if os.path.exists(path) else {}
    old.update(x.aliases)
    old = {k: v for k, v in old.items() if k not in x.full}  # a lower level that is a whole texture too stays itself
    with open(path, "w") as f:
        f.writelines(f"{k} {v}\n" for k, v in sorted(old.items()))
    with open(os.path.join(dump, "nopack.txt"), "w") as f:
        f.writelines(k + "\n" for k in sorted(x.fonts))
    print(f"{x.seen} textures in {len(files)} files; {x.written} new ones written to {dump}, {len(old)} aliases")


if __name__ == "__main__":
    main()
