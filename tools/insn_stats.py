"""Instruction mnemonic histogram over discovered functions.

Usage: python tools/insn_stats.py <file.xbe> <functions.json>
"""
import collections
import json
import sys

import capstone

from xbe_image import Image

img = Image(sys.argv[1])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
seen, counts, prefixes = set(), collections.Counter(), collections.Counter()
for f in json.load(open(sys.argv[2])):
    for start, end, _ in f["blocks"]:
        if start in seen or end <= start:
            continue
        seen.add(start)
        for insn in md.disasm(img.read(start, end - start), start):
            counts[insn.mnemonic] += 1
total = sum(counts.values())
print(f"{total:,} instructions, {len(counts)} distinct mnemonics")
acc = 0
for i, (m, n) in enumerate(counts.most_common()):
    acc += n
    if i < 60 or n * 10000 >= total:
        print(f"{m:<12}{n:>9,}  {100 * acc / total:6.2f}% cum")
x87 = sum(n for m, n in counts.items() if m.startswith("f"))
sse = sum(n for m, n in counts.items() if m.endswith(("ps", "ss")) or m in ("movd", "movq") or m.startswith("p"))
print(f"x87: {x87:,} ({100 * x87 / total:.1f}%), SSE/MMX-ish: {sse:,} ({100 * sse / total:.1f}%)")
print("all mnemonics:", " ".join(sorted(counts)))
