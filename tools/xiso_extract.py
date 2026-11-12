"""Extract an Xbox XDVDFS image: an XISO, or a full-disc (Redump) dump.

Usage: python tools/xiso_extract.py <image.iso> <out_dir>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game's XDVDFS volume starts: 0 in an XISO; in a full-disc dump, after the disc's video partition, at
# extract-xiso's offsets for XGD1 (every original Xbox disc), XGD2 and XGD3. Its sector numbers count from there.
VOLUME_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)


def extract(iso_path, out_dir):
    with open(iso_path, "rb") as f:
        for base in VOLUME_OFFSETS:
            f.seek(base + 32 * SECTOR)
            vd = f.read(SECTOR)
            if vd[:20] == MAGIC and vd[0x7EC:0x800] == MAGIC:
                break
        else:
            sys.exit("This .iso is not an Xbox disc image: no XDVDFS volume where an XISO or a full-disc dump has one.")
        if base:
            print(f"a full-disc image: the game's volume starts at {base:#x}")
        root_sector, root_size = struct.unpack_from("<II", vd, 20)
        count = 0
        total = 0

        def walk(dir_sector, dir_size, path):
            nonlocal count, total
            if dir_size == 0:
                return
            f.seek(base + dir_sector * SECTOR)
            table = f.read(dir_size)
            seen = set()

            # Entries form a binary tree; left/right are dword offsets into the table.
            stack = [0]
            while stack:
                off = stack.pop()
                if off in seen or off + 14 > len(table):
                    continue
                seen.add(off)
                left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
                if left == 0xFFFF:  # padding / empty directory
                    continue
                name = table[off + 14:off + 14 + nlen].decode("latin-1")
                if left:
                    stack.append(left * 4)
                if right:
                    stack.append(right * 4)
                target = os.path.join(path, name)
                if attr & 0x10:
                    os.makedirs(target, exist_ok=True)
                    walk(sector, size, target)
                else:
                    f.seek(base + sector * SECTOR)
                    remaining = size
                    with open(target, "wb") as out:
                        while remaining:
                            chunk = f.read(min(remaining, 1 << 22))
                            if not chunk:
                                sys.exit(f"truncated image while reading {target}")
                            out.write(chunk)
                            remaining -= len(chunk)
                    count += 1
                    total += size

        os.makedirs(out_dir, exist_ok=True)
        walk(root_sector, root_size, out_dir)

    xbe = os.path.join(out_dir, "default.xbe")
    assert os.path.exists(xbe), "default.xbe missing - extraction likely wrong"
    with open(xbe, "rb") as x:
        assert x.read(4) == b"XBEH", "default.xbe has bad magic"
    print(f"extracted {count} files, {total / 2**20:.1f} MiB -> {out_dir}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    extract(sys.argv[1], sys.argv[2])
