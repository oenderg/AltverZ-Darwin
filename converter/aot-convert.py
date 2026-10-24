#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
aot-convert.py -- turn an Android arm64 ELF shared library into a signed Mach-O dylib.

The point of this route is that the device must never write code, so every rewrite the
translation layer performs at load time has to happen here instead, on a machine that may
write anything. What it does, and why each step is cheap:

  layout      the guest's own layout is preserved: code bytes stay where the ELF put them,
              so every B/ADR/ADRP/LDR-literal in the library keeps working at any slide.
              The data segment moves (Mach-O needs it page-aligned); PC-relative
              references into it are therefore re-encoded, and the tool refuses to convert
              a library where it cannot.
  relocations every RELA slot in these libraries is inside a writable segment (checked per
              library: 0 in code for all of 1.4.2), so they all become pointers in a
              writable segment. They are emitted as assembler *expressions* (`.quad
              _guest_code + 0x1234`), which lets ld64 write the rebase/bind opcodes and the
              export trie instead of us hand-encoding them.
  patches     `mrs Xt, tpidr_el0` becomes `adrp Xt, <the fake thread block>`, which is
              page-relative and therefore slide-invariant; a stub pool is reserved in the
              code for the x18 and svc cases (not implemented yet -- the tool says so
              rather than guessing).

The target platform matters: a dylib linked for macOS will not load on iOS ("incompatible
platform"), which is not something you want to discover on the device. `--target` takes a
clang target triple; the default is macOS, which is what the offline tests run.

Run:  aot-convert.py <lib.so> [-o outdir] [--stub-bytes N] [--allow-unimplemented]
      [--target arm64-apple-ios17.0]
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aotlib import (Elf, PAGE, u32, u64, DT_INIT_ARRAY, DT_INIT_ARRAYSZ, is_adrp, is_adr, is_ldr_literal,
                    is_ldr_literal_any, is_tpidr_read,
                    is_tpidr_write, is_ctr_read, is_svc, names_x18, adrp_target, adr_target,
                    ldr_literal_target, sext, encode_adrp, encode_adr, encode_ldr_literal,
                    pair_offset, gpr_fields,
                    encode_b, mov64, encode_stur, encode_ldur, encode_ldr_imm, encode_str_imm, encode_add_imm,
                    encode_sub_imm, encode_stp, encode_ldp, encode_add_reg,
                    encode_mrs_tpidr, encode_mrs_nzcv, encode_msr_nzcv, pick_callee_saved,
                    encode_movz,
                    encode_orr,
                    gpr_mask, subst_gpr, pick_scratch, uses_gpr, writes_gpr,
                    REBASE_TYPES, BIND_TYPES, RELOC_NAMES, ABS64, sdk_for_target)

# Where the code section starts in the linked image: __TEXT at 0 with the code blob aligned
# to 16 KiB. Not a prediction -- the linker is told where __DATA goes and the result is
# checked, and the two disagree only if ld64 changed its mind about __TEXT.
CODE_SECTION_ADDR = 0x4000
HOOK_SECTION = "__guest_hooks"    # tail calls into the shim, for the redirected names


def hook_label(name):
    return "_guest_hook_" + name


# Symbols the shim takes over. The guest's inline-hook API is the only one: it writes a
# branch into the library it hooks, which a converted library cannot allow.
DEFAULT_REDIRECT = ("A64HookFunction", "A64HookFunctionV")
STUB_SEGMENT = "__GUESTSTUBS"   # executable, mapped after the data; see emit_asm
CODE_LABEL = "_guest_code"
DATA_LABEL = "_guest_data"
STUB_LABEL = "_guest_stubs"
TCB_LABEL = "_guest_tcb"
TCB_BYTES = 0x1000
CTR_EL0_VALUE = 0x8444C004     # what Apple's hardware reports; the smallest line
VX18_LABEL = "_guest_vx18"     # the virtual x18: Apple reserves the register, so the
VX18_BYTES = 0x1000           # guest's value lives in memory and stubs move it in and out
X18FN_LABEL = "_guest_x18_fn"  # the shim function a stub calls for the per-thread word
X18FN_BASE = "husk_guest_x18_addr_"   # ... one per library, so which library is an
X18_MAX_LIBS = 16              # immediate in the stub rather than a word in the data
X18_STUB_MAX = 192            # bytes reserved per x18 site, worst case
TCBFN_LABEL = "_guest_tcb_fn"  # the shim function a tpidr stub calls for the thread block
TCBFN_NAME = "husk_guest_tcb_addr"
TCB_STUB_MAX = 192            # bytes reserved per tpidr site, worst case
MACOS_TARGET = "arm64-apple-macos"

# ---------------------------------------------------------------- the mod's hooks
#
# The mod hooks the game's own functions: it asks for a branch to be written over the
# first instruction of each one it wants to replace. A converted library's text is signed
# and read-only, so the branch is written offline instead, into the image, and the request
# is answered at run time by writing the replacement's address into a slot.
#
# What is baked, at each hooked site (every piece in the same library, so every distance
# between them is a link-time constant and survives the slide):
#
#   the site    a branch to a dispatch stub in this library's own stub segment
#   the stub    loads the replacement's address out of a slot in this library's data and
#               jumps to it. A zero slot -- the hook was never asked for -- falls through
#               to the trampoline instead, so the original code still runs.
#   the slot    eight bytes in this library's data, written by the shim at run time
#   the tramp   the instruction the site displaced, put back, and a branch to site+4: the
#               original function, for both the fallback and the mod's own calls through
#               the trampoline the shim hands it
#
# The numbers themselves live in scripts/hook-layout.h, which the shim includes and this
# converter parses. The shim finds the stub by decoding the branch at the site, so the two
# ends have to agree on where every field of the stub is; two copies of the numbers would
# drift.
#
def hook_layout():
    """The numbers the converter and the shim must agree on, read from the one file both
    of them are written against (scripts/hook-layout.h). Parsing rather than copying is
    the point: a stub the shim misreads is a branch into nowhere, and nothing in either
    file would notice a second copy going stale."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook-layout.h")
    want = ("HOOK_STUB_BYTES", "HOOK_SLOT_DELTA", "HOOK_TRAMP_DELTA", "HOOK_MAGIC_AT",
            "HOOK_MAGIC", "HOOK_SLOT_BYTES", "HOOK_TRAMP_MAX")
    vals = {}
    for line in open(path):
        m = re.match(r"#define\s+(HOOK_\w+)\s+(0x[0-9a-fA-F]+|\d+)", line)
        if m and m.group(1) in want:
            vals[m.group(1)] = int(m.group(2), 0)
    missing = [w for w in want if w not in vals]
    if missing:
        raise Bail(f"scripts/hook-layout.h has no {', '.join(missing)}")
    return vals


HOOK = hook_layout()
HOOK_STUB_BYTES = HOOK["HOOK_STUB_BYTES"]     # the dispatch stub and its three numbers
HOOK_SLOT_DELTA = HOOK["HOOK_SLOT_DELTA"]     # where the stub carries the slot's distance
HOOK_TRAMP_DELTA = HOOK["HOOK_TRAMP_DELTA"]   # ... and the trampoline's
HOOK_MAGIC_AT = HOOK["HOOK_MAGIC_AT"]         # ... and the magic
HOOK_MAGIC = HOOK["HOOK_MAGIC"]               # "HOK!" -- tells the shim a stub is one of ours
HOOK_SLOT_BYTES = HOOK["HOOK_SLOT_BYTES"]     # the replacement's address, in the library's data
HOOK_TRAMP_MAX = HOOK["HOOK_TRAMP_MAX"]       # room for the trampoline of the worst case


class Bail(Exception):
    pass


# ---------------------------------------------------------------- image layout

def layout(e, hook_slots_n=0):
    """The image, laid out so that a guest address maps to `code + (vaddr - base)`.

    The whole point is that *every* guest address -- code and writable data alike -- moves
    by the same amount. Mach-O puts code in __TEXT and writable data in __DATA, and the
    linker chooses where __DATA goes; we tell it instead: __DATA is placed at exactly
    `code + (data_base - code_base)`, so the distance between any two guest addresses is
    what it was in the ELF.

    That is what makes the conversion cheap and safe. An `adrp` names a page and some later
    instruction adds an offset, and the pair is not always adjacent -- a compiler will put
    a `mov` in between -- so re-encoding every reference into the data would mean tracking
    which register each one used and how far ahead its partner was. Keeping the layout
    means there is nothing to re-encode: the arithmetic the guest does is arithmetic about
    its own image, and its image is intact.

    Read-only data joins the code blob, since Mach-O has no r-- segment and its distances
    to the code are part of that same arithmetic. The cost is that constants become
    executable, which costs nothing: the same pages hold code.

    The one rewrite that remains is the thread block: `mrs Xt, tpidr_el0` names a word the
    guest believes is at a fixed offset in the thread block, and the thread block is ours.
    It gets a page inside the data blob, laid out the way the compilers expect."""
    ro = e.segments("exec") + e.segments("read")
    if not ro:
        raise Bail("no executable segment")
    base = min(p["vaddr"] for p in ro)
    end = max(p["vaddr"] + p["filesz"] for p in ro)
    code = bytearray(e.bytes_at(base, end - base))
    wr = e.segments("write")
    if not wr:
        raise Bail("no writable segment")
    wend = max(p["vaddr"] + p["memsz"] for p in wr)
    wbase = min(p["vaddr"] for p in wr)

    # How far past the code the data has to sit: the guest's own distance, rounded down so
    # the page arithmetic still works (a guest page must land on an image page), and large
    # enough to hold the code blob that comes first. Rounding down only adds zeros.
    # The distance has to be a whole number of pages twice over: the code blob's shift must
    # be page-aligned for the guest's own adrp arithmetic, and the two segments must agree
    # modulo 16 KiB because this platform maps in 16 KiB pages. libRFLib.so's writable
    # data starts 0x1530 bytes past a 16 KiB boundary, which satisfies neither, so for it
    # the layout cannot be kept and every reference into the data is re-encoded instead.
    want = wbase - base
    delta = None
    for align in (16384, PAGE):
        cand = want & ~(align - 1)
        if cand >= len(code) and cand % PAGE == 0:
            delta = cand
            break
    preserve = delta is not None
    if not preserve:
        delta = (want + 0x3FFF) & ~0x3FFF          # 16 KiB aligned, wherever it lands
    wbase = base + delta
    data = bytearray(e.bytes_at(wbase, wend - wbase))
    # A page of padding at the end, because a relocation sometimes holds the address one
    # past the last mapped byte -- the limit an allocator compares against. With the page,
    # that pointer still lands inside our blob; without it, there is nowhere to put it.
    # Then the thread block and the reserved register's page, both inside the data blob so
    # that they too sit where the rewritten instructions expect them.
    data += b"\0" * PAGE
    tcb_off = (len(data) + PAGE - 1) & ~(PAGE - 1)
    data += b"\0" * (tcb_off + PAGE - len(data))
    vx18_off = tcb_off + PAGE
    data += b"\0" * PAGE
    # One slot per baked hook, at the end of the writable data: the shim writes the
    # replacement's address into it when the mod asks for the hook, and the stub baked at
    # the site loads it. Zero until then, which is what makes a site whose hook has not
    # been asked for yet run its original code. Page-aligned so the stub's `adrp` names a
    # page the slot is wholly inside, and 8 bytes each so the load can be a plain LDR.
    slots_off = (len(data) + PAGE - 1) & ~(PAGE - 1)
    hook_slots = [wbase + slots_off + HOOK_SLOT_BYTES * i for i in range(hook_slots_n)]
    data += b"\0" * (slots_off + HOOK_SLOT_BYTES * hook_slots_n - len(data))
    return {"code_base": base, "code": code, "data_base": wbase, "data": data,
            "data_real": wend - wbase, "delta": delta, "preserve": preserve,
            "tcb_off": tcb_off, "vx18_off": vx18_off,
            "hook_slots": hook_slots}


def seg_of(e, vaddr):
    for p in e.phs:
        if p["type"] == 1 and p["vaddr"] <= vaddr < p["vaddr"] + p["memsz"]:
            return p
    return None


def host_expr(e, lay, link_addr):
    """An assembler expression for an address in the guest image, as a label plus an
    offset, so the linker resolves it and emits the rebase."""
    if link_addr < 0:
        raise Bail(f"cannot express address {link_addr:#x}")
    cb, db = lay["code_base"], lay["data_base"]
    if cb <= link_addr < cb + len(lay["code"]):
        return f"{CODE_LABEL} + {link_addr - cb:#x}"
    if db <= link_addr < db + len(lay["data"]):
        return f"{DATA_LABEL} + {link_addr - db:#x}"
    raise Bail(f"address {link_addr:#x} is in neither the code nor the data image")


# ---------------------------------------------------------------- survey

def survey(e, lay, redirect=()):
    """Everything the rewrite needs, counted before anything is written."""
    out = {"tpidr": [], "ctr": [], "x18_sites": [], "x18_unknown": [], "svc": [],
           "code_refs": [],
           "adrp_literals": 0, "unknown_refs": [], "relocs": [], "binds": []}
    # x18: Apple reserves the register and the kernel zeroes it on every exception return,
    # so a guest instruction may not keep a value in it. Each such instruction becomes a
    # branch to a stub that works on a "virtual x18" in our own memory. Which words are
    # instructions and which are constants comes from the data map, the same oracle the
    # translation layer uses -- a constant table that looked like an instruction would be
    # rewritten into a branch, and nothing would notice until the guest read it.
    dmap = e.data_map()
    for i, insn in e.iter_code():
        if ((insn & 31) == 18 or ((insn >> 5) & 31) == 18 or ((insn >> 10) & 31) == 18
                or ((insn >> 16) & 31) == 18):
            hit, known = uses_gpr(insn, 18)
            if hit and known and i not in dmap:
                out["x18_sites"].append(i)
            elif hit and not known:
                out["x18_unknown"].append(i)
    for lo, hi in e.text_ranges():
        i = lo
        while i + 4 <= hi:
            insn = e.word(i)
            if is_tpidr_read(insn):
                out["tpidr"].append(i)
            elif is_tpidr_write(insn):
                raise Bail(f"{e.path}: the guest writes tpidr_el0 at {i:#x}; the loader "
                           "does not patch that, so refuse rather than guess")
            elif is_ctr_read(insn):
                out["ctr"].append(i)
            elif is_svc(insn):
                out["svc"].append(i)
            i += 4
    # PC-relative references. The code keeps its own layout, so a reference into code
    # stays correct at any slide; only the data blob moves, and the references into it
    # have to be re-encoded. An `adrp` names a page, not an address -- the address is the
    # page plus whatever the next instruction adds -- so the pair is decoded as a unit.
    # Where the next instruction is not a pairing form we can decode, the reference is
    # recorded as unresolved rather than assumed harmless.
    db, de = lay["data_base"], lay["data_base"] + len(lay["data"])
    for lo, hi in e.text_ranges():
        i = lo
        while i + 4 <= hi:
            insn = e.word(i)
            if is_adrp(insn):
                page = adrp_target(i, insn)
                rd = insn & 0x1F
                nxt = e.word(i + 4) if i + 8 <= hi else None
                off = pair_offset(nxt, rd) if nxt is not None else None
                if off is None:
                    if db <= page < de:
                        out["code_refs"].append(("adrp", i, page, 0, "page"))
                    elif not (lay["code_base"] <= page < lay["code_base"] + len(lay["code"])):
                        out["unknown_refs"].append((i, page))
                else:
                    eff = page + off
                    if db <= eff < de:
                        out["code_refs"].append(("adrp", i, eff, off, "pair"))
            elif is_adr(insn):
                t = adr_target(i, insn)
                if db <= t < de:
                    out["code_refs"].append(("adr", i, t, 0, ""))
            elif is_ldr_literal(insn):
                t = ldr_literal_target(i, insn)
                if db <= t < de:
                    out["code_refs"].append(("ldr", i, t, 0, ""))
            i += 4
    # An adrp whose page is outside the whole image, or which is not followed by a pairing
    # form, is left alone: it cannot be a live reference into anything we moved, and it is
    # usually a constant that happens to decode. The loader skips these too.
    symvals = {i: v for i, (_, v, _, _, _) in enumerate(e.symbols())}
    for r in e.all_relocs():
        off, typ = r["off"], r["type"]
        if not e.in_writable(off):
            raise Bail(f"{e.path}: relocation {RELOC_NAMES.get(typ, typ)} at {off:#x} is "
                       "not in a writable segment; a Mach-O cannot rebase it there")
        # A JUMP_SLOT or GLOB_DAT whose symbol this library *defines* is not an import:
        # the guest calls its own function through the PLT and wants the slot to hold that
        # function's address. libPVZ2.so has 3,238 of those. Treating them as imports
        # would ask dyld for symbols nobody exports, so they are rebases like any other
        # self-reference.
        defined_here = bool(symvals.get(r["sym"], 0)) and r["sym"] != 0
        symoff, stroff = e.dynsyms()
        name = e.sym_name(symoff, stroff, r["sym"]) if symoff and r["sym"] else ""
        # A name the shim is meant to take over. The guest's own inline-hook API is one:
        # A64HookFunction stores a branch straight into the library it is hooking, and
        # under conversion that code is signed and read-only. So the call is bound to the
        # shim, which records the request; the hook itself is baked in ahead of time.
        if defined_here and name in redirect and typ in BIND_TYPES | {ABS64}:
            out["binds"].append({"off": off, "sym": name, "addend": r["addend"]})
            continue
        # ABS64 is a pointer to a symbol, wherever it sits. In the code segment the
        # instruction that loads it is rewritten instead, so the slot is not one the
        # linker fills; in the data segment it is a plain 8-byte word that has to receive
        # the symbol's address. libPVZ2.so has 6,458 of those against imported symbols --
        # every reference to std::exception's typeinfo, every vtable slot, every
        # __shared_weak_count destructor -- and read as a rebase they get the addend,
        # which for a plain pointer is zero, and the guest then calls through a null.
        if typ == ABS64 and not defined_here:
            out["binds"].append({"off": off, "sym": name, "addend": r["addend"]})
            continue
        if typ in BIND_TYPES and not defined_here:
            out["binds"].append({"off": off, "sym": name, "addend": r["addend"]})
            continue
        if typ in REBASE_TYPES or defined_here:
            # RELATIVE is B + A; ABS64 is S + A. The link-time address of the slot is
            # what the assembler expression has to name, so that is what goes into the
            # dylib and dyld re-adds the slide to it.
            if typ == 1027:
                value = r["addend"] & 0xFFFFFFFFFFFFFFFF
            else:
                value = (symvals.get(r["sym"], 0) + r["addend"]) & 0xFFFFFFFFFFFFFFFF
            # A value of zero is the image base (B + 0), which is a real address the
            # linker can express; host_expr decides whether it is one we can name.
            out["relocs"].append({"off": off, "value": value, "addend": r["addend"]})
        else:
            raise Bail(f"{e.path}: relocation type {RELOC_NAMES.get(typ, typ)} at {off:#x} "
                       "is not implemented")
    return out


# ---------------------------------------------------------------- emit

def emit_asm(e, lay, sv, outdir, stub_bytes, mod_init=False, code_gap=0, redirect=(),
             lib_index=0):
    data = lay["data"]
    slots = {}
    for r in sv["relocs"]:
        slots.setdefault(r["off"], ("rebase", r["value"]))
    for b in sv["binds"]:
        slots.setdefault(b["off"], ("bind", b["sym"], b.get("addend", 0)))
    for off in slots:
        if off % 8:
            raise Bail(f"relocation slot at {off:#x} is not 8-byte aligned")
    lines = [
        "// generated by aot-convert.py -- do not edit",
        "// the guest image, preserved; the linker places it and writes the rebases",
        # The stub pool gets a segment of its own, after the data. It cannot live in
        # __TEXT: __DATA's address is pinned to code_base + (data_base - code_base), and
        # libPVZ2's 900 KiB of stubs do not fit in the gap the guest left there. Only the
        # branches baked below ever reach it, so it can live anywhere in the image.
        ".section %s,%s,regular,pure_instructions" % (STUB_SEGMENT, "__guest_stubs"),
        f".globl {STUB_LABEL}",
        f"{STUB_LABEL}:",
        f".space {stub_bytes:#x}",
        ".section __TEXT,%s,regular,pure_instructions" % ("__guest_code"),
        ".p2align 4",
        # Padding inside this section, in front of the blob, so that the blob starts where
        # it has to: the data segment's address is the code's address plus the guest's own
        # distance between the two, and that sum must come out 16 KiB aligned or ld64 will
        # not accept the address. It cannot go in the section before this one, because ld64
        # orders sections by alignment and would put them the other way round. The first
        # link measures where the section landed; the second pads by the difference.
        f".space {code_gap:#x}",
        f".globl {CODE_LABEL}",
        f"{CODE_LABEL}:",
        f'.incbin "{CODE_LABEL}.bin"',
        ".section __DATA,__guest_data,regular",
        f".globl {DATA_LABEL}",
        f"{DATA_LABEL}:",
    ]
    pos = 0
    pending = bytearray()

    def flush():
        if not pending:
            return
        for i in range(0, len(pending), 16):
            chunk = pending[i:i + 16]
            lines.append(".byte " + ",".join(str(c) for c in chunk))
        pending.clear()

    for off in sorted(slots):
        d_off = off - lay["data_base"]
        if not (0 <= d_off < len(data)):
            raise Bail(f"relocation slot at {off:#x} is outside the data image")
        pending += data[pos:d_off]
        flush()
        slot = slots[off]
        if slot[0] == "rebase":
            lines.append(f".quad {host_expr(e, lay, slot[1])}")
        else:
            _, sym, addend = slot
            # An import is S + A, not just S. Vtable pointers are the common case here:
            # a guest typeinfo/vtable reference names the symbol plus 0x10, and dropping
            # the addend points the guest at the start of the vtable instead of its first
            # entry, which makes every dynamic_cast through that typeinfo fail.
            if addend & (1 << 63):
                addend -= 1 << 64
            if addend > 0:
                lines.append(f".quad _{sym} + {addend:#x}")
            elif addend < 0:
                lines.append(f".quad _{sym} - {-addend:#x}")
            else:
                lines.append(f".quad _{sym}")
        pos = d_off + 8
    # The thread block and the reserved register's page, labelled inside the data blob so
    # the rewritten instructions can name them and the runtime can find and initialise them.
    for off, label, what in ((lay["tcb_off"], TCB_LABEL, "the thread block"),
                             (lay["vx18_off"], VX18_LABEL, "the virtual x18")):
        if pos <= off < len(data):
            pending += data[pos:off]
            flush()
            lines.append(f".globl {label}")
            lines.append(f"{label}:")
            pos = off
    pending += data[pos:]
    flush()
    # How a stub finds the reserved register's value *for the thread that is running*. The
    # word itself is per-thread and lives in the shim, which is the only place that can keep
    # one: a converted library's data is one image shared by every thread, so a word in it
    # is one value for all of them, and a guest that uses the register as a general one (this
    # game does, in hand-written SIMD) would hand another thread's value to the next
    # instruction to read it.
    #
    # Which library's word it is, is an immediate in the stub's own code rather than a
    # second word here. That word was reachable: the guest can store anywhere it can compute
    # an address, and a library whose identity arrived as 0, 1, 2 or 3 is what an overwritten
    # word looks like. An immediate cannot be overwritten.
    if lib_index >= X18_MAX_LIBS:
        raise Bail(f"library index {lib_index} is past the {X18_MAX_LIBS} the shim keeps words for")
    lines.append(".section __DATA,__guest_x18,regular")
    lines.append(f".globl {X18FN_LABEL}")
    lines.append(f"{X18FN_LABEL}:")
    lines.append(f".quad _{X18FN_BASE}{lib_index}")
    # And the same arrangement for the thread block. Unlike the reserved register, this one
    # is not per-library: a thread pointer is a property of the thread, and all four
    # libraries read the same one, so a single shim function serves them all. Naming it
    # here rather than in the stub's immediate means the branch is bound at link time.
    lines.append(".section __DATA,__guest_tcb_fn,regular")
    lines.append(f".globl {TCBFN_LABEL}")
    lines.append(f"{TCBFN_LABEL}:")
    lines.append(f".quad _{TCBFN_NAME}")
    lines.append("")
    # Constructors. Mach-O calls the entries of __mod_init_func when the image loads, so
    # listing the guest's init array there gives the same constructors in the same order
    # the ELF's loader would run them (libPVZ2.so has 1,046), with no table for the
    # runtime to walk. The array itself is already in the data blob with its relocations
    # applied; this is the list dyld reads.
    ia = e.dyn.get(DT_INIT_ARRAY)
    iasz = e.dyn.get(DT_INIT_ARRAYSZ, 0)
    entries = []
    if ia and iasz:
        # The list has to be the one the *image* will hold, not the one the file shows.
        # A dynamic linker computes each slot as base + addend, and this library's
        # .init_array relocation addends are all the same address even though the file
        # has 1,046 different ones in it: the file was patched after linking. Reading the
        # file would run 1,045 constructors the real loader never runs.
        relocated = {r["off"]: r["value"] for r in sv["relocs"]}
        for k in range(iasz // 8):
            vaddr = ia + 8 * k
            if not (lay["data_base"] <= vaddr < lay["data_base"] + len(lay["data"])):
                continue
            entries.append(relocated.get(vaddr, u64(e.b, e.file_off(vaddr))))
    # The constructors are recorded, not run by dyld. The order matters: the guest's own
    # loader runs them after the driver's JNI environment exists (tl_ld_init), because
    # they call back into Java. A __mod_init_func section would run them at dlopen,
    # which is earlier and hangs. The list goes in the facts file instead, and the
    # runtime walks it at the same point the ELF's loader would have.
    if entries and mod_init:
        lines.append(".section __DATA,__mod_init_func,regular")
        lines.append(".p2align 3")
        for val in entries:
            lines.append(f".quad {host_expr(e, lay, val)}")
        lines.append("")
    # Forwarders for the redirected names. The guest's own definition cannot stay: it
    # stores a branch into the library it is hooking, and a converted library's code is
    # signed and read-only. So the definition's first instruction becomes a branch to one
    # of these, and these tail-call the shim, which records what was asked for -- which is
    # what baking the hook ahead of time needs.
    if redirect:
        lines.append(f".section __TEXT,{HOOK_SECTION},regular")
        for name in redirect:
            lines.append(f".globl {hook_label(name)}")
            lines.append(f"{hook_label(name)}:")
            lines.append(f"b _{name}")            # the shim's copy, bound at load
        lines.append("")
    init_n = len(entries)
    # The guest's own symbols. ld64 cannot export a name it cannot see, and every one of
    # them is a word inside an opaque blob -- so they are defined as expressions over the
    # blob's label. dyld slides an exported symbol like any other, which is what makes two
    # converted libraries able to link to each other (libPVZ2.so calls Nimble's getEnv,
    # which lives inside libNimble's blob).
    # They go in their own __TEXT section: an expression-defined symbol left in a __DATA
    # section comes out of the assembler private, and ld64 then drops it from the export
    # trie, which is exactly the symbol a sibling library needs to bind.
    lines.append(".section __TEXT,__guest_exports,regular")
    lines.append("")
    exports = {}
    for name, val, size, typ, shndx in e.symbols():
        if shndx and name and name not in (CODE_LABEL, DATA_LABEL, STUB_LABEL, TCB_LABEL,
                                           VX18_LABEL):
            # A redirected name is not exported: its definition now branches to the
            # shim, and exporting it would make the forwarder bind to this library's own
            # (patched) entry instead of to the shim's implementation.
            if name in redirect:
                continue
            exports[name] = val
    for name, val in sorted(exports.items(), key=lambda kv: kv[1]):
        try:
            expr = host_expr(e, lay, val)
        except Bail:
            continue                        # a symbol outside the image (an ABS one)
        lines.append(f".globl _{name}")
        lines.append(f".set _{name}, {expr}")
    if exports:
        lines.append("")
        lines.append(f"// {len(exports)} of the guest's own symbols, for its siblings to bind")
        lines.append("")

    path = os.path.join(outdir, "guest.s")
    open(path, "w").write("\n".join(lines))
    sv["constructors"] = init_n
    sv["init_entries"] = entries
    open(os.path.join(outdir, f"{CODE_LABEL}.bin"), "wb").write(bytes(lay["code"]))
    return path


# ---------------------------------------------------------------- Mach-O reading

def macho_sections(path):
    """(bytes, sections by name, symbols by name). Symbols matter because the linker
    drops a section whose contents are all zeroes -- the reserved stub pool has no bytes
    until a later pass fills it -- so its address comes from the symbol table."""
    b = open(path, "rb").read()
    if u32(b, 0) != 0xFEEDFACF:
        raise Bail(f"{path}: not a 64-bit Mach-O")
    ncmds, sizeofcmds = u32(b, 16), u32(b, 20)
    off, secs, syms = 32, {}, {}
    symtab = None
    for _ in range(ncmds):
        cmd, cmdsize = u32(b, off), u32(b, off + 4)
        if cmd == 0x19:                      # LC_SEGMENT_64
            name = b[off + 8:off + 24].rstrip(b"\0").decode()
            nsects = u32(b, off + 64)
            s = off + 72
            for _ in range(nsects):
                sname = b[s:s + 16].rstrip(b"\0").decode()
                secs[sname] = {"seg": name, "addr": u64(b, s + 32), "size": u64(b, s + 40),
                               "offset": u32(b, s + 48)}
                s += 80
        elif cmd == 0x2:                    # LC_SYMTAB
            symtab = (u32(b, off + 8), u32(b, off + 12), u32(b, off + 16))
        off += cmdsize
    if symtab:
        symoff, nsyms, stroff = symtab
        for i in range(nsyms):
            o = symoff + i * 16
            n_strx = u32(b, o)
            if n_strx == 0:
                continue
            j = b.find(b"\0", stroff + n_strx)
            if j < 0:
                continue
            syms[b[stroff + n_strx:j].decode()] = u64(b, o + 8)
    return b, secs, syms


FRAME_BYTES = 0x100    # the stack a stub takes while it calls the shim
LONE_OFF = 0xA0        # the slot for a register that has no partner to be paired with
SSAVE_OFF = 0xA8       # the slots for the callee-saved registers a stub borrows
TSAVE_OFF = 0xB0
PSAVE_OFF = 0xB8
FLAG_OFF = 0xC0        # the slot for the condition flags
WORD_OFF = 0xC8       # the slot for the address the shim returned
FSAVE_OFF = 0xE0       # the slot for the frame register of an instruction that names sp
PARK_OFF = 0xF0        # the slot for a result waiting to be put where it belongs


def frame_save(out, keep):
    """Take a frame and put the caller's registers somewhere they can be put back.

    A stub has to call the shim to find this thread's copy of the guest's thread block,
    and a call takes the caller's registers with it. The guest's instructions are not
    calls -- the compiler expects everything else to be where it left it -- so the stub
    has to put them back itself.

    `keep` is the registers to save: the ones a call may destroy, less any the guest's
    own instruction writes (that result has to survive). The callee-saved registers are
    not in it: the callee puts those back by the ordinary rules.

    Returns the pairs and the one left over, so the restore can undo it exactly.
    """
    pairs = [(keep[i], keep[i + 1]) for i in range(0, len(keep) - 1, 2)]
    lone = keep[-1] if len(keep) % 2 else None
    out.append(encode_sub_imm(31, 31, FRAME_BYTES))
    for i, (a, b) in enumerate(pairs):
        out.append(encode_stp(a, b, 31, i * 16))
    if lone is not None:
        out.append(encode_str_imm(lone, 31, LONE_OFF))
    return pairs, lone


def frame_restore(out, pairs, lone):
    """Put the caller's registers back.

    The frame itself is released by the caller, which may still have something of its own
    to read out of it first.
    """
    for i, (a, b) in enumerate(pairs):
        out.append(encode_ldp(a, b, 31, i * 16))
    if lone is not None:
        out.append(encode_ldr_imm(lone, 31, LONE_OFF))


def flags_save(out, P):
    """Put the condition flags somewhere the call cannot reach them.

    The call the stub has to make takes the flags with it, and the guest's next
    instruction may branch on them. So they are read out before the call and written back
    after it -- before the guest's own instruction runs, so that the flags that
    instruction sets are the ones that survive.

    `P` is a callee-saved register the stub borrows, so the call cannot take it; its
    guest value is saved and put back, because the guest may have had something live
    there.
    """
    out.append(encode_str_imm(P, 31, PSAVE_OFF))
    out.append(encode_mrs_nzcv(P))
    out.append(encode_str_imm(P, 31, FLAG_OFF))


def flags_restore(out, P):
    """The flags the guest had, back again."""
    out.append(encode_ldr_imm(P, 31, FLAG_OFF))
    out.append(encode_msr_nzcv(P))


def uses_sp(insn):
    """Does the instruction name sp (rather than xzr) as a value?

    The stub's frame sits below the guest's stack pointer, so an instruction that reads
    sp through that frame reads the frame's own slots: `ldr x18,[sp,#0xc0]` finds the
    saved flags at exactly that offset, and the next instruction uses the carry bit as a
    pointer. The general case therefore puts the guest's sp back for the instruction and
    takes its own frame back afterwards.
    """
    # Load/store: the base is bits 5-9, and 31 there is sp.
    if (insn & 0x3B000000) in (0x38000000, 0x39000000) and ((insn >> 5) & 31) == 31:
        return True
    if (insn & 0x3A000000) == 0x28000000 and ((insn >> 5) & 31) == 31:   # ldp/stp
        return True
    # ADD/SUB: in the immediate and extended-register forms 31 is sp; in the shifted form
    # Rm=31 is xzr, but Rn and Rd are still sp. Rd=31 with S set is xzr, not sp.
    if (insn & 0x1F000000) in (0x11000000, 0x0B000000):
        rd, rn, rm = insn & 31, (insn >> 5) & 31, (insn >> 16) & 31
        if rn == 31:
            return True
        if rd == 31 and not ((insn >> 29) & 1):
            return True
        if ((insn >> 21) & 1) and rm == 31:
            return True
    return False


def writes_sp(insn):
    """Does the instruction write sp? Only ADD/SUB can, and only with S clear."""
    if (insn & 0x1F000000) in (0x11000000, 0x0B000000):
        return (insn & 31) == 31 and not ((insn >> 29) & 1)
    return False


def x18_stub(stub_pc, site_pc, insn, slot, mapped=None):
    """The stub for one instruction that names x18, as a list of words.

    The guest's value for the register lives in memory -- one word per thread, in the shim
    -- and the stub moves it into a register the instruction does not touch, runs the
    instruction there, and puts the result back if the instruction wrote one. Nothing ever
    writes the real x18, so an exception that zeroes it costs the guest nothing.

    `slot` is the pair of words the accessor is found and identified by: the shim function,
    which dyld has bound, and this library's own data blob, which is what the shim keys the
    per-thread storage on.

    The stub has to call the shim to find this thread's word, and that call is expensive in
    the only currency that matters here: the guest's own state. A call takes the caller's
    registers with it -- the link register among them, and the flags -- and the guest's
    instructions are not calls, so the compiler expects everything else to be where it left
    it. The stub therefore takes a frame, saves what the call would take, and puts it back.

    The order of that is the whole difficulty, and getting it wrong is invisible until the
    guest reads a register the call left behind. The guest's own instruction runs *inside*
    the stub, so the registers the call destroyed have to be back in place *before* it runs
    -- and the stub's own working registers have to survive that restore. They do, because
    they are callee-saved: x19-x29 are the ones a called function puts back for us, so the
    stub borrows three of them and the restore cannot touch them. The register the guest's
    instruction writes is left out of the frame, because its result has to survive.

    The flags are the same story and worse, because the guest's instruction may be a
    compare whose result the next instruction branches on: they are read out before the
    call and written back after it, and after that nothing else touches them -- the stores
    and the loads that follow do not set them, and the frame is given back with an add that
    does not either.

    Three shapes get their own treatment, because the instruction does not read x18 as a
    register: a pc-relative form, whose result is a constant we can compute here; a literal
    load, whose target may be a rebound pointer and so must be read at run time; and an
    indirect branch, which becomes a load and a branch. Follows the translation layer's
    stubs, which are the ones the game already runs.

    Two addresses, and the difference between them is the whole reason this took a while to
    get right. `site_pc` is where the guest's instruction is and where the stub returns to;
    `stub_pc` is where the stub itself will be, and it is the one every PC-relative
    instruction inside it has to be encoded against. An adrp names a page relative to the
    instruction that executes it, so encoding against the site and running in the stub pool
    puts every one of these a few megabytes from where it means to be -- and, since the
    stub pool is further from the data than the code blob is, somewhere past the end of the
    image, where the load faults on the first site the guest reaches.
    """
    back = site_pc + 4
    out = []

    def pc():
        """The address the instruction at the end of `out` will execute at."""
        return stub_pc + 4 * len(out)

    used = gpr_mask(insn)
    # The registers the guest's instruction writes: its result has to survive, so they are
    # not put back. The instruction's own operands are in `used`.
    written = [r for r in range(31) if writes_gpr(insn, r)]
    # The stub's working registers. They have to be *callee-saved* -- x19-x29 -- because the
    # call to the shim destroys the caller-saved ones and the stub needs them afterwards.
    # Which means the guest's values in them have to be saved and put back: a stub that
    # borrowed x27 and left it alone would break the game's stack-protector check, which
    # reads the cookie through the thread pointer in x27. It did, and the symptom was an
    # abort with the stack guard tripped.
    free = [r for r in range(19, 30) if not (used >> r) & 1 and r not in written]
    # An instruction that names sp needs one more callee-saved register: the frame the
    # stub has to come back to after running the guest's instruction with the guest's sp.
    sp_used = uses_sp(insn)
    if sp_used and writes_sp(insn):
        raise Bail("an x18 site writes sp; the stub cannot keep its own frame")
    if len(free) < (4 if sp_used else 3):
        raise Bail("no callee-saved registers free for an x18 stub")
    S, T, P = free[0], free[1], free[2]
    FP = free[3] if sp_used else None
    # The registers the call takes with it, less the ones the guest's own instruction
    # writes. The callee-saved ones are not in here: the callee puts those back, and the
    # stub puts back the three it borrows itself.
    #
    # A register the instruction writes is only dead *before* the instruction if the
    # instruction does not also read it. `madd x8, x0, x18, x8` reads and writes x8, and
    # leaving x8 out of the frame lets the shim's x8 -- a dyld address, as it happens --
    # become the addend. So a named operand is put back even when it is also the result.
    keep = [r for r in range(0, 19) if r == 18 or r not in written or (used >> r) & 1]
    if 30 not in written:
        keep.append(30)
    pairs, lone = frame_save(out, keep)
    # The three borrowed registers, and the flags, go where the call cannot reach them.
    out.append(encode_str_imm(S, 31, SSAVE_OFF))
    out.append(encode_str_imm(T, 31, TSAVE_OFF))
    out.append(encode_str_imm(P, 31, PSAVE_OFF))
    if sp_used:
        out.append(encode_str_imm(FP, 31, FSAVE_OFF))
    out.append(encode_mrs_nzcv(P))
    out.append(encode_str_imm(P, 31, FLAG_OFF))

    page = slot["fn"] & ~0xFFF

    def load_reg():
        """Find this thread's word for the register, and leave its address in the frame.

        The call is made with the accessor in x16, which the call is allowed to destroy;
        what comes back -- the address of the word -- is parked on the stack, because the
        frame's restore is about to put the guest's registers back and x0 is one of them.
        """
        out.extend([
            encode_adrp(16, pc(), page),
            encode_ldr_imm(16, 16, slot["fn"] - page),   # this library's accessor
            encode_movz(0, slot["lib"]),                  # x0 = which library
            0xD63F0000 | (16 << 5),                       # blr x16 -> x0 = the word
            encode_str_imm(0, 31, WORD_OFF),              # park the address of the word
        ])
        # The flags the guest had, back: the call took them, and the guest's own
        # instruction may be a compare whose result the next instruction branches on.
        out.append(encode_ldr_imm(P, 31, FLAG_OFF))
        out.append(encode_msr_nzcv(P))

    def put_reg():
        """The word in S back into this thread's slot."""
        out.append(encode_str_imm(S, T, 0))

    def finish():
        """Put the guest's registers back and go home."""
        frame_restore(out, pairs, lone)
        out.append(encode_ldr_imm(S, 31, SSAVE_OFF))     # the borrowed registers
        out.append(encode_ldr_imm(T, 31, TSAVE_OFF))
        out.append(encode_ldr_imm(P, 31, PSAVE_OFF))
        out.append(encode_add_imm(31, 31, FRAME_BYTES))
        out.append(encode_b(pc(), back))

    if is_adrp(insn) or is_adr(insn):
        load_reg()
        # The address is rebuilt as the page pair the linker rebases, not as a number baked
        # here. This is x18's new value and the next instruction reads it straight back, so
        # it has to be the address in the *loaded* image: a constant written at conversion
        # time is right until dyld slides the library, and wrong after -- as a fault on a
        # link-time data address, which is what it was.
        target = mapped & ~0xFFF
        out.append(encode_ldr_imm(T, 31, WORD_OFF))      # the address of the word
        out += [encode_adrp(S, pc(), target), encode_add_imm(S, S, mapped - target)]
        put_reg()
        finish()
        return out
    if is_ldr_literal_any(insn):
        # the literal may hold a pointer dyld rebases, so read it at run time rather than
        # baking in a value that is only correct before the slide.  The width matters: a
        # 32-bit LDR must zero the top half and LDRSW must sign-extend, as the guest's own
        # instruction would have.  A literal load is only PC-relative within +-1 MiB and
        # the stub pool is much farther than that from some sites, so address the literal
        # with adrp+add and load through the register.
        load_reg()
        out.append(encode_ldr_imm(T, 31, WORD_OFF))
        page = mapped & ~0xFFF
        out.append(encode_adrp(S, pc(), page))
        out.append(encode_add_imm(S, S, mapped - page))
        opc = (insn >> 30) & 3
        if opc == 0:
            out.append(encode_ldr_imm(S, S, 0, size=2))       # ldr wS,[xS]
        elif opc == 1:
            out.append(encode_ldr_imm(S, S, 0, size=3))       # ldr xS,[xS]
        else:
            out.append(0xB9800000 | (S << 5) | S)             # ldrsw xS,[xS]
        put_reg()
        finish()
        return out
    opc = (insn >> 21) & 0xF
    if (insn & 0xFE000000) == 0xD6000000 and opc in (0, 1, 2):
        # BR, BLR or RET through x18.  All three need the guest's registers and the
        # guest's stack back before control leaves the stub, and the target has to be
        # read out of the x18 word while the frame is still there.  It is parked in the
        # word slot, which the frame restore does not touch.
        load_reg()
        out.append(encode_ldr_imm(T, 31, WORD_OFF))
        out.append(encode_ldr_imm(T, T, 0))                   # T = x18's value
        out.append(encode_str_imm(T, 31, WORD_OFF))           # park the target
        frame_restore(out, pairs, lone)
        out.append(encode_ldr_imm(S, 31, SSAVE_OFF))
        out.append(encode_ldr_imm(T, 31, TSAVE_OFF))
        out.append(encode_ldr_imm(P, 31, PSAVE_OFF))
        # The real x18 is not guest state: no guest instruction reads it, so it can carry
        # the target across the frame release.  (This used to restore S before branching
        # and jumped to the borrowed register's guest value instead of the target.)
        out.append(encode_ldr_imm(18, 31, WORD_OFF))
        out.append(encode_add_imm(31, 31, FRAME_BYTES))
        if opc == 1:
            # BLR: call with the guest's stack and registers in place.  The callee returns
            # to the instruction after the call; give the guest the link value its own
            # BLR would have produced, not the address inside the stub.
            out.append(0xD63F0000 | (18 << 5))                # blr x18
            page = back & ~0xFFF
            out.append(encode_adrp(30, pc(), page))
            out.append(encode_add_imm(30, 30, back - page))
            out.append(encode_b(pc(), back))
        else:
            # BR and RET do not write the link register, and frame_restore has already put
            # the guest's x30 back.
            out.append(0xD61F0000 | (18 << 5))                # br x18
        return out
    if (insn & 0x7E000000) in (0x34000000, 0x36000000):        # cbz/cbnz, tbz/tbnz
        tb = (insn & 0x7E000000) == 0x36000000
        imm_bits, sign = (0x3FFF, 0x2000) if tb else (0x7FFFF, 0x40000)
        raw = (insn >> 5) & imm_bits
        if raw & sign:
            raw -= sign * 2
        # the branch target is where the guest's own instruction would have gone
        target = site_pc + raw * 4
        load_reg()
        frame_restore(out, pairs, lone)
        out.append(encode_ldr_imm(T, 31, WORD_OFF))
        out.append(encode_ldr_imm(S, T, 0))
        # The test jumps over the fall-through path (three restores, the frame
        # release and the branch back) to the copy that branches to the guest's
        # target.  The earlier offset was three, so the taken path landed on the
        # last restore, took the branch back, and the target branch at the end was
        # unreachable: the condition was effectively ignored.
        test = (insn & ~((imm_bits << 5) | 0x1F)) | (6 << 5) | S
        out.append(test)
        # fall-through: the condition was not taken; run the instruction after the site
        out.append(encode_ldr_imm(S, 31, SSAVE_OFF))
        out.append(encode_ldr_imm(T, 31, TSAVE_OFF))
        out.append(encode_ldr_imm(P, 31, PSAVE_OFF))
        out.append(encode_add_imm(31, 31, FRAME_BYTES))
        out.append(encode_b(pc(), back))
        # taken: the same restore, then the guest's own branch target
        out.append(encode_ldr_imm(S, 31, SSAVE_OFF))
        out.append(encode_ldr_imm(T, 31, TSAVE_OFF))
        out.append(encode_ldr_imm(P, 31, PSAVE_OFF))
        out.append(encode_add_imm(31, 31, FRAME_BYTES))
        out.append(encode_b(pc(), target))
        return out
    # The general case: run the instruction on a register it does not touch.
    #
    # Writing the result back is the question, and the guest decides it. Most instructions
    # that name the register read it, and storing the scratch register's value back would
    # replace the thread pointer with whatever the instruction happened to compute. But
    # hand-written routines use the register as a general one -- a NEON string loop in this
    # game does `and x18, x17, #~15` and then indexes off it -- so when the instruction does
    # write it, the result has to go back. Which of the two it is comes from the encoding.
    load_reg()
    frame_restore(out, pairs, lone)      # the guest's registers, before its own instruction
    out.append(encode_ldr_imm(T, 31, WORD_OFF))
    out.append(encode_ldr_imm(S, T, 0))  # the register's value, this thread's
    if sp_used:
        # The instruction reads the guest's stack, not this frame: keep the frame in FP
        # and give the guest's sp back for the one instruction, then take the frame back.
        out.append(encode_add_imm(FP, 31, 0))
        out.append(encode_add_imm(31, FP, FRAME_BYTES))
    out.append(subst_gpr(insn, 18, S))
    if sp_used:
        out.append(encode_add_imm(31, FP, 0))
    if writes_gpr(insn, 18):
        put_reg()
    out.append(encode_ldr_imm(S, 31, SSAVE_OFF))
    out.append(encode_ldr_imm(T, 31, TSAVE_OFF))
    out.append(encode_ldr_imm(P, 31, PSAVE_OFF))
    if sp_used:
        out.append(encode_ldr_imm(FP, 31, FSAVE_OFF))
    out.append(encode_add_imm(31, 31, FRAME_BYTES))
    out.append(encode_b(pc(), back))
    return out


def tpidr_stub(stub_pc, site_pc, rt, slot):
    """The stub for one `mrs Xt, tpidr_el0`.

    The instruction asks for the running thread's block. A page baked into the dylib would
    answer, and would answer wrongly: the guest writes through this block (libPVZ2 stores
    its stack cookie at tp+0x28 in 1,993 of the places it reads it, and the libraries also
    write tp+0x18, tp+0xc8 and further out), and the page in the image is one page for the
    main, GL and audio threads at once. So the site branches here instead and the shim --
    the only thing that can hold one value per thread -- hands back this thread's block.

    Three details the shape of the guest decides rather than the shape of the stub.

    It is entered with `b`, not `bl`. The stub has to return to site_pc + 4, and doing that
    with a branch keeps the link register untouched: a `bl` here would overwrite the return
    address the guest's caller is about to read. The stub branches out at the end instead,
    to an address fixed when the stub is placed.

    The registers the guest had live across its `mrs` are still live after the stub. A
    `mrs` is an ordinary instruction to the compiler, not a call, so anything the compiler
    had in a register before it is still there afterwards -- and asking the shim is a call,
    which takes the caller-saved registers with it. The compiler is entitled to keep
    anything in x0-x17, so the stub puts those back. x19-x29 the callee has already put
    back for us, by the ordinary rules, and the one the stub borrows for the flags it
    saves and restores itself.

    The flags are the same: the call takes them, and the guest's next instruction may
    branch on them.

    This is not a theoretical hazard. In libPVZ2 the sequence at 0x540c04 is
    `adrp x9, ...; mrs x22, tpidr_el0; add x9, x9, #0x69e; ldr x8, [x22, #0x28]` -- x9 is
    live across the `mrs`, and a stub that let the call clobber it sent the next instruction
    to a wild address. The result is parked on the stack across the restore rather than
    written straight into its register, because that register may be one of the ones being
    restored.
    """
    page = slot["fn"] & ~0xFFF
    S = pick_scratch((1 << rt) | (1 << 30) | (1 << 18), 0)
    T = pick_scratch((1 << rt) | (1 << 30) | (1 << 18) | (1 << S), S)
    if not S or not T:
        raise Bail("no scratch registers free for a tpidr stub")
    # The registers the call takes with it, less the ones the guest's own instruction
    # writes -- that result has to survive the stub, so it is not put back.
    if rt == 18:
        # The value would have to go into the reserved register's per-thread word, which
        # only the shim can find, and finding it is another call. No site in 1.4.2 asks
        # for it, so rather than guess, say so.
        raise Bail("`mrs x18, tpidr_el0` is not rewritten: the result would have to go "
                   "into the reserved register's per-thread word, and no site in this "
                   "library asks for it")
    written = [rt]
    keep = [r for r in range(0, 19) if r == 18 or r not in written]
    if 30 not in written:
        keep.append(30)
    # The register the stub borrows for the flags. It must not be the one the guest asked
    # the value for: the result is written there, and then the stub's own bookkeeping --
    # putting the borrowed register's value back -- would overwrite it. The game's own
    # thread-pointer sequence is `mrs x19, tpidr_el0`, so that register is the one the
    # stub must keep its hands off.
    P = pick_callee_saved(1 << rt)
    if not P:
        raise Bail("no callee-saved register free for a tpidr stub")
    out = []
    pairs, lone = frame_save(out, keep)
    out.append(encode_str_imm(P, 31, PSAVE_OFF))
    out.append(encode_mrs_nzcv(P))
    out.append(encode_str_imm(P, 31, FLAG_OFF))

    def pc():
        return stub_pc + 4 * len(out)

    out += [
        encode_adrp(S, pc(), page),
        encode_ldr_imm(T, S, slot["fn"] - page),      # the shim's accessor
        0xD63F0000 | (T << 5),                        # blr T -> x0 = this thread's block
        encode_str_imm(0, 31, PARK_OFF),              # park the result
    ]
    out.append(encode_ldr_imm(P, 31, FLAG_OFF))
    out.append(encode_msr_nzcv(P))
    out.append(encode_ldr_imm(rt, 31, PARK_OFF))     # the result, into the register asked for
    frame_restore(out, pairs, lone)
    out.append(encode_ldr_imm(P, 31, PSAVE_OFF))     # the borrowed register's value
    out.append(encode_add_imm(31, 31, FRAME_BYTES))
    out.append(encode_b(pc(), site_pc + 4))
    return out


def const_stub(stub_pc, site_pc, rd, value):
    """A stub that leaves a constant in a register and returns.

    Used for `mrs Xt, ctr_el0`, which is privileged on Apple silicon: the cache type
    register is the smallest cache line the program may assume, and code that flushes the
    instruction cache (a JIT engine's own housekeeping) reads it. The value is the one the
    translation layer uses -- what Apple's hardware reports, and safe for any.

    `stub_pc` is where this code will be and `site_pc` is where it returns to, for the same
    reason as in x18_stub: the branch back is encoded from the address that executes it.
    """
    S = pick_scratch(1 << rd)
    if not S:
        raise Bail("no scratch register free for a constant stub")
    out = mov64(S, value)
    out.append(0xAA0003E0 | (S << 16) | (rd & 0x1F))     # orr Rd, xzr, S
    out.append(encode_b(stub_pc + 4 * len(out), site_pc + 4))
    return out


# ---------------------------------------------------------------- the mod's hooks


def hook_stub(stub_pc, slot_addr, tramp_addr):
    """The dispatch stub for one baked hook, as a list of words.

    The stub loads the replacement's address out of this library's own data and jumps to
    it. The replacement is in another library, whose address is not known until everything
    is loaded, so the branch to it cannot be baked -- only the indirection can.

    The slot is zero until the mod asks for the hook, and a still-zero slot takes the
    trampoline instead, so the original code runs. That fallback is not a nicety: the
    branch at the site is live from the moment the image is loaded, and libPVZ2's own
    constructors run before libRFLib's ask for anything.

    The two distances after the code are measured from the stub itself, which is what
    survives the slide: the image moves as a whole, so the distance between two of its
    addresses does not change. The shim reads them at the offsets in hook-layout.h and
    checks the magic before believing any of it.

        +0   adrp x16, <the page the slot is on>
        +4   ldr  x17, [x16, #<the slot's offset from that page>]
        +8   cbz  x17, #+8        no replacement yet: run the original code
        +12  br   x17             the replacement
        +16  b    <the trampoline>
        +20  the slot's distance from this stub
        +28  the trampoline's distance from this stub
        +36  the magic
    """
    page = slot_addr & ~0xFFF
    slot_delta = slot_addr - stub_pc
    tramp_delta = tramp_addr - stub_pc
    code = [
        encode_adrp(16, stub_pc, page),
        encode_ldr_imm(17, 16, slot_addr - page),
        0xB4000000 | (2 << 5) | 17,            # cbz x17, #+8
        0xD61F0000 | (17 << 5),                # br x17
        encode_b(stub_pc + 16, tramp_addr),
    ]
    # The three fields sit at the offsets the shim reads them from, and those offsets
    # live in hook-layout.h for exactly this reason: the writer and the reader must not
    # drift apart. Writing them at hardcoded word indices would let the header change
    # without the writer following, and the shim would then read the slot's address out
    # of the middle of the code.
    if HOOK_SLOT_DELTA % 4 or HOOK_TRAMP_DELTA % 4 or HOOK_MAGIC_AT % 4:
        raise Bail("hook layout: the stub's fields are not at word-aligned offsets")
    slot_i, tramp_i, magic_i = HOOK_SLOT_DELTA // 4, HOOK_TRAMP_DELTA // 4, HOOK_MAGIC_AT // 4
    if slot_i != len(code):
        raise Bail("hook layout: the stub's code does not end where the slot's distance goes")
    words = [0] * (HOOK_STUB_BYTES // 4)
    for i, w in enumerate(code):
        words[i] = w
    words[slot_i] = slot_delta & 0xFFFFFFFF
    words[slot_i + 1] = (slot_delta >> 32) & 0xFFFFFFFF
    words[tramp_i] = tramp_delta & 0xFFFFFFFF
    words[tramp_i + 1] = (tramp_delta >> 32) & 0xFFFFFFFF
    words[magic_i] = HOOK_MAGIC
    assert len(words) * 4 == HOOK_STUB_BYTES
    return words


def hook_trampoline(tramp_pc, site_pc, insn):
    """The trampoline for one baked hook, as a list of words.

    It holds the instruction that was displaced from the site -- the site's own first
    instruction, as the linked image has it -- and a branch back to the instruction after
    it. A caller that wants the original function calls this, and so does the stub while
    the hook has not been asked for, so it has to run the original code exactly.

    The displaced instruction is usually a stack adjustment or a frame save, which does
    not care where it is. One that names an address has to name the same one from the
    trampoline: a branch is re-encoded to the same target, and a conditional branch is
    split into its inverse (skipping the branch below it) and an unconditional one,
    because the target can be too far away for the conditional form. An unconditional
    branch is the one exception to the branch back: it is the whole of the instruction's
    effect and control does not fall through it, so there is nothing to continue with.
    """
    def resume(when):
        """The branch that finishes the trampoline: the original code continues at the
        instruction after the one the site displaced."""
        return encode_b(when, site_pc + 4)

    out = []
    # The stub that the site's branch reaches uses x16 for the slot's page and x17 for
    # the replacement's address, and it does that before the trampoline runs. A displaced
    # instruction that *reads* x16 or x17 would therefore read the stub's leftovers
    # instead of the caller's values. None of the 290 sites does; the check keeps it that
    # way for the next hook set.
    fields = gpr_fields(insn)
    if fields is not None:
        for bit in fields:
            reg = (insn >> bit) & 0x1F
            if reg in (16, 17):
                raise Bail("hook site %#x: the displaced instruction reads x%d, which the "
                           "hook stub uses" % (site_pc, reg))
    # An unconditional branch: the same target, from the new address.
    if (insn & 0xFC000000) == 0x14000000:
        out.append(encode_b(tramp_pc, site_pc + sext(insn & 0x03FFFFFF, 26) * 4))
        return out
    # A compare-and-branch or a test-and-branch: the condition's operands stay where they
    # are and only the condition flips, so the inverse test can skip over a branch that
    # goes the whole distance.
    if (insn & 0x7E000000) == 0x36000000:              # tbz/tbnz: the bit number stays
        target = site_pc + sext((insn >> 5) & 0x3FFF, 14) * 4
        out.append((insn ^ 0x01000000) & ~(0x3FFF << 5) | (2 << 5))
        out.append(encode_b(tramp_pc + 4, target))
        return out + [resume(tramp_pc + 8)]
    if (insn & 0x7E000000) == 0x34000000:              # cbz/cbnz, 32- and 64-bit
        target = site_pc + sext((insn >> 5) & 0x7FFFF, 19) * 4
        out.append((insn ^ 0x01000000) & ~(0x7FFFF << 5) | (2 << 5))
        out.append(encode_b(tramp_pc + 4, target))
        return out + [resume(tramp_pc + 8)]
    # A conditional branch: the inverse condition, then the branch it would have taken.
    if (insn & 0xFF000010) == 0x54000000:
        target = site_pc + sext((insn >> 5) & 0x7FFFF, 19) * 4
        out.append(0x54000000 | (2 << 5) | ((insn & 0xF) ^ 1))
        out.append(encode_b(tramp_pc + 4, target))
        return out + [resume(tramp_pc + 8)]
    # The addresses an adrp, an adr or a literal load names. The link address of the
    # target is what the *linked* instruction already points at, so re-encoding from the
    # trampoline to that same link address is the whole of the work -- an adrp names a
    # page its partner instruction completes, and the partner stays where it is.
    if is_adrp(insn):
        out.append(encode_adrp(insn & 0x1F, tramp_pc, adrp_target(site_pc, insn) & ~0xFFF))
    elif is_adr(insn):
        out.append(encode_adr(insn & 0x1F, tramp_pc, adr_target(site_pc, insn)))
    elif is_ldr_literal(insn):
        out.append(encode_ldr_literal(insn & 0x1F, tramp_pc, ldr_literal_target(site_pc, insn)))
    # Position-independent: copy it as it is.
    else:
        # The remaining pc-relative forms are a call, a signed or SIMD literal load and
        # a prefetch. Their displacement is relative to the instruction's own address,
        # so copying the word into the trampoline would make it name a different
        # address -- a call would call something unrelated. Refuse the site instead of
        # baking a trampoline that does the wrong thing. None of the 290 sites is one.
        if (insn & 0xFC000000) == 0x94000000:                     # bl
            raise Bail("hook site %#x: the displaced instruction is a call, which the "
                       "trampoline cannot re-encode" % site_pc)
        if (insn & 0xFF000000) in (0x98000000, 0x18000000, 0x1C000000, 0x3C000000):
            raise Bail("hook site %#x: the displaced instruction is a pc-relative load "
                       "(%08x), which the trampoline cannot re-encode" % (site_pc, insn))
        out.append(insn)
    return out + [resume(tramp_pc + 4 * len(out))]



def encode_pair_offset(insn, base_reg, off):
    """Re-encode the instruction after an `adrp` so that page + offset lands on the same
    byte at a new address. The two forms a compiler emits are `add Xd, Xn, #imm12 lsl #n`
    and `ldr/str [Xn, #imm12 * size]`; anything else is refused rather than guessed."""
    if ((insn >> 5) & 0x1F) != base_reg:
        raise ValueError("the next instruction does not use the adrp's register")
    if (insn & 0xFF800000) == 0x91000000:
        order = [((insn >> 22) & 3)] + [s for s in (0, 1, 2) if ((insn >> 22) & 3) != s]
        for shift in order:
            if off % (1 << shift) == 0 and (off >> shift) < 4096:
                return (insn & ~(0xFFF << 10)) | ((off >> shift) << 10) | (shift << 22)
        raise ValueError(f"offset {off:#x} does not fit a 12-bit add")
    if (insn & 0x3B000000) == 0x39000000:
        size = (insn >> 30) & 3
        if off % (1 << size):
            raise ValueError(f"offset {off:#x} is not a multiple of the access size "
                             f"{1 << size}")
        if (off >> size) >= 4096:
            raise ValueError(f"offset {off:#x} does not fit a 12-bit scaled immediate")
        return (insn & ~(0xFFF << 10)) | ((off >> size) << 10)
    raise ValueError(f"{insn:#010x} is not an add or ldr/str immediate form")


# ---------------------------------------------------------------- the patch pass

def patch_linked(path, e, lay, sv, stub_bytes, strict_refs=False, redirect=(),
                 lib_index=0):
    """Bake the load-time rewrites into the linked dylib.

    Why post-link: the rewrites are page-relative, and the linker decides where the two
    blobs land. Doing it here means we use its actual addresses instead of predicting them,
    and a slide cannot change the answer.
    """
    b, secs, syms = macho_sections(path)
    b = bytearray(b)                       # the patch pass edits the file in place
    code, data = secs["__guest_code"], secs["__guest_data"]
    stub_sec = secs["__guest_stubs"]
    for need in (CODE_LABEL, DATA_LABEL, STUB_LABEL, TCB_LABEL, VX18_LABEL,
                 X18FN_LABEL, TCBFN_LABEL):
        if need not in syms:
            raise Bail(f"the linked dylib has no symbol {need}")
    # The stub pool may sit anywhere: only branches we bake ourselves reach it. The two
    # blobs and the thread block are reached by the guest's own page-relative arithmetic,
    # so their addresses have to preserve the pages the ELF assumed.
    # The blob may start part-way into its section (the padding in front of it), so every
    # address and offset from here on is the label's, not the section's.
    blob = syms[CODE_LABEL]
    code = {"addr": blob, "size": secs["__guest_code"]["size"],
            "offset": secs["__guest_code"]["offset"] + (blob - secs["__guest_code"]["addr"])}
    for need in (CODE_LABEL, DATA_LABEL, TCB_LABEL):
        if syms[need] & 0xFFF:
            # The section table says what the linker put in front of it (names, addresses
            # and sizes only: safe to print in a public build log).
            table = "; ".join(f"{name} {s['addr']:#x}+{s['size']:#x}"
                              for name, s in sorted(secs.items(), key=lambda kv: kv[1]["addr"]))
            raise Bail(f"{need} landed at {syms[need]:#x}, not page-aligned; the guest's "
                       "page-relative adrp arithmetic would be wrong for every site in "
                       f"the first page. Force the section alignment. Sections: {table}")
    # The one property everything else rests on: the data sits at the same distance from
    # the code as it did in the ELF. If the linker put it anywhere else, every adrp in the
    # guest would be pointing at the wrong page, and nothing else would notice until the
    # game dereferenced one.
    got = data["addr"] - code["addr"]
    if lay["preserve"] and got != lay["delta"]:
        raise Bail(f"__DATA landed {got:#x} after the code, not the {lay['delta']:#x} the "
                   f"guest's addresses assume (code at {code['addr']:#x}, data at "
                   f"{data['addr']:#x}): every PC-relative reference into the data would "
                   "be wrong")
    stubs = {"addr": syms[STUB_LABEL], "size": stub_bytes,
             "base": syms[STUB_LABEL]}
    # The hook blocks are the last thing in the pool, one per baked hook: a dispatch stub
    # and the trampoline after it. Nothing else writes there, and the passes below stop
    # before the first of them, so the pool's size does not have to be guessed.
    hook_space = len(sv["hook_sites"]) * (HOOK_STUB_BYTES + HOOK_TRAMP_MAX)
    hook_base = stubs["base"] + stub_bytes - hook_space
    tcb_addr = syms[TCB_LABEL]
    vx18_addr = syms[VX18_LABEL]
    x18fn = syms[X18FN_LABEL]
    tcbfn = syms[TCBFN_LABEL]
    stubs_used = []

    def map_addr(v):
        """A guest address as the address it will have in the linked dylib."""
        if lay["code_base"] <= v < lay["code_base"] + len(lay["code"]):
            return code["addr"] + (v - lay["code_base"])
        if lay["data_base"] <= v < lay["data_base"] + len(lay["data"]):
            return data["addr"] + (v - lay["data_base"])
        raise Bail(f"address {v:#x} is in neither blob, so it cannot be mapped")

    def blob_off(addr):
        """The file offset of a link-time address inside one of our sections."""
        if code["addr"] <= addr < code["addr"] + code["size"]:
            return code["offset"] + (addr - code["addr"])
        if data["addr"] <= addr < data["addr"] + data["size"]:
            return data["offset"] + (addr - data["addr"])
        if stub_sec["addr"] <= addr < stub_sec["addr"] + stub_sec["size"]:
            return stub_sec["offset"] + (addr - stub_sec["addr"])
        raise Bail(f"{addr:#x} is outside the blobs")
    patches = []
    redirected = []

    def put(off_in_blob, word, what, vaddr=None, page=0, pair=0):
        struct.pack_into("<I", b, off_in_blob, word & 0xFFFFFFFF)
        patches.append({"vaddr": vaddr, "file_off": off_in_blob, "word": word & 0xFFFFFFFF,
                        "kind": what, "page": page, "pair": pair})

    # `mrs Xt, tpidr_el0` -> `b <stub that asks the shim for this thread's block>`.
    #
    # This used to become `adrp Xt, <the page in the data blob>`, which is page-relative and
    # so slide-invariant -- cheaper, and wrong for a reason that only shows up when the game
    # starts threads. The guest writes through the block, so a page in the image is a page
    # all of them share. The stub is six instructions and it buys one block per thread.
    tcb_stub_at = stubs["addr"]
    tcb_stub_cap = hook_base
    for v in sv["tpidr"]:
        pc = code["addr"] + (v - lay["code_base"])
        off = code["offset"] + (v - lay["code_base"])
        rt = u32(b, off) & 0x1F
        words = tpidr_stub(tcb_stub_at, pc, rt,
                           {"fn": tcbfn, "x18fn": x18fn})
        if tcb_stub_at + 4 * len(words) > tcb_stub_cap:
            raise Bail(f"the stub pool is too small for the tpidr sites: "
                       f"{len(sv['tpidr'])} sites need more than the reserved "
                       f"{stub_bytes:#x} bytes (raise --stub-bytes)")
        for n, w in enumerate(words):
            struct.pack_into("<I", b, blob_off(tcb_stub_at + 4 * n), w)
        branch = encode_b(pc, tcb_stub_at)
        struct.pack_into("<I", b, off, branch)
        put(off, branch, "tpidr->stub", v)
        stubs_used.append({"vaddr": v, "stub": tcb_stub_at, "words": len(words),
                           "kind": "tpidr-stub"})
        tcb_stub_at += 4 * len(words)
    stubs["addr"] = tcb_stub_at

    if not lay["preserve"]:
        # The sites the rewrite above has already claimed: a site that is now a branch
        # must not be touched again. The instruction after an adrp is re-encoded here --
        # that is the add that completes the page pair -- and an `mrs` can be that
        # instruction, because the compiler's thread-pointer sequence often uses the same
        # register for both. Re-encoding it turns the branch back into an arithmetic
        # instruction, and the guest then reads the raw hardware register: a value of
        # 0x102c, where a thread pointer belongs.
        # PC-relative references into the data blob, whose address moved. An adrp names a page
        # and the next instruction adds an offset, so moving the data means moving *both*:
        # the page moves to whatever page the new address falls in, and the offset becomes
        # the remainder. Keeping the offset and only moving the page would work only when
        # the two happened to stay page-aligned with each other, which is not generally so.
        for kind, v, target, pair_off, how in sv["code_refs"]:
            pc = code["addr"] + (v - lay["code_base"])
            off = code["offset"] + (v - lay["code_base"])
            old = u32(b, off)
            moved = data["addr"] + (target - lay["data_base"])
            if kind != "adrp":
                try:
                    new = (encode_adr if kind == "adr" else encode_ldr_literal)(old & 0x1F, pc, moved)
                except ValueError as ex:
                    raise Bail(f"{kind} at {v:#x}: {ex}")
                put(off, new, f"{kind}->data", v, moved & ~0xFFF, 0)
                continue
            new_page = moved & ~0xFFF
            new_off = moved - new_page
            try:
                new = encode_adrp(old & 0x1F, pc, new_page)
            except ValueError as ex:
                raise Bail(f"adrp at {v:#x} ({how}): {ex}")
            pair_word = 0
            if how == "pair":
                old_pair = u32(b, off + 4)
                try:
                    pair_word = encode_pair_offset(old_pair, old & 0x1F, new_off)
                except ValueError as ex:
                    raise Bail(f"the instruction after adrp at {v:#x}: {ex}")
                struct.pack_into("<I", b, off + 4, pair_word)
                patches.append({"vaddr": v + 4, "file_off": off + 4, "word": pair_word,
                                "kind": "adrp-pair-offset", "page": new_page, "pair": 0})
            put(off, new, "adrp->data" + ("+pair" if how == "pair" else ""), v, new_page, pair_word)

    # Otherwise there is deliberately no pass over the PC-relative references into the
    # data: layout() put __DATA at exactly code + (data_base - code_base), so every guest
    # address moved by the same amount and every adrp/add pair, however far apart the two
    # instructions are, still computes the address it computed in the ELF. Re-encoding them
    # would be the only thing in the conversion that could quietly break one.

    # A redirected symbol the guest defines itself: its entry becomes a branch to the
    # forwarder above. The shim, reached through it, records the request instead of
    # storing into code the linker has already signed.
    for name in redirect:
        fwd = syms.get(hook_label(name))
        if not fwd:
            continue
        for sname, sval, _sz, _ty, sshndx in e.symbols():
            if sname != name or not sshndx:
                continue
            if not (lay["code_base"] <= sval < lay["code_base"] + len(lay["code"])):
                continue
            pc = code["addr"] + (sval - lay["code_base"])
            off = code["offset"] + (sval - lay["code_base"])
            put(off, encode_b(pc, fwd), f"{name}->shim", sval)
            redirected.append((sval, name))

    # ctr_el0 is privileged here: the guest gets the constant in a stub
    for v in sv["ctr"]:
        off = code["offset"] + (v - lay["code_base"])
        pc = code["addr"] + (v - lay["code_base"])
        words = const_stub(stubs["addr"], pc, u32(b, off) & 0x1F, CTR_EL0_VALUE)
        if stubs["addr"] + 4 * len(words) > stubs["base"] + stub_bytes:
            raise Bail("the stub pool is too small for the ctr_el0 sites")
        for n, w in enumerate(words):
            struct.pack_into("<I", b, blob_off(stubs["addr"] + 4 * n), w)
        branch = encode_b(pc, stubs["addr"])
        struct.pack_into("<I", b, off, branch)
        put(off, branch, "ctr->stub", v)
        stubs_used.append({"vaddr": v, "stub": stubs["addr"], "words": len(words),
                           "kind": "ctr-stub"})
        stubs["addr"] += 4 * len(words)

    # x18: each site becomes a branch to a stub that works on a value in memory instead
    stub_at = stubs["addr"]
    stub_cap = hook_base
    for v in sv["x18_sites"]:
        off = code["offset"] + (v - lay["code_base"])
        pc = code["addr"] + (v - lay["code_base"])
        insn = u32(b, off)
        mapped = None
        unmappable = False
        if is_adrp(insn) or is_adr(insn):
            # the instruction still holds the ELF's encoding here, so decode it in the
            # guest's own coordinates and map the result
            tgt = adrp_target(v, insn) if is_adrp(insn) else adr_target(v, insn)
            try:
                mapped = map_addr(tgt)
            except Bail:
                unmappable = True
        elif is_ldr_literal_any(insn):
            try:
                mapped = map_addr(ldr_literal_target(v, insn))
            except Bail:
                unmappable = True
        if unmappable:
            # The word looks like an instruction using x18, but what it points at is not
            # in either blob, so there is no address to hand the stub: it is a constant
            # that decodes, and the loader leaves it alone too. Saying so here keeps the
            # count honest instead of failing the whole library over one word.
            sv["x18_unknown"].append(v)
            continue
        words = x18_stub(stub_at, pc, insn,
                         {"fn": x18fn, "lib": lib_index}, mapped)
        if stub_at + 4 * len(words) > stub_cap:
            raise Bail(f"the stub pool is too small for the x18 sites: {len(sv['x18_sites'])} "
                       f"sites need more than the reserved {stub_bytes:#x} bytes "
                       "(raise --stub-bytes)")
        for n, w in enumerate(words):
            struct.pack_into("<I", b, blob_off(stub_at + 4 * n), w)
        branch = encode_b(pc, stub_at)
        struct.pack_into("<I", b, off, branch)
        put(off, branch, "x18->stub", v)
        stubs_used.append({"vaddr": v, "stub": stub_at, "words": len(words),
                           "kind": "x18-stub"})
        stub_at += 4 * len(words)

    # The mod's hooks. At each site, the branch the mod would have written: `b stub`. The
    # stub reads the replacement's address out of a slot in this library's own data, and
    # falls back to the trampoline -- the instruction the site displaced, put back -- while
    # the mod has not asked for the hook yet. The replacement is in another library, whose
    # address is not known until everything is loaded, so only the indirection is baked;
    # everything else is a link-time constant and survives the slide.
    hooks_baked = []
    hook_at = hook_base
    if stubs["addr"] > hook_base:
        raise Bail(f"the stub pool is too small for the hooks: the other stubs reach "
                   f"{stubs['addr']:#x}, past the {hook_base:#x} the hook blocks start at "
                   "(raise --stub-bytes)")
    for i, v in enumerate(sv["hook_sites"]):
        if not (lay["code_base"] <= v < lay["code_base"] + len(lay["code"])):
            raise Bail(f"the hook site {v:#x} is not in the code image")
        pc = code["addr"] + (v - lay["code_base"])
        off = code["offset"] + (v - lay["code_base"])
        # The instruction the trampoline has to put back. It has to be the one the site
        # holds now, because a pass above may have rewritten the site -- and if it did,
        # the branch baked here would drop that rewrite, so refuse instead of guessing.
        insn = u32(b, off)
        if insn != u32(e.bytes_at(v, 4), 0):
            raise Bail(f"the hook site {v:#x} was already rewritten ({insn:#010x}); its "
                       "trampoline would copy the wrong instruction")
        slot_addr = data["addr"] + (lay["hook_slots"][i] - lay["data_base"])
        tramp_addr = hook_at + HOOK_STUB_BYTES
        tramp = hook_trampoline(tramp_addr, pc, insn)
        if 4 * len(tramp) > HOOK_TRAMP_MAX:
            raise Bail(f"the trampoline for the hook at {v:#x} needs more than the "
                       f"{HOOK_TRAMP_MAX} bytes reserved for it")
        for n, w in enumerate(hook_stub(hook_at, slot_addr, tramp_addr) + tramp):
            struct.pack_into("<I", b, blob_off(hook_at + 4 * n), w)
        branch = encode_b(pc, hook_at)
        struct.pack_into("<I", b, off, branch)
        put(off, branch, "hook->stub", v)
        stubs_used.append({"vaddr": v, "stub": hook_at, "slot": slot_addr,
                           "tramp": tramp_addr,
                           "words": len(hook_stub(hook_at, slot_addr, tramp_addr)) + len(tramp),
                           "kind": "hook-stub"})
        hooks_baked.append({"site": v, "stub": hook_at, "slot": slot_addr,
                            "tramp": tramp_addr, "insn": insn})
        hook_at += HOOK_STUB_BYTES + HOOK_TRAMP_MAX

    if os.environ.get("AOT_TRACE_PATCH"):
        for pt in patches[:40]:
            print("  patch %s vaddr %#x off %#x -> file now %#010x" % (
                pt["kind"], pt["vaddr"], pt["file_off"], u32(b, pt["file_off"])),
                file=sys.stderr)
    open(path, "wb").write(bytes(b))
    return patches, {"code": code, "data": data, "stubs": stubs, "tcb_addr": tcb_addr,
                     "redirected": redirected, "hooks": hooks_baked,
                     "vx18_addr": vx18_addr, "x18_fn": x18fn, "x18_stubs": stubs_used,
                     "tcb_file_off": data["offset"] + (tcb_addr - data["addr"])}


def verify_references(path, e, lay, info):
    """Every PC-relative reference left in the linked code must resolve to an address
    inside the image -- either blob. This is the check that catches a reference the
    survey mis-classified: nothing else would notice until the guest jumped into the
    weeds at run time."""
    b, secs, _ = macho_sections(path)
    code, data = secs["__guest_code"], secs["__guest_data"]
    stub_sec = secs["__guest_stubs"]
    cb, ce = code["addr"], code["addr"] + len(lay["code"])
    db, de = data["addr"], data["addr"] + len(lay["data"])
    # the thread block is part of the image the guest can address
    ranges = ((cb, ce), (db, de), (info["tcb_addr"] & ~0xFFF, info["tcb_addr"] + PAGE))
    bad = []
    # the ELF's own image, in guest coordinates: a word that already pointed outside
    # there was never one of ours to fix
    ecb, ece = lay["code_base"], lay["code_base"] + len(lay["code"])
    edb, ede = lay["data_base"], lay["data_base"] + info.get("data_real", len(lay["data"]))
    eranges = ((ecb, ece), (edb, ede))

    def effective(word, pc, second):
        if is_adrp(word):
            off = pair_offset(second, word & 0x1F)
            return adrp_target(pc, word) + (off or 0)
        if is_adr(word):
            return adr_target(pc, word)
        if is_ldr_literal(word):
            return ldr_literal_target(pc, word)
        return None

    # only the ranges the loader itself treats as code: the blob also holds the ELF
    # header, literal pools and relocations, whose words decode as nonsense
    for lo, hi in e.text_ranges():
        i = cb + (lo - lay["code_base"])
        stop = cb + (hi - lay["code_base"])
        while i + 8 <= stop:
            insn = u32(b, code["offset"] + (i - cb))
            nxt = u32(b, code["offset"] + (i - cb) + 4)
            eff = effective(insn, i, nxt)
            if eff is not None and not any(lo2 <= eff < hi2 for lo2, hi2 in ranges):
                # Outside now. That is only a fault if the word is the guest's own and
                # used to land inside its image: either we rewrote it, in which case the
                # new target is the one to judge, or we did not and the guest still has
                # to be able to reach what it pointed at.
                gvaddr = lay["code_base"] + (i - cb)
                elf_word = struct.unpack_from("<I", bytes(e.bytes_at(gvaddr, 4)))[0]
                if insn == elf_word:
                    was = effective(elf_word, gvaddr,
                                    struct.unpack_from("<I", bytes(e.bytes_at(gvaddr + 4, 4)))[0])
                    if was is not None and any(lo2 <= was < hi2 for lo2, hi2 in eranges):
                        bad.append((i, eff))
            i += 4
    if bad:
        where = ", ".join(f"{v:#x}->{t:#x}" for v, t in bad[:8])
        raise Bail(f"{len(bad)} PC-relative reference(s) in the linked image resolve outside "
                   f"both blobs ({where}); they would jump into unmapped memory at run time")
    print(f"  self-check: every PC-relative reference resolves inside the image "
          f"({len(lay['code']):,} bytes of code)")


def verify_against_elf(path, e, lay, patches, info):
    """The strongest check available without running anything: the linked code must be
    byte-identical to the ELF's, except for the words we baked. Any other difference means
    the linker or the emitter moved something, which would corrupt the guest silently."""
    b, _secs, _ = macho_sections(path)
    # the blob's own offset, which may be past the start of its section (see patch_linked)
    off = info["code"]["offset"]
    n = len(lay["code"])
    want = {(p["file_off"] - off): p["word"] for p in patches}
    bad = []
    for i in range(0, n, 4):
        got = struct.unpack_from("<I", b, off + i)[0]
        ref = struct.unpack_from("<I", e.b, lay["code_base"] + i) if False else None
        ref_word = struct.unpack_from("<I", bytes(e.bytes_at(lay["code_base"] + i, 4)))[0]
        if got != ref_word:
            if want.get(i) == got:
                continue
            bad.append((i, ref_word, got))
    if bad:
        where = ", ".join(f"+{i:#x} elf {r:#010x}->dylib {g:#010x}" for i, r, g in bad[:8])
        raise Bail(f"the linked code differs from the ELF in {len(bad)} unexpected word(s), "
                   f"none of them baked: {where}")
    print(f"  self-check: {len(want)} word(s) rewritten, every other byte identical to the ELF")


# ---------------------------------------------------------------- driver

def convert(elf_path, outdir, stub_bytes, allow_unimplemented, target=MACOS_TARGET,
            strict_x18=False, strict_refs=False, shims=(), cxx=True, verbose=True,
            mod_init=False, code_gap=0, redirect=DEFAULT_REDIRECT, lib_index=0,
            hooks=()):
    e = Elf(elf_path)
    # The sites of the mod's hooks, one entry each, deduplicated and in a fixed order so
    # that the slot array, the stub blocks and the facts file all come out the same for the
    # same input. A site is a guest vaddr in this library's code: everything else the bake
    # needs -- the displaced instruction, the trampoline -- is read out of the ELF here and
    # out of the linked image afterwards.
    hooks = sorted(set(hooks))
    lay = layout(e, len(hooks))
    sv = survey(e, lay, redirect)
    sv["hook_sites"] = hooks
    todo = []
    if sv["unknown_refs"] and strict_refs:
        todo.append(f"{len(sv['unknown_refs'])} pc-relative reference(s) that resolve "
                    "outside the image (first at "
                    f"{sv['unknown_refs'][0][0]:#x})")
    if sv["x18_unknown"] and strict_x18:
        todo.append(f"{len(sv['x18_unknown'])} x18 site(s) whose instruction shape is not "
                    f"one we know how to rewrite (first at {sv['x18_unknown'][0]:#x})")
    if sv["svc"]:
        todo.append(f"{len(sv['svc'])} svc sites")
    if stub_bytes == 0:
        # one stub per x18 site, plus room for anything the literal-slot table wants, plus
        # the hook blocks, which are the last thing in the pool
        stub_bytes = max(0x1000, (len(sv["x18_sites"]) * X18_STUB_MAX
                          + len(sv["tpidr"]) * TCB_STUB_MAX
                          + len(sv["ctr"]) * 64
                          + len(hooks) * (HOOK_STUB_BYTES + HOOK_TRAMP_MAX)
                          + 0xFFF) & ~0xFFF)
    else:
        stub_bytes = (stub_bytes + len(hooks) * (HOOK_STUB_BYTES + HOOK_TRAMP_MAX)
                      + 0xFFF) & ~0xFFF
    if todo and not allow_unimplemented:
        raise Bail("this library needs rewrites the converter does not implement yet:\n  - "
                   + "\n  - ".join(todo) + "\n  (--allow-unimplemented proceeds anyway; "
                   "those sites will be wrong)")
    if verbose:
        print(f"{os.path.basename(elf_path)} [{target}]: code {len(lay['code']):,} B, "
              f"data {len(lay['data']):,} B, {len(sv['tpidr'])} tpidr, "
              f"{len(sv['code_refs'])} code->data refs, {len(sv['relocs'])} rebases, "
              f"{len(sv['binds'])} binds, {sv.get('constructors', 0)} constructors, "
              f"{len(sv['x18_sites'])} x18, "
              f"{len(hooks)} baked hook(s), "
              f"{stub_bytes // 1024} KiB of stubs")
        for t in todo:
            print(f"  NOT IMPLEMENTED: {t}")
        if sv["unknown_refs"]:
            print(f"  {len(sv['unknown_refs'])} pc-relative site(s) resolve outside the "
                  "image and are left alone, as the loader leaves them (constants that "
                  "decode as instructions, most likely)")
        if sv["x18_unknown"]:
            # Words that match x18 on a bit field but whose shape we cannot decode. The
            # translation layer leaves these alone and counts them; so do we, and say where
            # they are, because most of them are constants in a table rather than
            # instructions -- a word the data map could not cluster on its own.
            print(f"  {len(sv['x18_unknown'])} word(s) look like x18 instructions but are "
                  f"shapes we cannot decode, left alone as the loader does: "
                  f"{', '.join(hex(v) for v in sv['x18_unknown'][:6])}"
                  + (" ..." if len(sv['x18_unknown']) > 6 else ""))

    dylib = os.path.join(outdir, e.path.rsplit("/", 1)[-1].replace(".so", ".dylib"))
    # No export filter: the labels and the guest's own entry points are already defined
    # (the latter as expressions over the code blob), so ld64 exports exactly the set of
    # names this library defines. That matters for more than tidiness -- libPVZ2.so calls
    # EA::Nimble::getEnv, which lives inside libNimble's blob, and the only way one
    # converted library can bind it is if its sibling exports it.
    symbols = []

    symmap = sorted({(val, name) for name, val, size, typ, shndx in e.symbols()
                     if shndx and name and size and val >= lay["code_base"]})
    # clang++, not clang: the guest's imports include the C++ runtime (__cxa_throw,
    # dynamic_cast, gxx_personality_v0), which a C link does not bring in. The input is
    # assembly, so the driver only decides which runtime to link.
    driver = ["xcrun", "clang++"] if cxx else ["xcrun", "clang"]
    cmd = driver + ["-target", target, "-dynamiclib", "-o", dylib,
                    "-install_name", f"@rpath/{os.path.basename(dylib)}"]
    # The converted libraries sit side by side and depend on each other by @rpath, so
    # each one has to be able to find its siblings: @loader_path is the directory the
    # dylib itself is in, wherever the bundle puts it.
    cmd += ["-Wl,-rpath,@loader_path", "-Wl,-segaddr,__TEXT,0"] + symbols
    # The SDK has to match the target: a dylib linked against the macOS SDK carries a
    # macOS load command and dyld on the device will not load it at all, and -lz would
    # come from the wrong platform's stub library.
    sdkname = sdk_for_target(target)
    if sdkname:
        sdk = subprocess.run(["xcrun", "--sdk", sdkname, "--show-sdk-path"],
                             capture_output=True, text=True)
        if sdk.returncode or not sdk.stdout.strip():
            raise Bail(f"no SDK for target {target} (xcrun --sdk {sdkname}): "
                       f"{(sdk.stderr or '').strip()}")
        cmd += ["-isysroot", sdk.stdout.strip()]
    # Imports the guest needs are satisfied by the other converted libraries (the game
    # library provides the hook API its engine libraries call, for instance) and by the
    # shim that stands in for bionic and the vendor runtimes. They are ordinary dynamic
    # links: ld64 writes the binds, dyld resolves them at load.
    for shim in shims:
        # a shim is either a dylib we built, or a system library to link as -lz/-lm:
        # the SDK ships .tbd stubs rather than the dylibs themselves
        cmd += [shim] if shim.startswith("-l") else [os.path.abspath(shim)]
    # Where the data goes is not the linker's choice: __DATA has to land at the same
    # distance from the code as it did in the ELF, so the guest's page-relative arithmetic
    # still describes its own image. Where the *code* lands depends on the stub pool, which
    # is only known once the linker has placed it, so: link, look, and if the distance is
    # not what it must be, say so and link again with the address the linker chose.
    # Segment addresses: the data goes at code + the guest's own distance between them,
    # and both have to come out 16 KiB aligned, because this platform maps in 16 KiB pages
    # and dyld will not map an image whose segments are not. The code's address is the
    # linker's choice until we pad in front of the blob to move it; the stub segment goes
    # after the data and only needs to be somewhere the baked branches can reach.
    gap = 0
    data_seg = 0x4000000          # the first pass only has to measure where the code lands
    stub_seg = 0x8000000
    if not lay["preserve"]:
        stub_seg = ((len(lay["code"]) + len(lay["data"]) + 0x20000) + 0x3FFF) & ~0x3FFF
    for attempt in (1, 2, 3):
        asm = emit_asm(e, lay, sv, outdir, stub_bytes, mod_init, gap, redirect,
                   lib_index)
        # __DATA's address is ours only when the layout is preserved; otherwise the
        # references into it were re-encoded and the linker may put it where it likes.
        seg_args = [f"-Wl,-segaddr,{STUB_SEGMENT},{stub_seg:#x}",
                    f"-Wl,-segprot,{STUB_SEGMENT},rx,rx",
                    # ld64 may put constant data (our absolute export symbols) in
                    # __DATA_CONST, which it would otherwise place inside __DATA. The
                    # segment has no say in the image we are building, so it is turned off.
                    "-Wl,-no_data_const"]
        if lay["preserve"]:
            seg_args.insert(0, f"-Wl,-segaddr,__DATA,{data_seg:#x}")
        r = subprocess.run(cmd + [asm] + seg_args,
                           cwd=outdir, capture_output=True, text=True)
        if r.returncode:
            break
        _b, secs, syms = macho_sections(dylib)
        # The blob's address, not its section's: the padding in front of it is what this
        # loop is adjusting, and the section's own address does not move.
        code_addr = syms.get(CODE_LABEL, secs["__guest_code"]["addr"])
        data_end = secs["__guest_data"]["addr"] + secs["__guest_data"]["size"]
        aligned = ((code_addr + lay["delta"]) & 0x3FFF) == 0
        # The data has to sit at the guest's own distance from the code. The first pass
        # only measures where the code lands -- the data is left at a placeholder address
        # -- so a pass that finds the code already aligned is not yet a pass that has
        # placed the data. Without this the loop could stop on a first pass whose code
        # happened to land 16 KiB-aligned, with the data still 64 MiB away.
        placed = (not lay["preserve"]) or (secs["__guest_data"]["addr"] - code_addr) == lay["delta"]
        if os.environ.get("AOT_TRACE_LINK"):
            print(f"  link pass {attempt}: code at {code_addr:#x}, data at "
                  f"{secs['__guest_data']['addr']:#x}, gap {gap:#x}, aligned {aligned}, "
                  f"placed {placed}", file=sys.stderr)
        if placed and (aligned or not lay["preserve"]) and stub_seg >= ((data_end + 0x3FFF) & ~0x3FFF):
            break
        if attempt < 3 and lay["preserve"]:
            # Move the code forward just far enough for the sum to be aligned. The blob's
            # own address does not have to be page-aligned: the guest's page arithmetic is
            # about its own coordinates and a uniform shift cancels out.
            gap += (-(code_addr + lay["delta"])) & 0x3FFF
            data_seg = ((code_addr + gap) + lay["delta"]) & ~0x3FFF
            stub_seg = (data_seg + len(lay["data"]) + 0x10000) & ~0x3FFF
    if r.returncode:
        missing = sorted({m for m in re.findall(r'"(_[A-Za-z0-9_]+)", referenced from:',
                                              r.stderr or "")})
        if missing:
            raise Bail(f"link failed: {len(missing)} imported symbol(s) are not defined "
                       "by anything this build links against. The game's libraries "
                       "provide some of them (convert those first and pass --shim), the "
                       "rest need the shim that stands in for bionic. First few: "
                       + ", ".join(missing[:8]))
        raise Bail("link failed:\n" + (r.stderr or r.stdout))

    patches, info = patch_linked(dylib, e, lay, sv, stub_bytes, strict_refs, redirect,
                               lib_index)
    r = subprocess.run(["codesign", "-s", "-", dylib], capture_output=True, text=True)
    if r.returncode:
        raise Bail("codesign failed:\n" + (r.stderr or r.stdout))

    verify_against_elf(dylib, e, lay, patches, info)
    verify_references(dylib, e, lay, info)

    manifest = {
        "target": target,
        "shims": list(shims),
        "elf": elf_path,
        "elf_sha256": hashlib.sha256(e.b).hexdigest(),
        "dylib": dylib,
        "dylib_sha256": hashlib.sha256(open(dylib, "rb").read()).hexdigest(),
        "labels": {"code": CODE_LABEL, "data": DATA_LABEL, "stubs": STUB_LABEL, "tcb": TCB_LABEL},
        "guest_code_base": lay["code_base"], "guest_data_base": lay["data_base"],
        "sections": info, "patches": patches,
        "relocs": [{"data_off": r_["off"] - lay["data_base"], "value": r_["value"],
                    "host": ("code" if lay["code_base"] <= r_["value"] < lay["code_base"] + len(lay["code"])
                             else "data"),
                    "host_off": (r_["value"] - lay["code_base"] if lay["code_base"] <= r_["value"] < lay["code_base"] + len(lay["code"])
                                 else r_["value"] - lay["data_base"])}
                   for r_ in sv["relocs"]],
        "binds": [{"data_off": b_["off"] - lay["data_base"], "sym": b_["sym"],
                   "addend": b_.get("addend", 0)} for b_ in sv["binds"]],
        "symbols": [{"vaddr": v, "name": n} for v, n in symmap],
        "x18_unrewritten": sv["x18_unknown"],
        "redirected": [{"vaddr": v, "name": n} for v, n in info.get("redirected", [])],
        "hooks": [{"site": h["site"], "stub": h["stub"], "slot": h["slot"],
                   "tramp": h["tramp"], "displaced": h["insn"]}
                  for h in info.get("hooks", [])],
        "constructors": sv.get("constructors", 0),
        "refs_outside_image": [{"vaddr": v, "target": t} for v, t in sv["unknown_refs"]],
        "not_implemented": todo,
    }
    mpath = os.path.join(outdir, os.path.basename(dylib) + ".manifest.json")
    open(mpath, "w").write(json.dumps(manifest, indent=1))

    # A flat, line-oriented copy of the same facts: JSON is fine for a record, but the
    # verifier that checks a *loaded* image should not need a parser (or get an
    # off-by-one from one).
    fpath = os.path.join(outdir, os.path.basename(dylib) + ".facts")
    with open(fpath, "w") as f:
        f.write(f"dylib {dylib}\n")
        f.write(f"code_link {info['code']['addr']:#x}\n")
        f.write(f"data_link {info['data']['addr']:#x}\n")
        f.write(f"tcb_link {info['tcb_addr']:#x}\n")
        f.write(f"stubs_link {info['stubs']['addr']:#x}\n")
        f.write(f"vx18_link {info['vx18_addr']:#x}\n")
        f.write(f"x18_fn_link {info['x18_fn']:#x}\n")
        f.write(f"guest_code_base {lay['code_base']:#x}\n")
        f.write(f"code_size {len(lay['code']):#x}\n")
        for pt in patches:
            f.write(f"patch {pt['vaddr']:#x} {pt['word']:#x} {pt['kind']} "
                    f"{pt.get('page', 0):#x} {pt.get('pair', 0):#x}\n")
        for r_ in manifest["relocs"]:
            f.write(f"rebase {r_['data_off']:#x} {r_['host']} {r_['host_off']:#x}\n")
        for b_ in manifest["binds"]:
            f.write(f"bind {b_['data_off']:#x} {b_['sym']}\n")
        for st in info.get("x18_stubs", []):
            f.write(f"stub {st['vaddr']:#x} {st['stub']:#x} {st['words']}\n")
        # The baked hooks: where the site's branch goes, where the stub reads the
        # replacement from, and where the original code is (the trampoline the shim hands
        # the mod). A line per hook, so a loaded image can be checked against the file.
        for h in info.get("hooks", []):
            f.write(f"hook {h['site']:#x} {h['stub']:#x} {h['slot']:#x} {h['tramp']:#x} "
                    f"{h['insn']:#010x}\n")
        # The constructors, in the order the guest's loader would run them. The runtime
        # calls these from tl_ld_init, which is after the JNI environment exists.
        for val in sv.get("init_entries", []):
            f.write(f"init {val:#x}\n")
        for val, name in symmap:
            f.write(f"sym {val:#x} {name}\n")
    if verbose:
        print(f"  -> {dylib}  ({os.path.getsize(dylib):,} bytes, {len(patches)} baked patches)")
        print(f"  -> {mpath}")
        print(f"  -> {fpath}")
    return manifest


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("-o", "--outdir", default=None)
    ap.add_argument("--lib-index", type=int, default=0,
                    help="which of the guest's libraries this is, 0-based: the per-thread "
                         "word for the reserved register is per-library, and the stub that "
                         "reads it names this one as an immediate")
    ap.add_argument("--stub-bytes", type=lambda s: int(s, 0), default=0,
                    help="bytes reserved for stubs; 0 sizes them from the work found")
    ap.add_argument("--allow-unimplemented", action="store_true")
    ap.add_argument("--no-cxx", dest="cxx", action="store_false",
                    help="link with clang instead of clang++, for a library that imports "
                         "nothing from the C++ runtime")
    ap.add_argument("--shim", action="append", default=[], metavar="DYLIB|-lNAME",
                    help="something that defines this library's imports: another "
                         "converted library, the bionic shim, or a system library as "
                         "-lz; repeatable")
    ap.add_argument("--strict-refs", action="store_true",
                    help="refuse when a pc-relative site resolves outside the image, "
                         "instead of leaving it alone the way the loader does")
    ap.add_argument("--strict-x18", action="store_true",
                    help="refuse when a word looks like an x18 instruction of a shape the "
                         "converter cannot decode, instead of leaving it alone")
    ap.add_argument("--target", default=os.environ.get("AOT_LINK_TARGET", MACOS_TARGET),
                    help="clang target triple, e.g. arm64-apple-ios17.0 for a device build")
    ap.add_argument("--hook-site", action="append", default=[], type=lambda s: int(s, 0),
                    metavar="VADDR",
                    help="bake the mod's hook at this guest vaddr: the site gets a branch "
                         "to a stub that reads the replacement out of a slot in this "
                         "library's data, and falls back to a trampoline until the shim "
                         "writes one in; repeatable")
    ap.add_argument("--hooks", metavar="FILE",
                    help="a hook table to bake every site of: either a JSON list of guest "
                         "vaddrs, or the mod's own table, whose entries carry them as "
                         '"site" (the frozen hookset.json is one of those)')
    a = ap.parse_args(argv[1:])
    hooks = list(a.hook_site)
    if a.hooks:
        table = json.load(open(a.hooks))
        entries = table.get("hooks", []) if isinstance(table, dict) else table
        for entry in entries:
            hooks.append(entry["site"] if isinstance(entry, dict) else int(entry))
    outdir = a.outdir or os.path.join(os.path.dirname(os.path.abspath(a.elf)) or ".",
                                      "aot-out")
    # The link runs with cwd=outdir, and the assembly it is given is a path that already
    # names the output directory. A relative out-dir would therefore be looked for twice
    # over -- "out/out/guest.s" -- and the link would fail with a file that is not there.
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)
    try:
        convert(a.elf, outdir, a.stub_bytes, a.allow_unimplemented, a.target, a.strict_x18,
                a.strict_refs, a.shim, a.cxx, redirect=DEFAULT_REDIRECT,
                lib_index=a.lib_index, hooks=hooks)
    except Bail as ex:
        print(f"refusing: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))