"""Recursive-descent function discovery for an XBE.

Seeds: entry point, direct call targets, and code pointers found in data sections
(vtables, callbacks, SEH handlers). Jump tables are resolved heuristically.

Usage: python tools/find_functions.py <file.xbe> <out.json>
"""
import collections
import json
import re
import sys

import capstone
from capstone import x86

from xbe_image import Image, CODE_SECTIONS

MAX_JUMP_TABLE = 1024
# Kernel ordinals that never return: KeBugCheck, KeBugCheckEx, HalReturnToFirmware, PsTerminateSystemThread
NORETURN_KERNEL = {95, 96, 49, 258}


class Discoverer:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.insns = {}  # va -> CsInsn
        self.functions = {}  # entry -> source tag
        self.bad = set()  # entries whose code decoded to garbage
        self.imm_ptrs = set()  # code addresses seen as push/mov immediates

    def decode(self, va):
        if va in self.insns:
            return self.insns[va]
        code = self.img.read(va, 16)
        if not code or not self.img.is_code(va):
            return None
        insn = next(self.md.disasm(code, va), None)
        if insn is not None:
            self.insns[va] = insn
        return insn

    def jump_table(self, block_insns, insn):
        """(targets, table, [first byte, end]) of `jmp [reg*4 + table]`, or None if not that shape."""
        op = insn.operands[0]
        if op.type != x86.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            return None
        table, index = op.mem.disp & 0xFFFFFFFF, op.mem.index
        limit, bounded = MAX_JUMP_TABLE, False
        for prev in reversed(block_insns):
            # MSVC bounds the switch with `cmp reg, N` / `ja default` right before the jump
            if prev.id == x86.X86_INS_CMP and prev.operands[0].type == x86.X86_OP_REG \
                    and prev.operands[0].reg == index and prev.operands[1].type == x86.X86_OP_IMM:
                limit, bounded = prev.operands[1].imm + 1, True
                break
        targets, first, end = [], table, table
        for i in range(limit):
            t = self.img.u32(table + 4 * i)
            if t is None or not self.img.is_code(t):
                if i == 0 and not bounded:
                    continue  # CRT memcpy indexes from 1 (`and eax, 3` with eax != 0); slot 0 is padding
                break
            targets.append(t)
            end = table + 4 * (i + 1)
        if not bounded:
            # CRT memcpy also does `neg ecx; jmp [ecx*4 + table]`: entries sit before the table address
            for i in range(1, 32):
                t = self.img.u32(table - 4 * i)
                if t is None or not self.img.is_code(t):
                    break
                targets.append(t)
                first = table - 4 * i
        return targets, table, [first, end]

    def walk(self, entry):
        """Explore one function. Returns its description dict."""
        blocks, calls, tails, unresolved, tables = {}, set(), set(), [], []
        work = [entry]
        while work:
            start = work.pop()
            if start in blocks:
                continue
            va, block = start, []
            while True:
                insn = self.decode(va)
                if insn is None:
                    blocks[start] = (start, va, "bad")
                    break
                block.append(insn)
                groups = set(insn.groups)
                if insn.id in (x86.X86_INS_PUSH, x86.X86_INS_MOV) and insn.operands[-1].type == x86.X86_OP_IMM:
                    imm = insn.operands[-1].imm & 0xFFFFFFFF
                    if self.img.is_code(imm):  # function pointer passed as an argument (thread/callback)
                        self.imm_ptrs.add(imm)
                nxt = va + insn.size
                if insn.id == x86.X86_INS_CALL:
                    op = insn.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        calls.add(op.imm & 0xFFFFFFFF)
                    elif op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 \
                            and self.img.kernel_ordinal(op.mem.disp & 0xFFFFFFFF) in NORETURN_KERNEL:
                        blocks[start] = (start, nxt, "noreturn")
                        break
                    else:
                        unresolved.append(va)
                elif x86.X86_GRP_RET in groups or insn.id in (x86.X86_INS_INT3, x86.X86_INS_HLT, x86.X86_INS_UD2):
                    blocks[start] = (start, nxt, "ret")
                    break
                elif insn.id == x86.X86_INS_JMP:
                    op = insn.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        target = op.imm & 0xFFFFFFFF
                        if target in self.functions and target != entry:
                            tails.add(target)
                        else:
                            work.append(target)
                    else:
                        jt = self.jump_table(block, insn)
                        if jt:
                            targets, table, span = jt
                            tables.append({"at": va, "table": table, "targets": targets, "bytes": span})
                            work.extend(targets)
                        else:
                            unresolved.append(va)
                    blocks[start] = (start, nxt, "jmp")
                    break
                elif x86.X86_GRP_JUMP in groups:  # jcc / loop / jecxz
                    work.append(insn.operands[0].imm & 0xFFFFFFFF)
                    work.append(nxt)
                    blocks[start] = (start, nxt, "jcc")
                    break
                va = nxt
                if va in blocks:  # fell into an existing block
                    blocks[start] = (start, va, "fall")
                    break
        bad = any(kind == "bad" for _, _, kind in blocks.values())
        return {
            "entry": entry, "blocks": sorted(blocks.values()), "calls": sorted(calls), "tailcalls": sorted(tails),
            "unresolved_indirect": unresolved, "jump_tables": tables, "bad": bad,
        }

    def data_pointer_seeds(self, covered):
        """Code addresses stored in data: data sections, plus bytes of code sections that are not
        reached code (pointer tables embedded in code, e.g. FZGINIT's static-initializer list)."""
        seeds = set()
        for s in self.img.sections:
            in_code = s["name"] in CODE_SECTIONS
            d = s["data"]
            for off in range(0, len(d) - 3, 4):
                if in_code and any(s["va"] + off + k in covered for k in range(4)):
                    continue
                v = int.from_bytes(d[off:off + 4], "little")
                if self.img.is_code(v):
                    seeds.add(v)
        return seeds

    def crt_dispatch_seeds(self):
        """Handlers in the CRT's x87 transcendental table (fmod, exp, sinh, cosh, tanh): 32-byte entries holding a
        length-prefixed name zero-padded to +0xC, then at +0x10 four handlers the dispatcher jumps to through
        [entry + 0x10 + 4 * operand class]. The entries are unaligned, so the aligned pointer scan misses them."""
        seeds = set()
        for s in self.img.sections:
            if s["name"] in CODE_SECTIONS:
                continue
            d = s["data"]
            for m in re.finditer(rb"[\x01-\x0b](?=[a-z])", d):
                o, n = m.start(), d[m.start()]
                name, pad = d[o + 1:o + 1 + n], d[o + 1 + n:o + 0xC]
                if o + 0x20 > len(d) or not re.fullmatch(rb"[a-z]+", name) or pad.strip(b"\0"):
                    continue
                ptrs = [int.from_bytes(d[o + 0x10 + 4 * k:o + 0x14 + 4 * k], "little") for k in range(4)]
                if all(self.img.is_code(p) for p in ptrs):
                    seeds.update(ptrs)
        return seeds

    def run(self):
        results, covered, case_labels, table_bytes = {}, set(), set(), set()

        def drain(queue):
            while queue:
                va, tag = queue.popleft()
                if va in self.functions:
                    continue
                if not self.img.is_code(va):
                    self.bad.add(va)
                    continue
                self.functions[va] = tag
                f = results[va] = self.walk(va)
                for start, end, _ in f["blocks"]:
                    covered.update(range(start, end))
                case_labels.update(t for jt in f["jump_tables"] for t in jt["targets"])
                table_bytes.update(a for jt in f["jump_tables"] for a in range(*jt["bytes"]))
                queue.extend((c, "call") for c in f["calls"] if c not in self.functions)

        drain(collections.deque([(self.img.info["entry"], "entry")]))
        # Data pointers into code already reached are labels inside functions (SEH handlers, tables),
        # not function starts; seeding them duplicated whole functions.
        self.skipped_seeds = 0
        # jumped to as entry points, so seeded even inside covered code
        forced = self.crt_dispatch_seeds()
        # each phase can uncover code holding more immediates/pointers; iterate until stable
        seen = set()
        while True:
            fresh = (self.imm_ptrs | self.data_pointer_seeds(covered) | forced) - seen
            if not fresh:
                break
            for v in sorted(fresh):
                seen.add(v)
                if v in self.functions:
                    continue
                # a switch's jump table embedded in code: decoding it as a function runs on into the real function
                # after it, which then never gets its own entry (0x241420 swallowed the vtable method 0x241430)
                if v in table_bytes and v not in forced:
                    self.skipped_seeds += 1
                    continue
                # inside reached code it is normally a label, unless it follows int3/nop alignment padding or
                # an instruction that never falls through (ret / ret n / jmp rel32 / jmp rel8): then another walk
                # (a bogus one, or a tail jump) ran over a real function start
                # (switch cases often follow such an instruction too; those are jump-table targets)
                after_pad = self.img.read(v - 1, 1) in (b"\xcc", b"\x90")
                after_ret = self.img.read(v - 1, 1) == b"\xc3" or self.img.read(v - 3, 1) == b"\xc2"
                after_jmp = self.img.read(v - 5, 1) == b"\xe9" or self.img.read(v - 2, 1) == b"\xeb"
                # push ebp / mov ebp, esp: a frame prologue (a jump table decoded as code can cover a real start)
                prologue = self.img.read(v, 3) == b"\x55\x8b\xec"
                if v in covered and v not in forced and not after_pad and \
                        not ((after_ret or after_jmp or prologue) and v not in case_labels):
                    self.skipped_seeds += 1
                    continue
                drain(collections.deque([(v, "imm_ptr" if v in self.imm_ptrs else "data_ptr")]))
        # pointer seeds that decode to garbage were just values that happened to look like code addresses
        return {va: f | {"source": self.functions[va]} for va, f in results.items()
                if not (f["bad"] and self.functions[va] in ("data_ptr", "imm_ptr"))}


def main(xbe, out):
    img = Image(xbe)
    d = Discoverer(img)
    funcs = d.run()
    if d.bad:
        print(f"{len(d.bad)} call targets outside code sections (garbage decode): "
              f"{[hex(v) for v in sorted(d.bad)[:10]]}")
    by_section = collections.Counter(img.section(f["entry"])["name"] for f in funcs.values())
    covered = set()
    for f in funcs.values():
        for start, end, _ in f["blocks"]:
            covered.update(range(start, end))
    code_bytes = sum(s["vsize"] for s in img.sections if s["name"] in CODE_SECTIONS)
    src = collections.Counter(f["source"] for f in funcs.values())
    print(f"{len(funcs)} functions ({dict(src)}), {sum(f['bad'] for f in funcs.values())} with undecodable blocks, "
          f"{d.skipped_seeds} mid-function data pointers skipped")
    print(f"coverage: {len(covered):,} / {code_bytes:,} code bytes ({100 * len(covered) / code_bytes:.1f}%)")
    print(f"jump tables: {sum(len(f['jump_tables']) for f in funcs.values())}, "
          f"unresolved indirect jmp/call sites: {sum(len(f['unresolved_indirect']) for f in funcs.values())}")
    print("functions per section:", dict(by_section.most_common()))
    json.dump([funcs[k] for k in sorted(funcs)], open(out, "w"))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
