# ScarletNX pull request draft

Not opened yet. Base: ScarletNX `f0873dfbb74c01ef5aa3c401f19b303a952f5f68`. Fix commit: `f26bfd5049f264ef7ac6880e09062641c22a41ec`
on the local branch `fix/invalidate-every-leader` in the Windows VM (`C:\x64\ScarletNX-upstream`), also in
`upstream-fix.patch` here. It follows ScarletNX's `CONTRIBUTING.md` and `.github/PULL_REQUEST_TEMPLATE.md`: plain prose,
no semicolons between sentences, no game files or screenshots.

## Title

[recompiler] Clear every leader's dispatch slot on invalidation

## Description

With dispatch hooks on (`snx_recompiler_set_hook_direct_calls(1)`), a call into code that `snx_recompiled_invalidate`
has retired can loop forever. `keep_invalidation_map` keeps only the wrapper of a function's first leader, and the
invalidation clears only the dispatch slots that point at that wrapper. The entry table holds one function for each
address (`entries()` in `library.c` keeps the lowest function index), so a leader address that two functions share has
the wrapper of only one of them. A slot that the clear misses can still lead into the retired function. Its stale check
sets the pc to the same entry and returns, and `snx_recompiled_run` calls it again, so the guest makes no progress.

This change keeps the entry of every leader of a function in one `leader_entries` block and clears all of their slots
when the function is retired. It adds no API.

I found it with PvZ2 Reflourished, an Android game, on an x86-64 Windows host that runs the game's ARM64 libraries
through the library. The game's mod library patches the game's code at start-up, and the host reports the change with
`snx_recompiled_invalidate`.

Testing: Windows 11 x86-64 in a VM, Clang and LLVM 23.1.5, on `f0873dfb`.
- `scarletnx-recompiler` builds, and `tests/recompiler_library.c` built against it passes (`recompiler library: ok`).
- The game, one run without the change and two with it. Without it, after the start-up patch at 8.5 s the guest stays
  at one pc in `JNI_OnLoad` with no host calls until the run ends at 65 s. With it, the game loads its data at 9.9 s,
  starts to draw at 21.6 s and shows its title screen. A local build that also logged the counts showed that 854 of
  2800 invalidations cleared leader slots that the old loop did not reach. For one function the old loop cleared 9
  slots and the new loop reached 13.

Not tested: the full `ctest` and `tests/run_filesystem_tree.py`, because their configure fetches lz4 and I did not
download it on that machine. Other hosts and architectures. There is no unit test yet. The case needs two functions
that share a leader address, and I can add one if you point me at a way to build such a module in
`tests/recompiler_library.c`.

- [x] No keys, dumps, game files or saves in the commits or the description.
- [x] A new file starts with `SPDX-License-Identifier: GPL-3.0-or-later` and the project's copyright line. (No new file.)
- [x] Code ported from another project keeps its copyright lines, names where it came from and is listed in `docs/CREDITS.md`. (No ported code.)

## Evidence kept here (not for the pull request)

Logs: `out/win-upstream-nofix.log` and `out/win-upstream-fix.log` (worktree `out/`, not committed). The title screenshot
`out/win-upstream-fix-title.png` stays local: ScarletNX keeps game material out of its pull requests.

```
without the change:
[  8.486] x64: code changed at 0x5a0000df4540, 25 translated function(s) retired
[ 65.471] x64 sample: cpu 0 depth 1 pc 0x5a0001845b20 (libPVZ2.so JNI_OnLoad) ...
(no "Loading main RSB" line, every sample reports 0 host calls)

with the change (counts from the local logging build, not in the commit):
[  8.693] invalidate: 0x5a00018a6bd4..0x5a000196aac0 (...): alias 9 leader 13
[  9.893] Loading main RSB ...
[ 21.598] pvz2: starting the draw loop
[ 22.758] egl: swap #1, actual surface 1360x663, guest viewport (0,0) 1360x663
```

Before opening: decide whether to run the full `ctest` (needs the lz4 download in the VM), fork ScarletNX, push
`fix/invalidate-every-leader` to the fork, and open the pull request with the title and description above.
