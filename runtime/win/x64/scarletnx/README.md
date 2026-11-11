# ScarletNX, as the x86-64 build needs it

The x86-64 Windows build links ScarletNX's recompiler library
(https://github.com/scarletnx/ScarletNX, GPLv3) at commit `f5c2fa84`, with `scarletnx.patch` applied:

```sh
runtime/win/x64/scarletnx/build-scarletnx.sh <folder>   # in MSYS2's CLANG64 shell
```

It fetches that one commit into `<folder>`, applies the patch, and builds the `scarletnx-recompiler`
target (Release, `SNX_TOOLS_ONLY=ON`, no tests or front ends) into `<folder>/build/lib`, where
`build-x64.sh` links it from. By hand:

```sh
git clone https://github.com/scarletnx/ScarletNX && cd ScarletNX
git checkout f5c2fa84
git apply /path/to/runtime/win/x64/scarletnx/scarletnx.patch
```

What the patch adds to `src/common/cpu/recompiler/library.c` and `snx_recompiler.h`:

- **Diagnostics** for the driver's sampler and log: `snx_recompiled_host_function`,
  `snx_recompiled_function_state`, `snx_recompiler_set_log`, and the dispatch-slot counts the invalidator writes
  to that log.
- **Profile** (`TL_X64_PROFILE`): the phase times (codegen, unit compile, descriptor, link, load, save) and each
  unit's function range, weight and seconds, through the driver's log callback.

Everything else the build needs from ScarletNX is upstream as of `f5c2fa84`, from these pull requests:

- https://github.com/scarletnx/ScarletNX/pull/1449: every leader's dispatch slot is cleared on invalidation.
- https://github.com/scarletnx/ScarletNX/pull/1689: the temp folder names the user by its own SID instead of the
  owner-rights SID (OW), which Wine cannot apply, and says so in the log when it falls back to OW.
- https://github.com/scarletnx/ScarletNX/pull/1706: the progress callback (`snx_recompiler_set_progress`) that
  the Windows preparing window uses for its bar and estimate.
- https://github.com/scarletnx/ScarletNX/pull/1708: the unit schedule for a cold translation, units weighted by
  their blocks (30000 on desktop) and started largest first.
- https://github.com/scarletnx/ScarletNX/pull/1707: live code (`snx_recompiler_set_live_code`), so code the mod
  patches at run time (libRFLib's hooks) is compiled as it is then.
