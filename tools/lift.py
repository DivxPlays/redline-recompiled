"""Static recompiler: lift discovered x86 functions of an XBE to C.

Every guest function becomes `void f_XXXXXXXX(X86Ctx *c)`. The guest stack is kept
faithful: `call` pushes the real return address and `ret` pops it, so code that
inspects its stack frame still works. Anything not handled yet becomes
x86_unimpl(), so the output always compiles and the gaps are countable.

Functions named in an --hle list are not lifted; their body becomes a call to the host function
hle_<Name>(X86Ctx *c) in runtime/, which must honour the guest calling convention (pop its arguments).

Usage: python tools/lift.py <file.xbe> <functions.json> <out_dir> [--hle runtime/hle.txt --symbols symbols.json]
                            [--only 0xVA,...]
"""
import collections
import json
import os
import sys

import capstone
from capstone import x86

from xbe_image import Image

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = {r[1:]: r for r in REG32}  # ax -> eax
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
CC = {"o": "O", "no": "NO", "b": "B", "ae": "AE", "e": "E", "ne": "NE", "be": "BE", "a": "A",
      "s": "S", "ns": "NS", "p": "P", "np": "NP", "l": "L", "ge": "GE", "le": "LE", "g": "G"}
FUNCS_PER_FILE = 400
MMIO_SECTIONS = {"D3D"}  # sections whose code touches device registers


class Unsupported(Exception):
    pass


def u32(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class Lifter:
    def __init__(self, img, funcs, hle=(), trace=()):
        self.trace = set(trace)
        self.hle = set(hle)  # host-implemented entries: never inlined, even when a jump reaches one
        self.img = img
        self.funcs = {f["entry"]: f for f in funcs}
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.unimpl = collections.Counter()
        self.mem = "M"  # memory accessor prefix; "MR" inside driver code (see lift_function)
        self.lifted = 0

    # ---------- operands ----------
    def reg_read(self, name):
        if name in REG32:
            return f"c->{name}"
        if name in REG16:
            return f"(uint16_t)c->{REG16[name]}"
        if name in REG8L:
            return f"(uint8_t)c->{REG8L[name]}"
        if name in REG8H:
            return f"(uint8_t)(c->{REG8H[name]} >> 8)"
        raise Unsupported(f"reg {name}")

    def reg_write(self, name, val):
        if name in REG32:
            return f"c->{name} = (uint32_t)({val});"
        if name in REG16:
            r = REG16[name]
            return f"c->{r} = (c->{r} & 0xFFFF0000u) | (uint16_t)({val});"
        if name in REG8L:
            r = REG8L[name]
            return f"c->{r} = (c->{r} & 0xFFFFFF00u) | (uint8_t)({val});"
        if name in REG8H:
            r = REG8H[name]
            return f"c->{r} = (c->{r} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({val}) << 8);"
        raise Unsupported(f"reg {name}")

    def addr(self, insn, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(insn.reg_name(m.base)))
        if m.index:
            idx = self.reg_read(insn.reg_name(m.index))
            parts.append(f"{idx} * {m.scale}" if m.scale != 1 else idx)
        if m.disp or not parts:
            parts.append(u32(m.disp))
        if m.segment and insn.reg_name(m.segment) == "fs":
            parts.append("c->fs_base")
        elif m.segment and insn.reg_name(m.segment) == "gs":
            raise Unsupported("gs segment")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def read(self, insn, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            return self.reg_read(insn.reg_name(op.reg))
        if op.type == x86.X86_OP_IMM:
            return u32(op.imm & ((1 << (8 * size)) - 1))
        if op.type == x86.X86_OP_MEM:
            if size not in (1, 2, 4, 8):
                raise Unsupported(f"{size}-byte memory operand")
            return f"{self.mem}{8 * size}({self.addr(insn, op)})"
        raise Unsupported("operand")

    def write(self, insn, op, val):
        if op.type == x86.X86_OP_REG:
            return self.reg_write(insn.reg_name(op.reg), val)
        if op.type == x86.X86_OP_MEM:
            if op.size not in (1, 2, 4, 8):
                raise Unsupported(f"{op.size}-byte memory operand")
            if self.mem == "MR":  # driver code: the write may hit a device register
                return f"MW{8 * op.size}({self.addr(insn, op)}, {val});"
            return f"M{8 * op.size}({self.addr(insn, op)}) = ({val});"
        raise Unsupported("write operand")

    def xmm(self, insn, op):
        name = insn.reg_name(op.reg)
        if not name.startswith("xmm"):
            raise Unsupported(f"reg {name}")
        return f"c->xmm[{name[3:]}]"

    def sti(self, insn, op):
        name = insn.reg_name(op.reg)
        return int(name[3]) if name.startswith("st(") else None

    # ---------- per-function ----------
    def lift_function(self, f):
        entry = f["entry"]
        # driver code reaches GPU registers through pointers, so its plain loads/stores go through the
        # range-checked MR/MW accessors; everything else uses direct memory access
        self.mem = "MR" if self.img.section(entry)["name"] in MMIO_SECTIONS else "M"
        blocks = f["blocks"]
        labels = {b[0] for b in blocks}
        out = [f"void f_{entry:08X}(X86Ctx *c) {{", f"    Flags f = {{0}}; (void)f; c->last_fn = 0x{entry:08X}u;"]
        if entry in self.trace:  # --trace: log entry (runtime.c trace_enter)
            out.append(f"    trace_enter(c, 0x{entry:08X}u);")
        if blocks[0][0] != entry:
            out.append(f"    goto L_{entry:08X};")
        for start, end, kind in blocks:
            out.append(f"L_{start:08X}:;")
            if kind == "bad" or end <= start:
                out.append(f"    x86_unimpl(c, {u32(start)}, \"undecodable code\"); return;")
                continue
            last = None
            for insn in self.md.disasm(self.img.read(start, end - start), start):
                last = insn
                text = f"{insn.mnemonic} {insn.op_str}".strip().replace("\\", "\\\\").replace('"', "'")
                try:
                    lines = self.insn(insn, labels, f)
                    self.lifted += 1
                except Unsupported as e:
                    self.unimpl[insn.mnemonic] += 1
                    lines = [f'x86_unimpl(c, {u32(insn.address)}, "{text}"); /* {e} */']
                out.append(f"    /* {insn.address:08X} {text} */")
                out.extend("    " + line for line in lines)
            if kind == "noreturn":
                out.append("    return;")
            elif kind in ("fall", "jcc") or (last is not None and kind not in ("ret", "jmp")):
                out.append(f"    goto L_{end:08X};")
        out.append("}")
        return "\n".join(out)

    def goto(self, target, labels, src):
        if target in self.hle:  # e.g. a `jmp SwitchToFiber` tail call that discovery merged into this function
            return [f"f_{target:08X}(c); return;"]
        if target in labels:
            # a backward branch closes a loop: let a thread waiting for the guest lock in (see SAFEPOINT)
            return [f"SAFEPOINT(); goto L_{target:08X};" if target <= src else f"goto L_{target:08X};"]
        if target in self.funcs:  # tail call: callee's ret pops our caller's return address
            return [f"f_{target:08X}(c); return;"]
        return [f"x86_call_indirect(c, {u32(target)}); return;"]

    # ---------- instructions ----------
    def insn(self, insn, labels, func):
        parts = insn.mnemonic.split()
        m = parts[-1]
        prefixes = set(parts[:-1])
        ops = insn.operands
        nxt = insn.address + insn.size
        n = ops[0].size * 8 if ops else 32
        R = lambda i, size=None: self.read(insn, ops[i], size)
        W = lambda i, v: self.write(insn, ops[i], v)

        if m in ("nop", "wait", "fwait", "fnclex", "prefetchnta", "prefetcht0", "sfence", "cli", "sti", "wbinvd", "invd",
                 "emms"):
            return []
        if m == "mov":
            return [W(0, R(1))]
        if m in ("movzx", "movsx"):
            v = R(1) if m == "movzx" else f"(uint32_t)(int32_t)(int{ops[1].size * 8}_t)({R(1)})"
            return [W(0, v)]
        if m == "lea":
            return [W(0, self.addr(insn, ops[1]))]
        if m == "push":
            if ops[0].size != 4:
                raise Unsupported("push16")
            return [f"PUSH32({R(0)});"]
        if m == "pop":
            return ["{ uint32_t t = M32(c->esp); c->esp += 4;", "  " + W(0, "t") + " }"]
        if m == "pushal":
            return ["{ uint32_t s = c->esp; PUSH32(c->eax); PUSH32(c->ecx); PUSH32(c->edx); PUSH32(c->ebx);",
                    "  PUSH32(s); PUSH32(c->ebp); PUSH32(c->esi); PUSH32(c->edi); }"]
        if m == "popal":
            return ["c->edi = M32(c->esp); c->esi = M32(c->esp + 4); c->ebp = M32(c->esp + 8);",
                    "c->ebx = M32(c->esp + 16); c->edx = M32(c->esp + 20); c->ecx = M32(c->esp + 24);",
                    "c->eax = M32(c->esp + 28); c->esp += 32;"]
        if m == "pushfd":
            return ["PUSH32(fl_pack(&f, c));"]
        if m == "popfd":
            return ["fl_unpack(&f, c, M32(c->esp)); c->esp += 4;"]
        if m == "lahf":
            return ["c->eax = (c->eax & 0xFFFF00FFu) | ((fl_pack(&f, c) & 0xFF) << 8);"]
        if m == "sahf":
            return ["{ uint32_t t = (fl_pack(&f, c) & ~0xFFu) | ((c->eax >> 8) & 0xFF); fl_unpack(&f, c, t); }"]
        if m == "leave":
            return ["c->esp = c->ebp; c->ebp = M32(c->esp); c->esp += 4;"]
        if m in ("add", "adc", "sub", "sbb", "cmp"):
            carry = "f.cf" if m in ("adc", "sbb") else "0"
            op = "+" if m in ("add", "adc") else "-"
            fl = "fl_add" if op == "+" else "fl_sub"
            lines = [f"{{ uint32_t a = {R(0)}, b = {R(1)}, ci = {carry}, r = a {op} b {op} ci;",
                     f"  {fl}(&f, a, b, ci, r, {n});"]
            if m != "cmp":
                lines.append("  " + W(0, "r"))
            return lines + ["}"]
        if m in ("and", "or", "xor", "test"):
            op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
            lines = [f"{{ uint32_t r = {R(0)} {op} {R(1)}; fl_logic(&f, r, {n});"]
            if m != "test":
                lines.append("  " + W(0, "r"))
            return lines + ["}"]
        if m in ("inc", "dec"):
            op = "+" if m == "inc" else "-"
            return [f"{{ uint32_t a = {R(0)}, r = a {op} 1; fl_{m}(&f, a, r, {n}); {W(0, 'r')} }}"]
        if m == "neg":
            return [f"{{ uint32_t a = {R(0)}, r = 0u - a; fl_sub(&f, 0, a, 0, r, {n}); {W(0, 'r')} }}"]
        if m == "not":
            return [W(0, f"~{R(0)}")]
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            fn = {"sal": "shl"}.get(m, m)
            cnt = R(1) if len(ops) > 1 else "1"
            return [f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t r = op_{fn}(&f, {R(0)}, n, {n}); {W(0, 'r')} }} }}"]
        if m in ("shld", "shrd"):
            if n != 32:
                raise Unsupported("16-bit shld/shrd")
            return [f"{{ uint32_t n = ({R(2)}) & 31; if (n) {{ uint32_t r = op_{m}(&f, {R(0)}, {R(1)}, n); {W(0, 'r')} }} }}"]
        if m == "imul":
            if len(ops) == 1:
                if n != 32:
                    raise Unsupported("8/16-bit one-operand imul")
                return [f"op_imul32(c, &f, {R(0)});"]
            a, b = (R(0), R(1)) if len(ops) == 2 else (R(1), R(2))
            return [f"{{ uint32_t r = op_imul2(&f, {a}, {b}, {n}); {W(0, 'r')} }}"]
        if m == "mul":
            if n != 32:
                raise Unsupported("8/16-bit mul")
            return [f"op_mul32(c, &f, {R(0)});"]
        if m in ("div", "idiv"):
            if n == 32:
                return [f"op_{m}32(c, {R(0)}, {u32(insn.address)});"]
            if n == 8 and m == "div":
                return [f"op_div8(c, {R(0)}, {u32(insn.address)});"]
            raise Unsupported(f"{n}-bit {m}")
        if m == "cdq":
            return ["c->edx = (uint32_t)((int32_t)c->eax >> 31);"]
        if m == "xlatb":  # al = [ebx + al]
            return ["c->eax = (c->eax & 0xFFFFFF00u) | M8(c->ebx + (c->eax & 0xFF));"]
        if m == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)c->eax;"]
        if m == "cbw":
            return ["c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)c->eax;"]
        if m == "xchg":
            return [f"{{ uint32_t t = {R(0)}; {W(0, R(1))} {W(1, 't')} }}"]
        if m == "xadd":
            return [f"{{ uint32_t a = {R(0)}, b = {R(1)}, r = a + b; fl_add(&f, a, b, 0, r, {n}); {W(1, 'a')} {W(0, 'r')} }}"]
        if m == "cmpxchg":
            acc = self.reg_read({32: "eax", 16: "ax", 8: "al"}[n])
            acc_w = self.reg_write({32: "eax", 16: "ax", 8: "al"}[n], "d")
            return [f"{{ uint32_t a = {acc}, d = {R(0)}; fl_sub(&f, a, d, 0, a - d, {n});",
                    f"  if (f.zf) {{ {W(0, R(1))} }} else {{ {acc_w} }} }}"]
        if m == "bswap":
            return [W(0, f"_byteswap_ulong({R(0)})")]
        if m in ("bsf", "bsr"):
            fn = "_BitScanForward" if m == "bsf" else "_BitScanReverse"
            return [f"{{ unsigned long i; uint32_t s = {R(1)}; f.zf = s == 0; if (s) {{ {fn}(&i, s); {W(0, 'i')} }} }}"]
        if m == "clc":
            return ["f.cf = 0;"]
        if m == "stc":
            return ["f.cf = 1;"]
        if m == "cmc":
            return ["f.cf ^= 1;"]
        if m == "cld":
            return ["c->df = 0;"]
        if m == "std":
            return ["c->df = 1;"]
        if m in ("in", "out"):  # port I/O: southbridge/SMBus/PCI config, handled by the runtime
            port_op, val_op = (ops[0], ops[1]) if m == "out" else (ops[1], ops[0])
            port = R(ops.index(port_op), 2) if port_op.type == x86.X86_OP_REG else u32(port_op.imm & 0xFFFF)
            n = val_op.size * 8
            if m == "out":
                return [f"x86_out(c, {port}, {R(ops.index(val_op))}, {n});"]
            return [W(ops.index(val_op), f"x86_in(c, {port}, {n})")]
        if m == "rdtsc":
            return ["{ uint64_t t = x86_rdtsc(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }"]
        if m == "cpuid":
            return ["x86_cpuid(c);"]
        if m.startswith("set") and m[3:] in CC:
            return [W(0, f"CC_{CC[m[3:]]}")]
        if m.startswith("cmov") and m[4:] in CC:
            return [f"if (CC_{CC[m[4:]]}) {{ {W(0, R(1))} }}"]

        # strings
        for base, fn in (("movs", "movs"), ("stos", "stos"), ("lods", "lods"), ("cmps", "cmps"), ("scas", "scas")):
            if m[:-1] == base and m[-1] in "bwd":
                sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
                rep = 1 if prefixes & {"rep", "repe"} else 2 if "repne" in prefixes else 0
                if fn in ("movs", "stos"):
                    return [f"str_{fn}(c, {sz}, {int(rep != 0)});"]
                if fn == "lods":
                    if rep:
                        raise Unsupported("rep lods")
                    return [f"str_lods(c, {sz});"]
                return [f"str_cmps(c, &f, {sz}, {rep}, {int(fn == 'scas')});"]

        # control flow
        if m == "call":
            op = ops[0]
            if op.type == x86.X86_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                call = f"f_{t:08X}(c);" if t in self.funcs else f"x86_call_indirect(c, {u32(t)});"
                return [f"PUSH32({u32(nxt)}); {call}"]
            # read the target before pushing: it may be addressed relative to esp
            return [f"{{ uint32_t t = {R(0)}; PUSH32({u32(nxt)}); x86_call_indirect(c, t); }}"]
        if m == "ret":
            pop = 4 + (ops[0].imm if ops else 0)
            return [f"c->esp += {pop}; return;"]
        if m == "jmp":
            op = ops[0]
            if op.type == x86.X86_OP_IMM:
                return self.goto(op.imm & 0xFFFFFFFF, labels, insn.address)
            table = next((t for t in func["jump_tables"] if t["at"] == insn.address), None)
            lines = [f"{{ uint32_t t = {R(0)};"]
            if table:
                cases = " ".join(f"case {u32(t)}: goto L_{t:08X};" for t in sorted(set(table["targets"]))
                                 if t in labels)
                lines.append(f"  switch (t) {{ {cases} }}")
            return lines + ["  x86_call_indirect(c, t); return; }"]
        if m.startswith("j") and m[1:] in CC:
            return [f"if (CC_{CC[m[1:]]}) {{ {' '.join(self.goto(ops[0].imm & 0xFFFFFFFF, labels, insn.address))} }}"]
        if m == "jecxz":
            return [f"if (!c->ecx) {{ {' '.join(self.goto(ops[0].imm & 0xFFFFFFFF, labels, insn.address))} }}"]
        if m in ("loop", "loope", "loopne"):
            cond = {"loop": "c->ecx", "loope": "c->ecx && f.zf", "loopne": "c->ecx && !f.zf"}[m]
            return [f"c->ecx--; if ({cond}) {{ {' '.join(self.goto(ops[0].imm & 0xFFFFFFFF, labels, insn.address))} }}"]
        if m in ("int3", "ud2", "hlt"):
            return [f'x86_fault(c, {u32(insn.address)}, "{m}"); return;']

        if m.startswith("f"):
            return self.x87(insn, m, ops, R, W)
        if any(o.type == x86.X86_OP_REG and insn.reg_name(o.reg).startswith("mm") for o in ops):
            return self.mmx(insn, m, ops, R, W)
        return self.sse(insn, m, ops, R, W)

    # ---------- MMX (lane ops in x86.h) ----------
    MMX_OPS = {m: "MX_" + m.upper() for m in (
        "paddb paddw paddd paddq psubb psubw psubd psubq paddsb paddsw paddusb paddusw psubsb psubsw psubusb "
        "psubusw pmullw pmulhw pmulhuw pmaddwd pand pandn por pxor pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd "
        "punpcklbw punpcklwd punpckldq punpckhbw punpckhwd punpckhdq packsswb packssdw packuswb pavgb pavgw pminub "
        "pmaxub pminsw pmaxsw psadbw psllw pslld psllq psrlw psrld psrlq psraw psrad").split()}

    def mmx(self, insn, m, ops, R, W):
        def is_mm(i):
            return ops[i].type == x86.X86_OP_REG and insn.reg_name(ops[i].reg).startswith("mm")

        def mm(i):
            return f"c->mm[{insn.reg_name(ops[i].reg)[2:]}]"

        def q(i):  # 64-bit source: mm register, memory, or (shift counts) an immediate
            if is_mm(i):
                return mm(i)
            if ops[i].type == x86.X86_OP_IMM:
                return f"{ops[i].imm & 0xFF}ull"
            if ops[i].type == x86.X86_OP_MEM:
                return f"M64({self.addr(insn, ops[i])})"
            raise Unsupported("mmx operand")

        if m in self.MMX_OPS:
            return [f"{mm(0)} = mmx({self.MMX_OPS[m]}, {mm(0)}, {q(1)});"]
        if m in ("movq", "movntq"):
            if is_mm(0):
                return [f"{mm(0)} = {q(1)};"]
            return [f"M64({self.addr(insn, ops[0])}) = {mm(1)};"]
        if m == "movd":
            if is_mm(0):
                return [f"{mm(0)} = (uint32_t)({R(1)});"]
            return [W(0, f"(uint32_t){mm(1)}")]
        if m == "pshufw":
            return [f"{mm(0)} = mmx_pshufw({q(1)}, {ops[2].imm & 0xFF});"]
        if m == "pmovmskb":
            return [W(0, f"mmx_pmovmskb({mm(1)})")]
        if m == "pextrw":
            return [W(0, f"(uint32_t)(({mm(1)} >> {16 * (ops[2].imm & 3)}) & 0xFFFF)")]
        if m == "pinsrw":
            sh = 16 * (ops[2].imm & 3)
            return [f"{mm(0)} = ({mm(0)} & ~(0xFFFFull << {sh})) | ((uint64_t)(uint16_t)({R(1)}) << {sh});"]
        if m == "cvtpi2ps":
            x = f"c->xmm[{insn.reg_name(ops[0].reg)[3:]}]"
            return [f"{{ uint64_t v = {q(1)}; {x}.f[0] = (float)(int32_t)v; {x}.f[1] = (float)(int32_t)(v >> 32); }}"]
        if m in ("cvtps2pi", "cvttps2pi"):
            fn = "sse_cvtt" if m == "cvttps2pi" else "sse_cvt"
            src = (f"c->xmm[{insn.reg_name(ops[1].reg)[3:]}]" if ops[1].type == x86.X86_OP_REG
                   else f"MX({self.addr(insn, ops[1])})")
            return [f"{{ Xmm s = {src}; {mm(0)} = (uint32_t){fn}(s.f[0]) | (uint64_t)(uint32_t){fn}(s.f[1]) << 32; }}"]
        raise Unsupported("mmx")

    # ---------- x87 ----------
    def fmem(self, insn, op):
        a = self.addr(insn, op)
        return {4: f"(double)MF32({a})", 8: f"MF64({a})", 10: f"x86_ld80({a})"}[op.size]

    def x87(self, insn, m, ops, R, W):
        regs = [self.sti(insn, o) if o.type == x86.X86_OP_REG else None for o in ops]
        consts = {"fldz": "0.0", "fld1": "1.0", "fldpi": "3.14159265358979323846", "fldl2e": "1.44269504088896340736",
                  "fldln2": "0.693147180559945309417", "fldlg2": "0.301029995663981195214",
                  "fldl2t": "3.32192809488736234787"}
        if m in consts:
            return [f"fpush(c, {consts[m]});"]
        if m == "fld":
            if regs[0] is not None:
                return [f"{{ double t = ST({regs[0]}); fpush(c, t); }}"]
            return [f"fpush(c, {self.fmem(insn, ops[0])});"]
        if m == "fild":
            t = {2: "int16_t", 4: "int32_t", 8: "int64_t"}[ops[0].size]
            return [f"fpush(c, (double)*({t} *)(g_mem + {self.addr(insn, ops[0])}));"]
        if m in ("fst", "fstp"):
            pop = " fpop(c);" if m == "fstp" else ""
            if regs[0] is not None:
                return [f"ST({regs[0]}) = ST(0);{pop}"]
            a = self.addr(insn, ops[0])
            st = {4: f"MF32({a}) = (float)ST(0);", 8: f"MF64({a}) = ST(0);", 10: f"x86_st80({a}, ST(0));"}
            return [st[ops[0].size] + pop]
        if m in ("fist", "fistp"):
            t = {2: "int16_t", 4: "int32_t", 8: "int64_t"}[ops[0].size]
            pop = " fpop(c);" if m == "fistp" else ""
            return [f"*({t} *)(g_mem + {self.addr(insn, ops[0])}) = ({t})fround(c, ST(0));{pop}"]
        if m == "fxch":
            i = regs[-1] if regs else 1
            return [f"{{ double t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}"]
        arith = {"fadd": "{d} + {s}", "fsub": "{d} - {s}", "fsubr": "{s} - {d}", "fmul": "{d} * {s}",
                 "fdiv": "{d} / {s}", "fdivr": "{s} / {d}"}
        base = m[:-1] if m.endswith("p") and m[:-1] in arith else m
        if base in arith:
            pop = m != base
            if ops and ops[0].type == x86.X86_OP_MEM:
                d, s = "ST(0)", self.fmem(insn, ops[0])
            elif len(regs) == 2:
                d, s = f"ST({regs[0]})", f"ST({regs[1]})"
            elif pop:
                d, s = f"ST({regs[0] if regs else 1})", "ST(0)"
            else:
                d, s = "ST(0)", f"ST({regs[0]})"
            return [f"{d} = {arith[base].format(d=d, s=s)};" + (" fpop(c);" if pop else "")]
        iarith = {"fiadd": "+", "fisub": "-", "fimul": "*", "fidiv": "/"}
        if m in iarith:
            t = {2: "int16_t", 4: "int32_t"}[ops[0].size]
            return [f"ST(0) = ST(0) {iarith[m]} (double)*({t} *)(g_mem + {self.addr(insn, ops[0])});"]
        unary = {"fchs": "-ST(0)", "fabs": "fabs(ST(0))", "fsqrt": "sqrt(ST(0))", "fsin": "sin(ST(0))",
                 "fcos": "cos(ST(0))", "frndint": "fround(c, ST(0))", "f2xm1": "exp2(ST(0)) - 1.0"}
        if m in unary:
            return [f"ST(0) = {unary[m]};"]
        if m == "fsincos":
            return ["{ double t = ST(0); ST(0) = sin(t); fpush(c, cos(t)); }"]
        if m == "fptan":
            return ["ST(0) = tan(ST(0)); fpush(c, 1.0);"]
        if m == "fpatan":
            return ["ST(1) = atan2(ST(1), ST(0)); fpop(c);"]
        if m == "fyl2x":
            return ["ST(1) = ST(1) * log2(ST(0)); fpop(c);"]
        if m == "fscale":
            return ["ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"]
        if m == "fprem":
            return ["ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x0400;"]
        if m == "fprem1":  # IEEE remainder (quotient rounded to nearest)
            return ["ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x0400;"]
        if m == "fyl2xp1":
            return ["ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c);"]
        if m == "fxtract":  # ST(0) = significand in [1, 2), ST(1) = unbiased exponent
            return ["{ int e; double s = frexp(ST(0), &e); ST(0) = (double)(e - 1); fpush(c, s * 2); }"]
        if m in ("fcom", "fcomp", "fucom", "fucomp", "fcompp", "fucompp"):
            if ops and ops[0].type == x86.X86_OP_MEM:
                s = self.fmem(insn, ops[0])
            else:
                s = f"ST({regs[0] if regs else 1})"
            pops = 2 if m.endswith("pp") else 1 if m.endswith("p") else 0
            return [f"fcom(c, ST(0), {s});" + " fpop(c);" * pops]
        if m == "ficomp":
            t = {2: "int16_t", 4: "int32_t"}[ops[0].size]
            return [f"fcom(c, ST(0), (double)*({t} *)(g_mem + {self.addr(insn, ops[0])})); fpop(c);"]
        if m == "ftst":
            return ["fcom(c, ST(0), 0.0);"]
        if m == "fxam":
            return ["fxam(c, ST(0));"]
        if m in ("fcomi", "fcompi", "fucomi", "fucompi"):
            pop = " fpop(c);" if "p" in m[4:] else ""
            return [f"fcomi(&f, ST(0), ST({regs[-1]}));{pop}"]
        if m in ("fnstsw", "fstsw"):
            return [W(0, "fstsw(c)")]
        if m in ("fnstcw", "fstcw"):
            return [W(0, "c->fcw")]
        if m == "fldcw":
            return [f"c->fcw = {R(0)};"]
        if m in ("fninit", "finit"):
            return ["c->top = 0; c->fsw = 0; c->fcw = 0x037F;"]
        if m in ("fcmovb", "fcmove", "fcmovbe", "fcmovu", "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"):
            cond = {"b": "CC_B", "e": "CC_E", "be": "CC_BE", "u": "CC_P", "nb": "CC_AE", "ne": "CC_NE",
                    "nbe": "CC_A", "nu": "CC_NP"}[m[5:]]
            return [f"if ({cond}) ST(0) = ST({regs[-1]});"]
        raise Unsupported("x87")

    # ---------- SSE ----------
    def sse(self, insn, m, ops, R, W):
        def x(i):
            return self.xmm(insn, ops[i])

        def is_x(i):
            return ops[i].type == x86.X86_OP_REG and insn.reg_name(ops[i].reg).startswith("xmm")

        def src_f(i):  # scalar float source
            return f"{x(i)}.f[0]" if is_x(i) else f"MF32({self.addr(insn, ops[i])})"

        def src_x(i):  # 128-bit source
            return x(i) if is_x(i) else f"MX({self.addr(insn, ops[i])})"

        if m == "movss":
            if is_x(0) and is_x(1):
                return [f"{x(0)}.f[0] = {x(1)}.f[0];"]
            if is_x(0):
                return [f"{{ float t = {src_f(1)}; {x(0)} = (Xmm){{0}}; {x(0)}.f[0] = t; }}"]
            return [f"MF32({self.addr(insn, ops[0])}) = {x(1)}.f[0];"]
        if m in ("movaps", "movups", "movntps"):  # movntps: non-temporal hint only
            if is_x(0):
                return [f"{x(0)} = {src_x(1)};"]
            return [f"MX({self.addr(insn, ops[0])}) = {x(1)};"]
        if m in ("movlps", "movhps"):
            q = 0 if m == "movlps" else 1
            if is_x(0):
                return [f"{x(0)}.q[{q}] = M64({self.addr(insn, ops[1])});"]
            return [f"M64({self.addr(insn, ops[0])}) = {x(1)}.q[{q}];"]
        if m == "movhlps":
            return [f"{x(0)}.q[0] = {x(1)}.q[1];"]
        if m == "movlhps":
            return [f"{x(0)}.q[1] = {x(1)}.q[0];"]
        scalar = {"addss": "{d} + {s}", "subss": "{d} - {s}", "mulss": "{d} * {s}", "divss": "{d} / {s}",
                  "minss": "({d} < {s} ? {d} : {s})", "maxss": "({d} > {s} ? {d} : {s})",
                  "sqrtss": "sqrtf({s})", "rsqrtss": "1.0f / sqrtf({s})", "rcpss": "1.0f / {s}"}
        if m in scalar:
            return [f"{x(0)}.f[0] = {scalar[m].format(d=f'{x(0)}.f[0]', s=src_f(1))};"]
        # one host SSE op each, the same lane math: a loop over the 4 lanes per instruction made cl need 65 GB for the
        # long SSE blocks in funcs_039.c (GitHub #2, #3, #8, #16). rsqrtps/rcpps stay exact, as before (the host's
        # approximations differ between CPUs)
        packed = {"addps": "_mm_add_ps(d, s)", "subps": "_mm_sub_ps(d, s)", "mulps": "_mm_mul_ps(d, s)",
                  "divps": "_mm_div_ps(d, s)", "minps": "_mm_min_ps(d, s)", "maxps": "_mm_max_ps(d, s)",
                  "sqrtps": "_mm_sqrt_ps(s)", "rsqrtps": "_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(s))",
                  "rcpps": "_mm_div_ps(_mm_set1_ps(1.0f), s)", "andps": "_mm_and_ps(d, s)", "orps": "_mm_or_ps(d, s)",
                  "xorps": "_mm_xor_ps(d, s)", "andnps": "_mm_andnot_ps(d, s)"}
        if m in packed:
            return [f"{{ __m128 s = XL({src_x(1)}), d = XL({x(0)}); XS({x(0)}, {packed[m]}); }}"]
        if m == "shufps":
            return [f"{x(0)} = sse_shufps({x(0)}, {src_x(1)}, {ops[2].imm & 0xFF});"]
        if m in ("unpcklps", "unpckhps"):
            h = 0 if m == "unpcklps" else 2
            return [f"{{ Xmm s = {src_x(1)}, d = {x(0)}, r; r.u[0] = d.u[{h}]; r.u[1] = s.u[{h}];",
                    f"  r.u[2] = d.u[{h + 1}]; r.u[3] = s.u[{h + 1}]; {x(0)} = r; }}"]
        if m in ("comiss", "ucomiss"):
            return [f"sse_comi(&f, {x(0)}.f[0], {src_f(1)});"]
        if m == "cvtsi2ss":
            return [f"{x(0)}.f[0] = (float)(int32_t){R(1)};"]
        if m in ("cvttss2si", "cvtss2si"):
            fn = "sse_cvtt" if m == "cvttss2si" else "sse_cvt"
            return [W(0, f"(uint32_t){fn}({src_f(1)})")]
        if m == "ldmxcsr":
            return [f"c->mxcsr = {R(0)};"]
        if m == "stmxcsr":
            return [W(0, "c->mxcsr")]
        raise Unsupported("sse/mmx/system")


def write_if_changed(path, text):
    """Leave unchanged files alone so the incremental build only recompiles what moved."""
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, "w") as f:
        f.write(text)


def load_hle(path, symbols_json):
    """HLE list: one function per line, `Name` (looked up in the symbol scan) or `0xVA Name`; # comments."""
    names = {s["name"]: s["va"] for s in json.load(open(symbols_json))["symbols"]} if symbols_json else {}
    hle = {}
    for line in open(path):
        line = line.split("#")[0].split()
        if not line:
            continue
        if line[0].startswith("0x"):
            hle[int(line[0], 16)] = line[1]
        elif line[0] in names:
            hle[names[line[0]]] = line[0]
        else:
            sys.exit(f"HLE list: {line[0]} was not found by the symbol scan; give its address as `0xVA {line[0]}`")
    return hle


def main():
    args = sys.argv[1:]
    opts = {}
    for flag in ("--only", "--hle", "--symbols", "--trace"):
        if flag in args:
            i = args.index(flag)
            opts[flag] = args[i + 1]
            del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    xbe, funcs_json, out_dir = args
    only = {int(v, 16) for v in opts["--only"].split(",")} if "--only" in opts else None
    hle = load_hle(opts["--hle"], opts.get("--symbols")) if "--hle" in opts else {}
    img = Image(xbe)
    funcs = json.load(open(funcs_json))
    entries = {f["entry"] for f in funcs}
    # a replaced function needs an entry even if discovery never reached it (e.g. only called via the API)
    funcs += [{"entry": va, "blocks": [], "jump_tables": []} for va in sorted(hle) if va not in entries]
    funcs.sort(key=lambda f: f["entry"])
    trace = set()
    if "--trace" in opts and os.path.exists(opts["--trace"]):  # one hex VA per line, '#' comments
        trace = {int(l.split("#")[0], 16) for l in open(opts["--trace"]) if l.split("#")[0].strip()}
    lifter = Lifter(img, funcs, hle, trace)
    todo = [f for f in funcs if only is None or f["entry"] in only]
    os.makedirs(out_dir, exist_ok=True)

    write_if_changed(os.path.join(out_dir, "funcs.h"),
                     '#pragma once\n#include "x86.h"\n'
                     + "".join(f"void f_{f['entry']:08X}(X86Ctx *c);\n" for f in funcs))

    nfiles = 0
    for i in range(0, len(todo), FUNCS_PER_FILE):
        parts = ['#include <intrin.h>\n#include "funcs.h"\n\n']
        for f in todo[i:i + FUNCS_PER_FILE]:
            if f["entry"] in hle:  # replaced by a host implementation in runtime/
                name, va = hle[f["entry"]], f["entry"]
                parts.append(f"RT_API void hle_{name}(X86Ctx *c);\nvoid f_{va:08X}(X86Ctx *c) {{ "
                             f"c->last_fn = 0x{va:08X}u; hle_{name}(c); }}\n\n")
                if f["blocks"]:  # keep the original as o_XXXXXXXX so a host version can wrap or defer to it
                    parts.append(lifter.lift_function(f).replace(f"void f_{va:08X}(", f"GAME_API void o_{va:08X}(", 1)
                                 + "\n\n")
            else:
                parts.append(lifter.lift_function(f) + "\n\n")
        write_if_changed(os.path.join(out_dir, f"funcs_{nfiles:03d}.c"), "".join(parts))
        nfiles += 1

    write_if_changed(os.path.join(out_dir, "func_table.c"),
                     '#include "funcs.h"\n\nGAME_API uint8_t *g_mem; /* the runtime\'s, set by game_load */\n\n'
                     'GAME_API const FuncEntry g_func_table[] = {\n'
                     + "".join(f"    {{0x{f['entry']:08X}u, f_{f['entry']:08X}}},\n" for f in todo)
                     + f"}};\nGAME_API const uint32_t g_func_count = {len(todo)};\n")
    if hle:
        print(f"{len(hle)} functions replaced by host implementations")

    total = lifter.lifted + sum(lifter.unimpl.values())
    print(f"lifted {len(todo)} functions into {nfiles} files; {lifter.lifted:,}/{total:,} instructions "
          f"({100 * lifter.lifted / max(total, 1):.2f}%) translated")
    print("top unimplemented:", lifter.unimpl.most_common(30))


if __name__ == "__main__":
    main()
