# [recompiler] clear every leader's dispatch slot on invalidation

## What happens

A translated function can be entered at more than one leader. When code under it changes,
`snx_recompiled_invalidate` retires the function and clears its dispatch table slots. Only the first leader's slots
are cleared today. A call through another leader enters the retired function. Its stale check writes the same pc back
to the CPU and returns, so `snx_recompiled_run` calls it again, and the guest stops making progress.

## Why

`keep_invalidation_map` in `src/common/cpu/recompiler/library.c` records only `first_entry` in
`SnxCompiledFunction`. `snx_recompiled_invalidate` clears the entries whose `entry_functions` value equals
`function->code`, which is the wrapper of that first leader. A leader that maps to another wrapper stays in the
dispatch table. The stale check (`stale_check` in `src/common/cpu/recompiler/llvm/ir_codegen.c`) sets `CPU.pc` to
the entry and returns. `snx_recompiled_run` sees the same pc and calls it again.

## Reproduce

- Build a game with the library and `snx_recompiler_set_hook_direct_calls(1)`.
- At start-up the game patches code and calls `snx_recompiled_invalidate` for the patched range.
- Without this change the guest stops after the invalidation. A sampler shows the same pc and `0 host calls` for
  every sample. On the x86-64 PvZ2 Reflourished host the pc is in `libPVZ2.so JNI_OnLoad`; older builds stop at
  `Loading main RSB`.
- With the change the same build passes that point.

## Fix

- `SnxCompiledFunction` keeps the entry index of every leader and their count.
- `keep_invalidation_map` puts every leader's entry in one `leader_entries` block.
- `snx_recompiled_invalidate` clears each recorded leader's dispatch slot after the alias loop.
- `snx_recompiled_free` frees the block.

No public API changes.

## Tests

- `cmake` and `cmake --build build/lib --target scarletnx-recompiler` with Clang 23.1.5 and LLVM 23.1.5: pass.
- `tests/recompiler_library.c` built against the archive and run: `recompiler library: ok`.
- A unit test for the failing case is not practical with the current test program: every leader of one codegen
  function maps to the same `F_` wrapper, so the alias loop already clears the simple two-leader case. The failure
  needs a module whose leaders have different wrappers, which the library's test API does not construct. The host is
  the regression.

## Local evidence for review

Remove this section when opening the pull request.

```
without the change:
[  8.486] x64: code changed at 0x5a0000df4540, 25 translated function(s) retired
[ 11.341] x64 sample: cpu 0 depth 1 pc 0x5a0001845b20 (libPVZ2.so JNI_OnLoad) ...
(no "Loading main RSB" line; every sample reports 0 host calls)

with the change:
[  8.693] invalidate: 0x5a00018a6bd4..0x5a000196aac0 (entry 0x5a00018a6bd4, code 0x7ff91e2915b0): alias 9 leader 13
[  8.694] invalidate: 0x5a00018b7e28..0x5a000196caf8 (entry 0x5a00018b7e28, code 0x7ff91e298950): alias 12 leader 19
[  9.893] android[D/PvZ2Debug]: Loading main RSB from the path /sdcard/Android/obb/com.ea.game.pvz2_rfl/main.675.com.ea.game.pvz2_rfl.obb
[ 21.299] pvz2: Native_onSurfaceChanged(1360,663)
[ 21.598] pvz2: starting the draw loop
[ 22.758] egl: swap #1, actual surface 1360x663, guest viewport (0,0) 1360x663
```

The `alias` and `leader` counts came from a local logging build. The pull request's commit does not contain
the counter logging: it would add output the library does not need. The counts show that the leader loop clears
more slots than the first wrapper's aliases, for example 13 slots where the aliases cover 9.

Title screenshot: `out/win-upstream-fix-title.png`.
Full logs: `out/win-upstream-nofix.log` and `out/win-upstream-fix.log`.

Prepared against ScarletNX `f0873dfbb74c01ef5aa3c401f19b303a952f5f68` (129 commits past `b0beb4f`).
Fix commit: `273823f3df81da5df83325beac28a92f69dfeb8a`.
