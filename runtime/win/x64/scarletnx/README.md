# ScarletNX, as the x86-64 build needs it

The x86-64 Windows build links ScarletNX's recompiler library
(https://github.com/scarletnx/ScarletNX, GPLv3) at commit `b0beb4f`, with `scarletnx.patch` applied:

```sh
git clone https://github.com/scarletnx/ScarletNX && cd ScarletNX
git checkout b0beb4f
git apply /path/to/runtime/win/x64/scarletnx/scarletnx.patch
```

What the patch adds to `src/common/cpu/recompiler/library.c` and `snx_recompiler.h`:

- **Live code** (`snx_recompiler_set_live_code`): the recompiler reads the guest's code where it is, not from a
  copy, so code the mod patches at run time (libRFLib's hooks) is compiled as it is then.
- **Every leader's slot is cleared on invalidation.** A translated function can be entered at several leaders,
  each with its own dispatch wrapper. `snx_recompiled_invalidate` cleared only the first leader's slot, so a call
  through another leader re-entered the retired function, its stale check returned with the pc unchanged, and the
  run loop spun forever: the game stopped at "Loading main RSB". The patch records every leader's entry and clears
  all of them.
- **Diagnostics** for the driver's sampler and log: `snx_recompiled_host_function`,
  `snx_recompiled_function_state`, `snx_recompiler_set_log`.

The leader fix is a ScarletNX bug and should go upstream; until then the patch lives here.
