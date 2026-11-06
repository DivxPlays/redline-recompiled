"""Load an XBE's sections into a VA-addressable image."""
import bisect

from xbe_info import parse

# Sections holding x86 code. Every XBE section carries the X flag, so we go by name;
# DOLBY is DSP microcode and the rest are data.
CODE_SECTIONS = {
    ".text", "FZGINIT", "FZFULLUI", "D3D", "D3DX", "XGRPH", "DSOUND", "WMADEC", "XACTENG",
    "XONLINE", "XNET", "XPP", "BINK", "BINK32", "BINK32A", "BINK16", "BINK4444", "BINK5551",
    "BINKYUY2", "BINK16MX", "BINK16X2", "BINK16M", "BINK32MX", "BINK32X2", "BINK32M",
}


class Image:
    def __init__(self, path):
        self.info = parse(path)
        raw = open(path, "rb").read()
        self.sections = sorted(self.info["sections"], key=lambda s: s["va"])
        self.starts = [s["va"] for s in self.sections]
        for s in self.sections:
            # bss tail (vsize > rsize) reads as zero
            s["data"] = raw[s["raw"]:s["raw"] + s["rsize"]] + bytes(max(0, s["vsize"] - s["rsize"]))
        self.kernel_thunk = self.info["kernel_thunk"]
        self.kernel_count = len(self.info["kernel_imports"])

    def section(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0:
            s = self.sections[i]
            if va < s["va"] + s["vsize"]:
                return s
        return None

    def is_code(self, va):
        s = self.section(va)
        return s is not None and s["name"] in CODE_SECTIONS

    def read(self, va, n):
        s = self.section(va)
        if s is None:
            return None
        off = va - s["va"]
        return s["data"][off:off + n]

    def u32(self, va):
        b = self.read(va, 4)
        return int.from_bytes(b, "little") if b and len(b) == 4 else None

    def kernel_ordinal(self, slot_va):
        """Ordinal for a kernel thunk slot address, else None."""
        idx, rem = divmod(slot_va - self.kernel_thunk, 4)
        if rem == 0 and 0 <= idx < self.kernel_count:
            return self.info["kernel_imports"][idx]
        return None
