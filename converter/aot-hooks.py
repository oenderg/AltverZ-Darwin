#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
aot-hooks.py -- turn a codewrite log into the hook set an ahead-of-time port must freeze.

The mod (libRFLib, And64InlineHook-style) patches function prologues at start-up. Under
Husk those stores fault and the fault handler performs them through the writable view
(husk-tl-codewrite.c); with TL_CODEWRITE_LOG=<path> every one is recorded. A JIT-free
port cannot perform them at runtime, so it needs to know, ahead of time, the *final*
content of every address the mod writes -- the last value written wins, because that is
what the code would have seen when it starts.

    aot-hooks.py <log> [--json]            summarise one log
    aot-hooks.py <logA> <logB>             also diff two runs (determinism check)

Addresses in the log are offsets from the translation layer's region base, which is laid
out identically for a given set of libraries, so two runs of the same build produce the
same offsets.
"""
import json
import sys
from collections import Counter, defaultdict


def parse(path):
    """-> (final value per address, arena writes, stats)"""
    final, arena_writes, writers = {}, [], set()
    n = 0
    for line in open(path):
        if line.startswith("exec-range"):
            continue
        if not line.startswith("patch"):
            continue
        d = dict(kv.split("=", 1) for kv in line.split()[1:])
        off, size, val, pc = int(d["off"], 16), int(d["n"]), int(d["val"], 16), int(d["pc"], 16)
        n += 1
        writers.add(pc)
        if size == 4:
            final[off] = val                      # the last store is what the code sees
        else:
            arena_writes.append((off, size, val))  # trampolines: not addressable offline
    return final, arena_writes, {"stores": n, "writers": len(writers)}


def branch_target(insn, at):
    """B (and BL) imm26, and the ADR form, which is how some hooks are written."""
    if (insn >> 26) == 0b000101:
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000:
            imm -= 0x04000000
        return at + imm * 4
    if (insn >> 24) == 0x1E:                       # ADR Xd, label
        imm = insn & 0x00FFFFFF
        if imm & 0x01000000:
            imm -= 0x02000000
        return at + imm
    return None


def summarise(path, final, arena, stats):
    kinds = Counter()
    targets = []
    for off, val in sorted(final.items()):
        if (val >> 26) == 0b000101:
            kinds["B"] += 1
            targets.append((off, val, branch_target(val, off)))
        elif (val >> 26) == 0b100101:
            kinds["BL"] += 1
            targets.append((off, val, branch_target(val, off)))
        elif val == 0:
            kinds["zero"] += 1
        else:
            kinds[f"other(0x{val:08x})"] += 1
    print(f"{path}")
    print(f"  stores logged            {stats['stores']:,}")
    print(f"  distinct faulting PCs    {stats['writers']:,}   (the mod's patch writers)")
    print(f"  distinct 4-byte targets  {len(final):,}   {dict(kinds)}")
    print(f"  non-word stores (trampolines in the arena): {len(arena):,}")
    hooks = [t for t in targets if t[2] is not None]
    if hooks:
        tgts = Counter(t[2] for t in hooks)
        print(f"  hooks with a decodable target: {len(hooks):,}, "
              f"{len(tgts):,} distinct replacement functions, "
              f"largest fan-in {max(tgts.values())}")
    return {off: val for off, val in final.items()}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    results = []
    for p in args:
        final, arena, stats = parse(p)
        results.append((p, summarise(p, final, arena, stats)))
    if len(args) == 2:
        (pa, a), (pb, b) = results
        only_a = sorted(set(a) - set(b))
        only_b = sorted(set(b) - set(a))
        changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
        print()
        print(f"comparing {pa} and {pb}:")
        print(f"  addresses only in the first  {len(only_a)}")
        print(f"  addresses only in the second {len(only_b)}")
        print(f"  same address, different value {len(changed)}")
        if only_a[:5]:
            print(f"    e.g. first only: {[hex(x) for x in only_a[:5]]}")
        if only_b[:5]:
            print(f"    e.g. second only: {[hex(x) for x in only_b[:5]]}")
        print("  VERDICT:", "deterministic" if not (only_a or only_b or changed)
              else "NOT deterministic -- the hook set depends on the run")
    if "--json" in argv:
        print(json.dumps([{hex(k): v for k, v in r[1].items()} for r in results], indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))