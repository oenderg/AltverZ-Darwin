# ScarletNX, as the x86-64 build needs it

The x86-64 Windows build links ScarletNX's recompiler library
(https://github.com/scarletnx/ScarletNX, GPLv3) at commit `7950099a`, with `scarletnx.patch` applied:

```sh
runtime/win/x64/scarletnx/build-scarletnx.sh <folder>   # in MSYS2's CLANG64 shell
```

It fetches that one commit into `<folder>`, applies the patch, and builds the `scarletnx-recompiler`
target (Release, `SNX_TOOLS_ONLY=ON`, no tests or front ends) into `<folder>/build/lib`, where
`build-x64.sh` links it from. By hand:

```sh
git clone https://github.com/scarletnx/ScarletNX && cd ScarletNX
git checkout 7950099a
git apply /path/to/runtime/win/x64/scarletnx/scarletnx.patch
```

What the patch adds to `src/common/cpu/recompiler/library.c` and `snx_recompiler.h`:

- **Live code** (`snx_recompiler_set_live_code`): the recompiler reads the guest's code where it is, not from a
  copy, so code the mod patches at run time (libRFLib's hooks) is compiled as it is then. Proposed upstream:
  https://github.com/scarletnx/ScarletNX/pull/1707.
- **Diagnostics** for the driver's sampler and log: `snx_recompiled_host_function`,
  `snx_recompiled_function_state`, `snx_recompiler_set_log`, and the dispatch-slot counts the invalidator writes
  to that log.
- **Unit scheduling** for a cold translation: the unit weight is instructions plus four times the leaders (basic
  blocks), the code is split by that weight, and the units start largest first, so a long unit does not start
  last. The desktop default is 30000 weighted instructions; `TL_X64_CHUNK`, `TL_X64_UNIT_WEIGHT=instructions` and
  `TL_X64_UNIT_SORT=0` measure other values. Proposed upstream without those switches, with the cold-run
  numbers: https://github.com/scarletnx/ScarletNX/pull/1708.
- **Profile** (`TL_X64_PROFILE`): the phase times (codegen, unit compile, descriptor, link, load, save) and each
  unit's function range, weight and seconds, through the driver's log callback.
- **Progress** (`snx_recompiler_set_progress`): a callback with the units' weight and functions done and total,
  once with the totals and then after each unit finishes; the Windows preparing window uses it for its bar and
  estimate. Proposed upstream: https://github.com/scarletnx/ScarletNX/pull/1706.

The leader fix that used to be in the patch is upstream as `5b9c0fab` ("Clear every leader's dispatch slot on
invalidation", https://github.com/scarletnx/ScarletNX/pull/1449, merged 2026-11-08), so the patch no longer
carries it.

The temp folder fix for Wine (`snx_temp_dir` names the user by its own SID instead of the owner-rights SID, OW,
which Wine cannot apply, and says so in the log when it falls back to OW) is upstream as of `7950099a`
(https://github.com/scarletnx/ScarletNX/pull/1689, merged 2026-11-11), so the patch no longer carries it either.
