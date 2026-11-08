# ScarletNX, as the x86-64 build needs it

The x86-64 Windows build links ScarletNX's recompiler library
(https://github.com/scarletnx/ScarletNX, GPLv3) at commit `5b9c0fab`, with `scarletnx.patch` applied:

```sh
git clone https://github.com/scarletnx/ScarletNX && cd ScarletNX
git checkout 5b9c0fab
git apply /path/to/runtime/win/x64/scarletnx/scarletnx.patch
```

What the patch adds to `src/common/cpu/recompiler/library.c` and `snx_recompiler.h`:

- **Live code** (`snx_recompiler_set_live_code`): the recompiler reads the guest's code where it is, not from a
  copy, so code the mod patches at run time (libRFLib's hooks) is compiled as it is then.
- **Diagnostics** for the driver's sampler and log: `snx_recompiled_host_function`,
  `snx_recompiled_function_state`, `snx_recompiler_set_log`, and the dispatch-slot counts the invalidator writes
  to that log.

The leader fix that used to be in the patch is upstream as `5b9c0fab` ("Clear every leader's dispatch slot on
invalidation", https://github.com/scarletnx/ScarletNX/pull/1449, merged 2026-11-08), so the patch no longer
carries it. `upstream-fix.patch` and `upstream-pr.md` record that change and its evidence.
