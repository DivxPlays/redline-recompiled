"""Explain why an address is not a discovered function: who covers it, who points at it.

Usage: python tools/why_missing.py <file.xbe> <functions.json> 0xVA
"""
import json
import sys

import capstone

from xbe_image import Image

img = Image(sys.argv[1])
funcs = json.load(open(sys.argv[2]))
t = int(sys.argv[3], 16)
print("section:", img.section(t)["name"])
print("is function:", any(f["entry"] == t for f in funcs))
print("covered by:", [(hex(f["entry"]), f["source"]) for f in funcs if any(s <= t < e for s, e, _ in f["blocks"])][:5])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i in md.disasm(img.read(t - 8, 0x20), t - 8):
    print(f"  {i.address:08X} {i.mnemonic} {i.op_str}")
for s in img.sections:
    d, needle = s["data"], t.to_bytes(4, "little")
    off = d.find(needle)
    while off != -1:
        print("  referenced from", s["name"], hex(s["va"] + off))
        off = d.find(needle, off + 1)
