#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
aotlib.py -- ELF64/AArch64 reading and arm64 rewriting, shared by the ahead-of-time
tools.

`aot-inventory.py` counts what would have to be rewritten; `aot-convert.py` does the
rewriting. Both need the same three things: the ELF headers, the code ranges (which is
the interesting part -- these libraries do not mark their bulk code executable, so the
rule is "an executable segment's bytes that no section claims are code too"), and arm64
instruction encode/decode for the handful of forms that must change.
"""

import bisect
import struct

PT_LOAD, PT_DYNAMIC = 1, 2
SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4
PF_X, PF_W = 1, 2

DT_RELA, DT_RELASZ, DT_RELAENT, DT_TEXTREL = 7, 8, 9, 16
DT_JMPREL, DT_PLTRELSZ, DT_PLTREL = 23, 2, 20
DT_INIT_ARRAY, DT_INIT_ARRAYSZ = 25, 27
DT_NEEDED, DT_STRTAB, DT_SYMTAB, DT_STRSZ = 1, 5, 6, 10
DT_SONAME, DT_INIT, DT_INIT_ARRAY = 14, 12, 25

RELOC_NAMES = {
    1027: "RELATIVE", 257: "ABS64", 1025: "GLOB_DAT", 1026: "JUMP_SLOT",
    275: "ADR_PREL_PG_HI21", 277: "LD_PREL_LO19", 278: "ADD_ABS_LO12_NC",
    282: "JUMP26", 283: "CALL26", 1024: "COPY", 1031: "TLSDESC",
}

# relocation types whose slot holds an address we have to emit as an assembler
# expression so the linker produces the rebase for us
REBASE_TYPES = {1027, 257}
# R_AARCH64_ABS64: a 64-bit pointer to a symbol. In the data segment the word itself is the
# reference; in the code segment it is an adrp/add pair the converter rewrites instead.
ABS64 = 257
# relocation types whose slot is a GOT entry bound to an imported symbol
BIND_TYPES = {1025, 1026}


def sdk_for_target(target):
    """Which SDK a clang target triple wants, or None for macOS (where the default is
    already right). A device build has to say so: `--target arm64-apple-ios15.5` alone
    still links against the macOS SDK.

    This is here rather than in one of the tools because both of them ask the linker the
    same question -- "which of these names does the platform provide?" -- and they have
    to be asked against the same libraries, or they answer differently.
    """
    for platform in ("ios", "watchos", "tvos", "xros", "macosx", "macos"):
        if platform in target:
            if platform == "macosx":
                return "macosx"
            if platform == "macos":
                return None                     # 'arm64-apple-macos': the host SDK
            return "iphoneos" if platform == "ios" else platform
    return None


def _uleb(b, p):
    """(value, next offset)"""
    v, shift = 0, 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << shift
        if not x & 0x80:
            return v, p
        shift += 7


def _uleb_at(b, p):
    return _uleb(b, p)


def _sleb(b, p):
    v, shift = 0, 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            if x & 0x40:
                v -= 1 << shift
            return v, p


def _enc_len(enc):
    fmt = enc & 0x0F
    return {0x00: 8, 0x01: -1, 0x02: 2, 0x03: 4, 0x04: 8, 0x09: -1, 0x0A: 2, 0x0B: 4,
            0x0C: 8}.get(fmt, 4)


def _decode_encoded(b, p, enc, pc):
    fmt = enc & 0x0F
    app = enc & 0x70
    n = _enc_len(enc)
    if n < 0:
        v, p = (_uleb(b, p) if fmt == 0x01 else _sleb(b, p))
    else:
        raw = b[p:p + n]
        v = int.from_bytes(raw, "little", signed=(fmt in (0x08, 0x0A, 0x0B, 0x0C)))
    if app == 0x10:
        v += pc + p
    elif app == 0x30:
        v += pc
    return v

PAGE = 0x1000
XMAGIC = 0xFEEDFACF


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def sext(v, bits):
    """Sign-extend a `bits`-wide field to a Python int.

    The C idiom `(v << (64 - bits)) >> (64 - bits)` does not survive the port: Python's
    integers are unbounded and its right shift is arithmetic only for a negative left
    operand, so on a positive `v` it is a logical shift and the function was the
    identity. Every immediate that has its top bit set -- a backward branch, a negative
    ADRP offset -- came out positive and hundreds of megabytes out.
    """
    v &= (1 << bits) - 1
    return v - (1 << bits) if (v >> (bits - 1)) & 1 else v


# ---------------------------------------------------------------- arm64 encodings
#
# The PC-relative forms are the whole game here: a guest library computes most of its
# addresses relative to its own position, which is why its code can be moved into a
# Mach-O without touching a single byte. Getting an immediate wrong by a factor of four
# looks like a crash far away from the instruction, so these are checked against the
# assembler and against real hardware (see aot-roundtrip.py).

def is_adrp(insn):
    return (insn & 0x9F000000) == 0x90000000


def is_adr(insn):
    return (insn & 0x9F000000) == 0x10000000


def is_ldr_literal(insn):
    return (insn & 0xFF000000) == 0x58000000


def is_ldr_literal_any(insn):
    """LDR W, LDR X or LDRSW from a literal; not PRFM and not a SIMD load."""
    return ((insn & 0x3B000000) == 0x18000000
            and not ((insn >> 26) & 1) and ((insn >> 30) & 3) in (0, 1, 2))


def is_branch(insn):
    return (insn & 0x7C000000) == 0x14000000


def is_tpidr_read(insn):
    return (insn & 0xFFFFFFE0) == 0xD53BD040


def is_tpidr_write(insn):
    return (insn & 0xFFFFFFE0) == 0xD51BD040


def is_ctr_read(insn):
    return (insn & 0xFFFFFFE0) == 0xD53B0020


def is_svc(insn):
    return (insn & 0xFFE0001F) == 0xD4000001


def adrp_imm(insn):
    """ADRP: immlo is bits 30:29 and immhi is bits 23:5, imm = immhi:immlo << 12 pages."""
    imm = (((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3)
    return sext(imm, 21)


def adr_imm(insn):
    """ADR: immhi is bits 23:5, immlo is bits 30:29; the result is a byte offset."""
    return sext(((((insn >> 5) & 0xFFFF) << 2) | ((insn >> 29) & 3)), 21)


def adrp_target(pc, insn):
    """The page an `adrp` names."""
    return (pc & ~0xFFF) + (adrp_imm(insn) << 12)


def adr_target(pc, insn):
    return pc + adr_imm(insn)


def ldr_literal_target(pc, insn):
    return pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2)


def encode_adrp(rt, from_pc, to_pc):
    """`adrp Xt, <to_page>` written at from_pc. The immediate is a page delta relative to
    the instruction's own page, which is what makes it survive a slide unchanged -- as
    long as the image base itself is page-aligned (the converter checks that)."""
    delta = ((to_pc & ~0xFFF) - (from_pc & ~0xFFF)) >> 12
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError(f"adrp target {to_pc:#x} is out of range from {from_pc:#x}")
    return 0x90000000 | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5) | (rt & 0x1F)


def encode_adr(rt, from_pc, to_pc):
    """`adr Xt, <to>` written at from_pc. The immediate is a signed 21-bit *byte* offset
    relative to the instruction itself, and the two halves of it are 2 and 19 bits wide --
    truncating the high half to 16 turns every negative offset into a positive one a
    multiple of 0x4000 away, which is a valid instruction pointing somewhere else."""
    delta = to_pc - from_pc
    if delta & 3 or not -(1 << 20) <= delta < (1 << 20):
        raise ValueError(f"adr target {to_pc:#x} is out of range from {from_pc:#x}")
    imm = delta & 0x1FFFFF
    return 0x10000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | (rt & 0x1F)


def encode_b(pc, to_pc):
    """An unconditional branch. ±128 MiB, which is plenty: the stubs are in the same
    blob as the sites they serve."""
    delta = to_pc - pc
    if delta & 3 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError(f"branch to {to_pc:#x} from {pc:#x} is out of range")
    return 0x14000000 | ((delta >> 2) & 0x03FFFFFF)


def encode_ldr_literal(rt, from_pc, to_pc, opc=1):
    """LDR literal, 32-bit (opc 0), 64-bit (opc 1) or LDRSW (opc 2)."""
    if opc not in (0, 1, 2):
        raise ValueError(f"ldr literal opc {opc} is not a GPR load")
    delta = to_pc - from_pc
    if delta & 3 or not -(1 << 20) <= delta < (1 << 20):
        raise ValueError(f"ldr literal target {to_pc:#x} is out of range from {from_pc:#x}")
    return ((0x18000000, 0x58000000, 0x98000000)[opc]
            | (((delta >> 2) & 0x7FFFF) << 5) | (rt & 0x1F))


def pair_offset(insn, base_reg):
    """The byte offset the instruction after an `adrp` adds to it, for the two forms
    compilers actually emit: `add Xd, Xn, #imm12` and `ldr/str [Xn, #imm12 * size]`.
    Returns None when the pairing is some other form, and the caller must then refuse
    rather than rewrite the page and leave the offset wrong."""
    if base_reg is not None and ((insn >> 5) & 0x1F) != base_reg:
        return None
    if (insn & 0xFF800000) == 0x91000000:                    # add Xd, Xn, #imm12 {lsl #shift}
        return ((insn >> 10) & 0xFFF) << ((insn >> 22) & 3)
    if (insn & 0x3B000000) == 0x39000000:                    # ldr/str [Xn, #imm12 * size]
        return ((insn >> 10) & 0xFFF) << ((insn >> 30) & 3)
    return None


def gpr_fields(w):
    """The bit positions of w that hold general-purpose register numbers, by instruction
    shape; None when the shape is not one we can decode.

    Ported from the translation layer's arm64 classifier (research/repos/husk,
    src/translation-layer-next/husk-tl-a64.c), which is the authority on this: a shape
    that decodes wrongly would either miss a register that needs rewriting or invent one
    that does not exist. The loader's own counts (14,378 x18 sites in libPVZ2.so) are
    reproduced by this."""
    f = []
    if (w & 0x1C000000) == 0x10000000:                       # data processing, immediate
        k = (w >> 23) & 7
        if k in (0, 1):
            f = [0]                                             # adr, adrp
        elif k in (2, 4, 6):
            f = [0, 5]                                          # add/sub, logical, bitfield
        elif k == 5:
            f = [0]                                             # move wide
        elif k == 7:
            f = [0, 5, 16]                                      # extract
        else:
            return None
    elif (w & 0x0E000000) == 0x0A000000:                      # data processing, register
        if (w & 0x1F000000) in (0x0A000000, 0x0B000000):
            f = [0, 5, 16]
        elif (w & 0x1FE00000) == 0x1A000000:
            f = [0, 5, 16]                                      # with carry
        elif (w & 0x1FE00000) == 0x1A400000:
            f = [5] + ([] if (w & 0x800) else [16])            # conditional compare
        elif (w & 0x1FE00000) == 0x1A800000:
            f = [0, 5, 16]                                      # conditional select
        elif (w & 0x1F000000) == 0x1B000000:
            f = [0, 5, 10, 16]                                  # three source
        elif (w & 0x5FE00000) == 0x1AC00000:
            f = [0, 5, 16]                                      # two source
        elif (w & 0x5FE00000) == 0x5AC00000:
            f = [0, 5]                                          # one source
        else:
            return None
    elif (w & 0x3A000000) == 0x28000000:                      # load/store pair
        f = ([] if (w >> 26) & 1 else [0, 10]) + [5]
    elif (w & 0x3B000000) == 0x18000000:                      # load literal
        f = [] if ((w >> 26) & 1) or ((w >> 30) & 3) == 3 else [0]
    elif (w & 0x3B000000) in (0x38000000, 0x39000000):         # load/store register
        v, size, opc = (w >> 26) & 1, (w >> 30) & 3, (w >> 22) & 3
        prfm = not v and size == 3 and opc == 2
        f = ([] if (v or prfm) else [0]) + [5]
        if not ((w >> 24) & 1) and ((w >> 21) & 1):
            b = (w >> 10) & 3
            if b in (0, 2):
                f.append(16)                                    # register offset; atomic ops
            else:
                return None                                     # pointer-authenticated
    elif (w & 0x3F000000) == 0x08000000:                      # exclusive / acquire-release
        f = [0, 5, 10, 16]
    elif (w & 0xBE000000) == 0x0C000000:                      # SIMD load/store structure
        f = [5] + ([16] if ((w >> 23) & 1) else [])
    elif (w & 0x0E000000) == 0x0E000000:                      # SIMD and floating point
        if (w & 0x7F20FC00) == 0x1E200000:                     # float <-> int
            k = (w >> 16) & 7
            f = [0] if k in (0, 1, 4, 5, 6) else [5]
        elif (w & 0xFFFFFC00) == 0x1E7E0000:
            f = [0]                                             # fjcvtzs
        elif (w & 0x7F200000) == 0x1E000000:                   # fixed-point conversion
            k = (w >> 16) & 7
            f = [5] if k in (2, 3) else [0]
        elif (w & 0x9FE08400) == 0x0E000400:                   # advanced SIMD copy
            k = (w >> 11) & 0xF
            f = [5] if k in (1, 3) else ([0] if k in (5, 7) else [])
    elif (w & 0xFFC00000) == 0xD5000000:                      # system
        f = [0]
    elif (w & 0x7E000000) in (0x34000000, 0x36000000):         # cbz/cbnz, tbz/tbnz
        f = [0]
    elif (w & 0xFE000000) == 0xD6000000:                      # br, blr, ret
        f = [5]
    elif ((w & 0x7C000000) == 0x14000000 or (w & 0xFF000010) == 0x54000000
          or (w & 0xFFE0001F) == 0xD4000001):                  # b, bl, b.cond, svc
        f = []
    else:
        return None
    return f


def uses_gpr(insn, reg):
    """(does it name `reg`, is the shape one we can decode)"""
    f = gpr_fields(insn)
    if f is None:
        return any(((insn >> s) & 31) == reg for s in (0, 5, 10, 16)), False
    return any(((insn >> s) & 31) == reg for s in f), True


def gpr_mask(insn):
    f = gpr_fields(insn)
    if f is None:
        return 0
    m = 0
    for s in f:
        m |= 1 << ((insn >> s) & 31)
    return m


def subst_gpr(insn, frm, to):
    f = gpr_fields(insn)
    if f is None:
        return insn
    out = insn
    for s in f:
        if ((insn >> s) & 31) == frm:
            out = (out & ~(31 << s)) | (to << s)
    return out


def pick_scratch(used_mask, avoid=0):
    """A register the instruction does not touch, for use inside a stub. x18 is never
    chosen: it is the register being emulated."""
    for r in (16, 17, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8):
        if r != avoid and not (used_mask >> r) & 1:
            return r
    return 0


# ---------------------------------------------------------------- arm64 encoders

def encode_movz(rd, imm16, shift=0, is64=True):
    base = 0xD2800000 if is64 else 0x52800000
    return base | ((imm16 & 0xFFFF) << 5) | (shift << 21) | (rd & 0x1F)


def encode_movk(rd, imm16, shift=0, is64=True):
    base = 0xF2A00000 if is64 else 0x72A00000
    return base | ((imm16 & 0xFFFF) << 5) | (shift << 21) | (rd & 0x1F)


def mov64(rd, value):
    """The two-to-four instructions that materialise a 64-bit constant."""
    out = [encode_movz(rd, value & 0xFFFF, 0)]
    for i in range(1, 4):
        part = (value >> (16 * i)) & 0xFFFF
        if part:
            out.append(encode_movk(rd, part, i))
    return out


# The unscaled and the scaled forms put their two registers in opposite order: in LDR/STR
# the base is above the target, and in LDUR/STUR the target is above the base. Getting that
# backwards compiles to a valid-looking instruction that addresses through the wrong
# register -- which is how a stub that saves a register on the stack ends up storing zero
# through whatever the scratch register happened to be holding.
def encode_stur(rt, imm9, rn=31):
    return 0xF8000000 | ((imm9 & 0x1FF) << 12) | ((rn & 0x1F) << 5) | (rt & 0x1F)


def encode_ldur(rt, imm9, rn=31):
    return 0xF8400000 | ((imm9 & 0x1FF) << 12) | ((rn & 0x1F) << 5) | (rt & 0x1F)


def writes_gpr(insn, reg):
    """Does this instruction write `reg`, or only read it?

    The reserved register cannot stay reserved in the guest's own code: hand-written
    routines use it as an ordinary one -- a NEON string loop here does `and x18, x17, #~15`
    and then indexes off it -- so a stub has to put the result back when the instruction
    computes one, and leave the slot alone when it does not.

    The register number in the low five bits is a destination for most integer shapes, but
    it is the wrong field for several others and there is no destination at all for the
    rest: in a SIMD store those bits name the vector register being stored, in an `ins` or
    `scvtf` they name the vector destination while the GPR is the source in bits 5-9, and
    a pre/post-indexed store writes its base instead of its data register. Treating the
    low field as a destination in those shapes left that GPR out of the stub's frame; the
    accessor's return value -- the address of the x18 word -- then survived in a register
    the guest expected to keep, and the first vector growth stored it as the new begin.
    """
    if reg == 31:
        return False
    rt = insn & 0x1F
    rn = (insn >> 5) & 0x1F
    rt2 = (insn >> 10) & 0x1F
    rs = (insn >> 16) & 0x1F

    # Data processing, immediate: ADR/ADRP, ADD/SUB, logical, bitfield, move wide and
    # extract all put their result in Rd.  Rd=31 is XZR, or SP for ADD/SUB, and neither is
    # a general register.
    if (insn & 0x1C000000) == 0x10000000:
        return ((insn >> 23) & 7) in (0, 1, 2, 4, 5, 6, 7) and rt == reg

    # Data processing, register.  The conditional compares only set the flags; every other
    # shape this classifier decodes writes Rd.
    if (insn & 0x0E000000) == 0x0A000000:
        if (insn & 0x1FE00000) == 0x1A400000:
            return False
        return rt == reg

    # Load/store pair.  Bit 26 is V, bit 22 is the load bit, and bits 24-23 say whether the
    # base is written back (post-index 01, pre-index 11).
    if (insn & 0x3A000000) == 0x28000000:
        if reg == rn and ((insn >> 23) & 3) in (1, 3):
            return True
        return (not ((insn >> 26) & 1) and ((insn >> 22) & 1)
                and reg in (rt, rt2))

    # Load literal: LDR W, LDR X and LDRSW write Rt; PRFM and the SIMD forms do not.
    if (insn & 0x3B000000) == 0x18000000:
        return (not ((insn >> 26) & 1) and ((insn >> 30) & 3) in (0, 1, 2)
                and reg == rt)

    # Load/store register, including the atomic memory operations.
    if (insn & 0x3B000000) in (0x38000000, 0x39000000):
        v = (insn >> 26) & 1
        size = (insn >> 30) & 3
        opc = (insn >> 22) & 3
        load = (insn >> 22) & 1
        b24 = (insn >> 24) & 1
        b21 = (insn >> 21) & 1
        mode = (insn >> 10) & 3
        if not v and size == 3 and opc == 2:
            return False                                      # PRFM
        if not b24 and b21:
            if mode == 0:
                # Atomic memory operations: LDADD/SWP and friends return the old value in
                # Rt; the store-only forms encode XZR there, so they write nothing.  (CAS
                # and the exclusive forms are a different class, below.)
                return not v and reg == rt
            if mode == 2:
                return not v and load == 1 and reg == rt     # register offset
            return False                                      # pointer authentication
        if not v and load == 1 and reg == rt:
            return True
        # Pre/post-index writes the base back.  The unsigned-offset and unscaled forms do
        # not (their bits 11-10 are not an addressing mode).
        return (b24 == 0 and b21 == 0 and mode in (1, 3) and reg == rn)

    # Exclusive and acquire-release.  A load writes Rt (and Rt2 for the pair); a store
    # returns its success/failure status in Rs.  STXR's value operands are sources.
    if (insn & 0x3F000000) == 0x08000000:
        if (insn >> 22) & 1:
            return reg in (rt, rt2)
        return reg == rs

    # SIMD load/store structure: no GPR data destination; a post-index form writes the
    # base (bit 23) back.
    if (insn & 0xBE000000) == 0x0C000000:
        return ((insn >> 23) & 1) == 1 and reg == rn

    # SIMD and floating point.  gpr_fields names the one GPR field: bit 0 is a destination
    # (FCVTZS, FMOV to GPR, UMOV/SMOV), bit 5 is a source (SCVTF, INS, FMOV to FP), and no
    # field means the instruction touches no GPR at all.
    if (insn & 0x0E000000) == 0x0E000000:
        return gpr_fields(insn) == [0] and reg == rt

    # System: MRS and SYSL copy a system register into Rt; MSR and SYS read Rt.
    if (insn & 0xFFC00000) == 0xD5000000:
        return ((insn >> 21) & 1) == 1 and reg == rt

    # CBZ/CBNZ and TBZ/TBNZ only read their register.
    if (insn & 0x7E000000) in (0x34000000, 0x36000000):
        return False

    # BR/BLR/RET and the pointer-authenticated forms.  BLR writes the link register, the
    # others do not.
    if (insn & 0xFE000000) == 0xD6000000:
        return ((insn >> 21) & 0xF) in (1, 9, 11) and reg == 30

    # BL writes the link register; B, B.cond and SVC write no GPR.
    if (insn & 0xFC000000) == 0x94000000:
        return reg == 30
    return False


def encode_orr(rd, rm):
    """ORR Rd, xzr, Rm -- a register move, as one instruction."""
    return 0xAA0003E0 | ((rm & 0x1F) << 16) | (rd & 0x1F)


def encode_add_imm(rt, rn, imm12):
    """ADD (immediate): the other half of the page pair, and the reason an address can be
    written as a page the linker rebases instead of a number that stops being true when the
    image slides."""
    return 0x91000000 | ((imm12 & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)


def encode_add_reg(rd, rn, rm):
    """ADD (extended register), 64-bit, UXTX -- the other way of adding a runtime value to
    an address. The form that does not set the flags: a stub runs the guest's own
    instruction and must not disturb what that instruction set."""
    return 0x8B000000 | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F)


def encode_mrs_tpidr(rt):
    """MRS Xt, TPIDR_EL0 -- the thread pointer of the thread that is running.

    Readable from user space, and the only way a stub can find the running thread's own
    storage without calling anything: a call would take the guest's live registers with
    it, including the link register, and the flags its own instruction had set.

    The destination is the low five bits, as it is for every MRS: the three bits above
    them are the operation, and putting the register there asks for a different system
    register entirely -- a privileged one, which is an undefined instruction at this level
    of privilege, and arrives as SIGILL."""
    return 0xD53BD040 | (rt & 0x1F)


def encode_mrs_nzcv(rt):
    """MRS Xt, NZCV -- the condition flags.

    Readable and writable at this level of privilege, which is what lets a stub hand the
    guest back the flags its own instruction set: the call the stub has to make would
    otherwise take them with it, and the guest's next instruction branches on them."""
    return 0xD53B4200 | (rt & 0x1F)


def encode_msr_nzcv(rt):
    """MSR NZCV, Xt -- the other half of the above."""
    return 0xD51B4200 | (rt & 0x1F)


def pick_callee_saved(used_mask):
    """A register the callee is required to put back (x19-x29), for a stub that needs one
    of its own across a call. It has to be one the guest's own instruction does not touch,
    or the guest's value would be lost."""
    for r in range(19, 30):
        if not (used_mask >> r) & 1:
            return r
    return 0


def encode_sub_imm(rt, rn, imm12):
    """SUB (immediate). `imm12` is the amount to take away, not a signed value: the
    instruction's field holds a magnitude, and `sub sp, sp, #0x100` is the ordinary way to
    make room on the stack."""
    return 0xD1000000 | ((imm12 & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)


def encode_stp(rt, rt2, rn, off):
    """STP (immediate, signed offset) -- a pair of registers stored together.

    The offset counts multiples of eight and is signed, in words. This is the form a stub
    uses to put the guest's live registers somewhere safe across a call: the pair form
    halves the instructions, and the signed offset reaches far enough down the frame to
    hold all of them.

    Two register combinations are not encodings at all. A pair whose two registers are the
    same is UNPREDICTABLE, and so is one whose second register is the zero register; the
    assembler refuses both, and on Apple silicon the unpredictable form raises an
    undefined-instruction exception -- which arrives as SIGILL, with no fault address and
    nothing to say which instruction did it. So they are refused here, where the mistake
    is one line of output."""
    if rt2 == rt:
        raise ValueError("a pair store cannot name the same register twice")
    if rt2 == 31:
        raise ValueError("a pair store cannot name the zero register")
    if off % 8:
        raise ValueError(f"a pair store needs a multiple of eight, not {off:#x}")
    words = off // 8
    if not -64 <= words <= 63:
        raise ValueError(f"a pair store reaches {words} words, which does not fit")
    return 0xA9000000 | ((words & 0x7F) << 15) | ((rt2 & 0x1F) << 10) | \
        ((rn & 0x1F) << 5) | (rt & 0x1F)


def encode_ldp(rt, rt2, rn, off):
    """LDP (immediate, signed offset) -- the other half of encode_stp."""
    if rt2 == rt:
        raise ValueError("a pair load cannot name the same register twice")
    if rt2 == 31:
        raise ValueError("a pair load cannot name the zero register")
    if off % 8:
        raise ValueError(f"a pair load needs a multiple of eight, not {off:#x}")
    words = off // 8
    if not -64 <= words <= 63:
        raise ValueError(f"a pair load reaches {words} words, which does not fit")
    return 0xA9400000 | ((words & 0x7F) << 15) | ((rt2 & 0x1F) << 10) | \
        ((rn & 0x1F) << 5) | (rt & 0x1F)


def encode_ldr_imm(rt, rn, off, size=3):
    """LDR (immediate, unsigned offset). `off` is a byte offset; the instruction's own
    field counts multiples of the access size, and dividing here is what keeps the two
    places that compute offsets from having to."""
    return (size << 30) | 0x39400000 | (((off >> size) & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)


def encode_str_imm(rt, rn, off, size=3):
    return (size << 30) | 0x39000000 | (((off >> size) & 0xFFF) << 10) | \
        ((rn & 0x1F) << 5) | (rt & 0x1F)


REG_FIELDS = ((4, 0), (9, 5), (14, 10), (19, 16))


def names_x18(insn):
    """Whether an instruction names x18, the register Apple reserves and the kernel
    zeroes on every exception return. Every such instruction needs a stub that works on a
    "virtual x18" kept in our own memory instead (see the converter)."""
    return uses_gpr(insn, 18)[0]
    top = insn & 0xFC000000
    if top in (0x14000000, 0x94000000):
        return False
    if (insn & 0x9F000000) in (0x90000000, 0x10000000):
        return False
    return any(((insn >> lo) & 0x1F) == 18 for _, lo in REG_FIELDS)


# ---------------------------------------------------------------- ELF

class Elf:
    def __init__(self, path):
        self.path = path
        self.b = open(path, "rb").read()
        b = self.b
        if b[:4] != b"\x7fELF":
            raise ValueError(f"{path}: not ELF")
        if b[4] != 2 or b[5] != 1:
            raise ValueError(f"{path}: not little-endian 64-bit")
        if u16(b, 0x12) != 0xB7:
            raise ValueError(f"{path}: e_machine is not AArch64")
        e_phoff, e_shoff = u64(b, 0x20), u64(b, 0x28)
        self.phoff, e_phentsize, e_phnum = e_phoff, u16(b, 0x36), u16(b, 0x38)
        self.shoff, e_shentsize, e_shnum = e_shoff, u16(b, 0x3A), u16(b, 0x3C)
        self.shstrndx = u16(b, 0x3E)
        self.phs = []
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            self.phs.append({
                "type": u32(b, o), "flags": u32(b, o + 4), "off": u64(b, o + 8),
                "vaddr": u64(b, o + 0x10), "filesz": u64(b, o + 0x20), "memsz": u64(b, o + 0x28),
                "align": u64(b, o + 0x30),
            })
        self.shs = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            self.shs.append({
                "name": u32(b, o), "type": u32(b, o + 4), "flags": u64(b, o + 8),
                "addr": u64(b, o + 0x10), "off": u64(b, o + 0x18),
                "size": u64(b, o + 0x20), "link": u32(b, o + 0x28), "info": u32(b, o + 0x2C),
                "addralign": u64(b, o + 0x30), "entsize": u64(b, o + 0x38),
            })
        self.dyn, self._needed = {}, []
        for p in self.phs:
            if p["type"] == PT_DYNAMIC:
                o = p["off"]
                while o + 16 <= len(b):
                    tag, val = u64(b, o), u64(b, o + 8)
                    if tag == 0:
                        break
                    if tag == DT_NEEDED:
                        self._needed.append(val)
                    self.dyn[tag] = val
                    o += 16
        self._text = None
        self._funcs = None
        self._ehfuncs = None
        self._shnames = None
        self._pool = None
        self._dmap = None
        self._ehstarts = []

    # -- segments -------------------------------------------------------------

    def segments(self, kind):
        """kind: 'exec' | 'write' | 'read'"""
        flag = {"exec": PF_X, "write": PF_W, "read": 0}[kind]
        return [p for p in self.phs
                if p["type"] == PT_LOAD and (p["flags"] & flag if flag else not (p["flags"] & (PF_X | PF_W)))]

    def word(self, vaddr):
        for p in self.phs:
            if p["type"] != PT_LOAD:
                continue
            o = vaddr - p["vaddr"]
            if 0 <= o and o + 4 <= p["filesz"]:
                return u32(self.b, p["off"] + o)
        return 0

    def bytes_at(self, vaddr, n):
        """n bytes of the file image at a virtual address (zero past filesz).

        The segments of one PT_LOAD do not have to be adjacent: libRFLib.so puts a
        read-only segment at vaddr 0 and starts its code a page later, so a gap between
        two segments is a page of zeros in the image, not the end of it. The caller keeps
        guest addresses by offset from the blob's base, so the gap has to be preserved.
        """
        out = bytearray()
        end = vaddr + n
        while vaddr < end:
            p = None
            for cand in self.phs:
                if cand["type"] != PT_LOAD:
                    continue
                if cand["vaddr"] <= vaddr < cand["vaddr"] + cand["memsz"]:
                    p = cand
                    break
            if p is None:
                # A hole: fill it, then carry on with whatever segment follows.
                after = [c["vaddr"] for c in self.phs
                         if c["type"] == PT_LOAD and c["vaddr"] > vaddr]
                gap = min(min(after, default=end), end) - vaddr
                out += b"\0" * gap
                vaddr += gap
                continue
            o = vaddr - p["vaddr"]
            take = min(end - vaddr, p["filesz"] - o)
            if take > 0:
                out += self.b[p["off"] + o: p["off"] + o + take]
                vaddr += take
            else:
                # Past filesz but inside memsz: .bss, which is zero by definition.
                out += b"\0" * (end - vaddr)
                break
        return bytes(out)
        return bytes(out)

    # -- code ranges ----------------------------------------------------------

    def text_ranges(self):
        if self._text is not None:
            return self._text
        out = []
        for s in self.shs:
            if s["type"] == SHT_PROGBITS and (s["flags"] & SHF_EXECINSTR) and s["size"]:
                lo, hi = s["addr"] & ~3, (s["addr"] + s["size"]) & ~3
                if hi > lo:
                    out.append((lo, hi))
        if not out:
            for p in self.segments("exec"):
                out.append((p["vaddr"], p["vaddr"] + p["filesz"]))
        claimed = [(s["addr"], s["addr"] + (0 if s["type"] == SHT_NOBITS else s["size"]))
                   for s in self.shs if s["flags"] & SHF_ALLOC and s["size"]]
        for p in self.segments("exec"):
            a, end = p["vaddr"], p["vaddr"] + p["filesz"]
            while a < end:
                nxt, inside = end, False
                for lo, hi in claimed:
                    if lo <= a < hi:
                        inside, a = True, hi
                        break
                    if a < lo < nxt:
                        nxt = lo
                if inside:
                    continue
                lo, hi = (a + 3) & ~3, nxt & ~3
                if hi > lo and hi - lo >= 256:
                    out.append((lo, hi))
                a = nxt
        out.sort()
        merged = []
        for lo, hi in out:
            if merged and lo <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
            else:
                merged.append((lo, hi))
        self._text = merged
        return merged

    def code_page(self, page):
        return any(lo <= page < hi for lo, hi in self.text_ranges())

    def data_page(self, page):
        """A page of the image that is not code: what has to be reachable for writing."""
        for p in self.phs:
            if p["type"] == PT_LOAD and p["vaddr"] <= page < p["vaddr"] + p["memsz"]:
                return not self.code_page(page)
        return False

    def in_writable(self, addr):
        return any(p["type"] == PT_LOAD and (p["flags"] & PF_W)
                   and p["vaddr"] <= addr < p["vaddr"] + p["memsz"] for p in self.phs)

    # -- relocations and symbols ---------------------------------------------

    def relocs(self, table="dyn"):
        """Relocations. Android keeps two tables: DT_RELA (.rela.dyn, the data
        relocations) and DT_JMPREL (.rela.plt, one JUMP_SLOT per import). Reading only
        the first misses every import's GOT slot -- for libPVZ2.so that is 3,713 of
        208,658."""
        if table == "plt":
            base, size = self.dyn.get(DT_JMPREL), self.dyn.get(DT_PLTRELSZ, 0)
            if self.dyn.get(DT_PLTREL, 7) != 7:      # REL vs RELA
                return []
        else:
            base, size = self.dyn.get(DT_RELA), self.dyn.get(DT_RELASZ, 0)
        ent = self.dyn.get(DT_RELAENT, 24) or 24
        out = []
        if base is None or not size:
            return out
        foff = self.file_off(base) if base is not None else None
        for i in range(size // ent):
            o = foff + i * ent
            if foff is None or o + 24 > len(self.b):
                break
            r_offset, r_info = u64(self.b, o), u64(self.b, o + 8)
            r_addend = u64(self.b, o + 16)
            out.append({"off": r_offset, "type": r_info & 0xFFFFFFFF,
                        "sym": r_info >> 32, "addend": r_addend})
        return out

    def all_relocs(self):
        return self.relocs("dyn") + self.relocs("plt")

    def needed(self):
        """DT_NEEDED names: each entry's value is an offset into the string table."""
        st = self.dyn.get(DT_STRTAB)
        if st is None:
            return []
        off = self.file_off(st)
        out = []
        for v in self.needed_values():
            o = off + v
            if o >= len(self.b):
                continue
            j = self.b.find(b"\0", o)
            out.append(self.b[o:j if j >= 0 else len(self.b)].decode(errors="replace"))
        return out

    def needed_values(self):
        if DT_NEEDED not in self.dyn:
            return []
        return self._needed

    def file_off(self, vaddr):
        for p in self.phs:
            if p["type"] != PT_LOAD:
                continue
            if p["vaddr"] <= vaddr < p["vaddr"] + p["filesz"]:
                return p["off"] + (vaddr - p["vaddr"])
        raise KeyError(hex(vaddr))

    def dynsyms(self):
        """(name, is_definition) for DT_SYMTAB entries, used to turn bind relocations
        into symbol names."""
        symtab = self.dyn.get(DT_SYMTAB)
        strtab = self.dyn.get(DT_STRTAB)
        if symtab is None or strtab is None:
            return []
        symoff, stroff = self.file_off(symtab), self.file_off(strtab)
        # the first entry is reserved; DT_HASH/others tell us the count, so guess from
        # the relocation symbol indices we actually need instead
        return (symoff, stroff)

    def sym_name(self, symoff, stroff, idx):
        o = symoff + idx * 24
        nameoff = u32(self.b, o)
        j = self.b.index(b"\0", stroff + nameoff)
        return self.b[stroff + nameoff:j].decode()

    def undef_symbols(self):
        """Names of undefined dynamic symbols (the imports)."""
        base = self.dyn.get(DT_SYMTAB)
        strtab = self.dyn.get(DT_STRTAB)
        strsz = self.dyn.get(DT_STRSZ, 0)
        if base is None or strtab is None or not strsz:
            return []
        symoff, stroff = self.file_off(base), self.file_off(strtab)
        out = []
        for i in range(strsz // 24 + 1):
            o = symoff + i * 24
            if o + 24 > len(self.b):
                break
            st_shndx = u16(self.b, o + 6)
            nameoff = u32(self.b, o)
            if not nameoff:
                continue
            j = self.b.index(b"\0", stroff + nameoff)
            name = self.b[stroff + nameoff:j].decode()
            if st_shndx == 0:
                out.append(name)
        return sorted(set(out))

    def soname(self):
        if DT_SONAME not in self.dyn:
            return None
        off = self.file_off(self.dyn[DT_SONAME])
        j = self.b.index(b"\0", off)
        return self.b[off:j].decode()

    # -- dynamic symbols -------------------------------------------------------

    def symtab(self):
        """(file offset of the symbols, of the strings, count). The section headers of
        these libraries are stripped, so the count comes from DT_HASH's nchain, or from
        the GNU hash table, or from where the string table starts."""
        symtab, strtab = self.dyn.get(DT_SYMTAB), self.dyn.get(DT_STRTAB)
        if symtab is None or strtab is None:
            return None, None, 0
        n = None
        if 4 in self.dyn:                                  # DT_HASH
            off = self.file_off(self.dyn[4])
            n = u32(self.b, off + 4)                       # nchain == nsyms
        elif 0x6FFFFEF5 in self.dyn:                        # DT_GNU_HASH
            off = self.file_off(self.dyn[0x6FFFFEF5])
            nbuckets, symoffset = u32(self.b, off), u32(self.b, off + 4)
            bloom_size = u32(self.b, off + 8)
            buckets = off + 16 + bloom_size * 8
            vals = [u32(self.b, buckets + 4 * i) for i in range(nbuckets)]
            last = max(vals) if vals else symoffset - 1
            if last >= symoffset:
                chains = buckets + nbuckets * 4
                n = last - symoffset + 1 + (u32(self.b, chains + 4 * (last - symoffset)) & ~1) + 1
        # the string table follows the symbols, so it bounds the count (and the GNU
        # hash walk can overshoot it)
        cap = (strtab - symtab) // 24
        if n is None or n > cap:
            n = cap
        return self.file_off(symtab), self.file_off(strtab), n

    def symbols(self):
        """[(name, value, size, type, shndx)]"""
        symoff, stroff, n = self.symtab()
        if symoff is None:
            return []
        out = []
        for i in range(n):
            o = symoff + i * 24
            if o + 24 > len(self.b):
                break
            nameoff = u32(self.b, o)
            if stroff + nameoff >= len(self.b):
                continue                       # count overshot the table
            j = self.b.find(b"\0", stroff + nameoff)
            if j < 0:
                continue
            out.append((self.b[stroff + nameoff:j].decode(errors="replace"), u64(self.b, o + 8),
                        u64(self.b, o + 16), self.b[o + 4] & 0xF, u16(self.b, o + 6)))
        return out

    def section_names(self):
        if self._shnames is None:
            self._shnames = {}
            if self.shstrndx < len(self.shs):
                base = self.shs[self.shstrndx]["off"]
                for sec in self.shs:
                    j = self.b.find(b"\0", base + sec["name"])
                    self._shnames[sec["addr"]] = self.b[base + sec["name"]:j].decode(
                        errors="replace")
        return self._shnames

    def eh_frame_funcs(self):
        """(start, end) for every function the unwind tables describe.

        This, not the symbol table, is what says whether a word in the code is an
        instruction: these libraries are stripped, so the symbol table has only their
        exported names -- a few hundred functions in libPVZ2.so against tens of thousands.
        Every compiled function has a frame description, so the FDE ranges cover the code
        the way a constant table never does. The translation layer uses the same oracle,
        which is what makes its x18 counts reproducible here.
        """
        if self._ehfuncs is not None:
            return self._ehfuncs
        out = []
        for sec in self.shs:
            if self.section_names().get(sec["addr"]) != ".eh_frame":
                continue
            blob, n = self.b[sec["off"]:sec["off"] + sec["size"]], sec["size"]
            off, cie_encoding = 0, 0x00          # DW_EH_PE_absptr is the default
            while off + 8 <= n:
                length = u32(blob, off)
                if length in (0, 0xFFFFFFFF):    # terminator, or a 64-bit extended length
                    break
                end = off + 4 + length
                if end > n:
                    break
                cie_id = u32(blob, off + 4)
                p = off + 8
                if cie_id == 0:                   # a CIE: its augmentation names the encoding
                    version = blob[p]
                    p += 1
                    j = blob.find(b"\0", p)
                    aug = blob[p:j].decode(errors="replace")
                    p = j + 1
                    if version >= 4:
                        p += 2                   # address size, segment selector size
                    _, p = _uleb(blob, p)         # code alignment factor
                    _, p = _sleb(blob, p)         # data alignment factor
                    if version == 1:
                        p += 1                   # return address register, one byte
                    else:
                        _, p = _uleb(blob, p)
                    if aug.startswith("z"):
                        zlen, p = _uleb(blob, p)
                        zend = p + zlen
                        for c in aug[1:]:
                            if c == "R":
                                cie_encoding = blob[p]
                                p += 1
                            elif c == "L":
                                _, p = _uleb(blob, p)
                            elif c == "P":
                                enc = blob[p]
                                p += 1 + max(_enc_len(enc), 0)
                            # "S" carries nothing
                        p = zend
                else:                             # an FDE: where it starts, how long
                    loc = _decode_encoded(blob, p, cie_encoding, sec["addr"] + off)
                    size = _decode_encoded(blob, p + max(_enc_len(cie_encoding), 0),
                                           cie_encoding & 0x0F, sec["addr"] + off)
                    if loc and size and loc + size > loc:
                        out.append((loc, loc + size))
                off = (end + 3) & ~3
        out.sort()
        merged = []
        for lo, hi in out:
            if merged and lo <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
            else:
                merged.append((lo, hi))
        self._ehfuncs = merged
        self._ehstarts = [lo for lo, _ in merged]
        return merged

    def code_page(self, page):
        return any(lo <= page < hi for lo, hi in self.text_ranges())

    def data_page(self, page):
        """A page of the image that is not code: what has to be reachable for writing."""
        for p in self.phs:
            if p["type"] == PT_LOAD and p["vaddr"] <= page < p["vaddr"] + p["memsz"]:
                return not self.code_page(page)
        return False

    def in_writable(self, addr):
        return any(p["type"] == PT_LOAD and (p["flags"] & PF_W)
                   and p["vaddr"] <= addr < p["vaddr"] + p["memsz"] for p in self.phs)

    # -- relocations and symbols ---------------------------------------------

    def iter_code(self):
        """(vaddr, word) over every word of every code range.

        A typed view of the file's bytes, not a lookup per address: these libraries hold
        22 MB of code, and calling word() for six million addresses took longer than the
        rest of the conversion put together."""
        for lo, hi in self.text_ranges():
            n = (hi - lo) // 4
            if n <= 0:
                continue
            try:
                off = self.file_off(lo)
            except KeyError:
                continue
            view = memoryview(self.b)[off:off + n * 4].cast("I")
            for i, w in enumerate(view):
                yield lo + 4 * i, w

    def literal_pool(self):
        """Addresses read by a `ldr <literal>` inside a function.

        Those words are constants wherever they sit, even between instructions: a
        compiler that runs out of range for an immediate parks a 64-bit value mid-function
        and loads it from there. Rewriting one of those as if it were an instruction would
        silently corrupt the constant, so the translation layer's data map claims them
        first, and so does this."""
        if self._pool is not None:
            return self._pool
        pool = set()
        for i, insn in self.iter_code():
            if (insn & 0x3B000000) == 0x18000000 and self.in_function(i):
                t = ldr_literal_target(i, insn)
                v, opc = (insn >> 26) & 1, (insn >> 30) & 3
                nw = ({0: 1, 1: 2, 2: 4}.get(opc, 0) if v else {1: 2, 3: 0}.get(opc, 1))
                for k in range(max(nw, 0)):
                    pool.add(t + 4 * k)
        self._pool = pool
        return pool

    @staticmethod
    def plausible_word(w):
        """Whether a word could be an instruction at all."""
        if (w >> 16) == 0:
            return True                     # zero padding between functions, and traps
        return ((w >> 25) & 0xF) > 3         # op0 of the A64 space: 0000-0011 unallocated

    def data_map(self):
        """Addresses in the code ranges that are data rather than instructions.

        The translation layer's rule, ported so the two agree site for site. A cluster of
        words that could not be an instruction is a constant table; the words around such a
        cluster are the table too, since a table's first and last words are ordinary
        values. Then two corrections: a function body is never a table (the unwind tables
        say where functions are), and the words a `ldr <literal>` reads are data wherever
        they are.
        """
        if self._dmap is not None:
            return self._dmap
        # The windows run per code range, the way the translation layer does it: a table
        # sits inside one range, and letting a window straddle a gap between functions
        # changes a handful of sites at the edges.
        out = set()
        for rlo, rhi in self.text_ranges():
            try:
                n = (rhi - rlo) // 4
                if n <= 0:
                    continue
                words = memoryview(self.b)[self.file_off(rlo):][: n * 4].cast("I")
            except (KeyError, ValueError):
                continue
            bad = bytearray(0 if (self.in_function(rlo + 4 * i) or self.plausible_word(w))
                            else 1 for i, w in enumerate(words))
            hot = bytearray(n)
            total = sum(bad[:8])
            for i in range(len(bad)):
                if i + 8 < len(bad):
                    total += bad[i + 8]
                if i >= 9:
                    total -= bad[i - 9]
                if total >= 3 and bad[i]:
                    hot[i] = 1
            data = bytearray(len(bad))
            last = -1
            for i in range(len(bad)):
                if not hot[i]:
                    continue
                start = last if (last >= 0 and i - last <= 64) else max(i - 32, 0)
                for k in range(start, min(i, len(bad) - 1) + 1):
                    data[k] = 1
                for k in range(i + 1, min(i + 33, len(bad))):
                    data[k] = 1
                last = i
            for i, flag in enumerate(data):        # a function body is never a table
                if flag and self.in_function(rlo + 4 * i):
                    data[i] = 0
            for i, w in enumerate(words):          # nor is what a literal load reads
                if (w & 0x3B000000) == 0x18000000 and not data[i]:
                    t = rlo + 4 * i
                    tgt = ldr_literal_target(t, w)
                    v, opc = (w >> 26) & 1, (w >> 30) & 3
                    nw = ({0: 1, 1: 2, 2: 4}.get(opc, 0) if v else
                          {1: 2, 3: 0}.get(opc, 1))
                    for k in range(max(nw, 0)):
                        j = (tgt - rlo) // 4 + k
                        if 0 <= j < len(data):
                            data[j] = 1
            out.update(rlo + 4 * i for i, flag in enumerate(data) if flag)
        self._dmap = out
        return self._dmap

    def in_function(self, vaddr, ranges=None):
        """Whether an address lies inside a function, per the unwind tables."""
        if ranges is not None:
            return any(lo <= vaddr < hi for lo, hi in ranges)
        if self._ehfuncs is None:
            self.eh_frame_funcs()
        i = bisect.bisect_right(self._ehstarts, vaddr) - 1
        return i >= 0 and vaddr < self._ehfuncs[i][1]

    def func_ranges(self):
        """Merged [start, end) of every sized STT_FUNC symbol: what the loader treats as
        code when it decides whether an x18 word is an instruction or a constant."""
        if self._funcs is not None:
            return self._funcs
        rs = sorted((v, v + s) for _, v, s, t, _ in self.symbols() if t == 2 and v and s)
        merged = []
        for lo, hi in rs:
            if merged and lo <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
            else:
                merged.append((lo, hi))
        self._funcs = merged
        return merged

    def symbol_func_ranges(self):
        """Merged ranges of the sized STT_FUNC symbols: only the library's exported
        functions, but exact where they exist."""
        if self._funcs is None:
            rs = sorted((v, v + sz) for _, v, sz, t, _ in self.symbols() if t == 2 and v and sz)
            merged = []
            for lo, hi in rs:
                if merged and lo <= merged[-1][1]:
                    merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
                else:
                    merged.append((lo, hi))
            self._funcs = merged
        return self._funcs