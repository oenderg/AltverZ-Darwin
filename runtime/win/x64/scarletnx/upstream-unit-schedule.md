# ScarletNX cold translation: schedule the code units by size, largest first

Draft for an upstream change; not sent to ScarletNX yet. The numbers are from the Windows x86-64
PvZ2 run in the VM (6 emulated cores), not from the 24-thread desktop PC that judges the final result.

## What

`chunks()` in `src/common/cpu/recompiler/library.c` splits the functions into units that compile on `jobs` threads.
The unit weight is now `instructions + 4 * leaders` (a leader is a basic block start), the split uses that weight,
and the units are queued largest first. The desktop default is 30000 weighted instructions. `TL_X64_CHUNK`,
`TL_X64_UNIT_WEIGHT=instructions` and `TL_X64_UNIT_SORT=0` are measurement knobs; the schedule does not change the
compiled code, so the cache key does not include it.

## Why

PvZ2's cold translation is 100,308 functions and about 6.3M instructions. The old 90,000-instruction units on the
VM (6 vCPU) took 632.2 s wall and 3447.9 s CPU (5.5 cores). The units took 620.1 s from the first to the last. The
median unit was 45.2 s, but one unit (68, functions 65489-66349, 90,210 instructions) took 186.7 s: a unit's time
is not proportional to its instruction count. The last unit (72, functions 90375-90943, 90,001 instructions, the
median weight) took 108.4 s and started last; it ran alone for the final 4.6 s.

Sorting the same units by instructions alone made the tail worse: 695.4 s wall and 3517.5 s CPU (5.1 cores), the
unit span 669.0 s, the median 46.9 s, the pathological unit still 186.8 s (now unit 36), and the last unit 97.8 s
with 50.4 s alone. The long unit's instruction weight is the median, so instruction order cannot put it first.

The 30,000-instruction units took 583.1 s wall and 3183.0 s CPU (5.5 cores); the unit span 555.4 s, the largest
unit 112.0 s and the median 13.65 s; the last unit ran alone for 2.4 s. The smaller units also lower the total CPU
by 7.7%: an LLVM module with 30,000 instructions compiles more efficiently per instruction than one with 90,000, so
the fixed cost of the extra units does not outweigh it.

The default, 30,000 weighted units with the leader weight and largest first, is the best of the four: 536.3 s wall,
3039.2 s CPU (5.7 cores), a unit span of 520.0 s, a median of 7.10 s, and the last unit ran alone for 1.3 s. The
largest unit (57, functions 66241-66406, 35,556 weight, one function with many leaders) took 103.9 s but started
first; apart from two 40 s units, the rest are under 23 s. The units sorted largest first take 521.1 s in a
simulated six-worker run, close to the 520.0 s measured.

`-O0` translates the same 30,000 weighted units in 84.8 s (293.7 s CPU, 3.5 cores): 6.3x faster than `-O2`, with a
unit span of 48.8 s, a median of 0.70 s and a last unit alone for 0.4 s. Its cache is 410.4 MB against 256.8 MB at
`-O2` (1.6x), and in a level at 1440x810 the VM presents 39-50 fps at `-O0` against 51-60 fps at `-O2`. The default
stays `-O2`: the translation time is a first-start cost, the frame rate is the game.

## Numbers (VM, 6 vCPU, one cold run each)

| units | schedule | wall s | CPU s | cores | unit span s | largest s | median s | last unit s | last alone s |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 90k | code order | 632.2 | 3447.9 | 5.5 | 620.1 | 186.7 | 45.2 | 108.4 | 4.6 |
| 90k | instruction, largest first | 695.4 | 3517.5 | 5.1 | 669.0 | 186.8 | 46.9 | 97.8 | 50.4 |
| 30k | code order | 583.1 | 3183.0 | 5.5 | 555.4 | 112.0 | 13.65 | 14.4 | 2.4 |
| 30k | instruction+leader, largest first | 536.3 | 3039.2 | 5.7 | 520.0 | 103.9 | 7.10 | 9.0 | 1.3 |
| 30k | instruction+leader, largest first, `-O0` | 84.8 | 293.7 | 3.5 | 48.8 | 2.1 | 0.70 | 1.5 | 0.4 |

The `-O2` cache is 269,211,648 bytes (one DLL) plus a 39,989 byte JIT cache and a 45,460 byte runtime object:
256.8 MB. At `-O0` it is 429,568,000 + 691,824 + 89,717: 410.4 MB. Its key includes the host CPU name and
features (`LLVMGetHostCPUName()` and `snx_ir_host_features()`), so it is CPU-specific: a cache made on one PC is
not found on another, and copying it there does not help. A warm start loads it in 4.2 s CPU over 7.7 s wall.

## The PC

A 24-thread desktop PC (8 performance and 8 efficiency cores) did the same 100,308 functions in 289.5 s with the old units, and a mid-run sample
averaged about one core: most workers had finished and a few units were still running. That is the tail this
change targets. The next test on that PC: 24 jobs, 30k and 45k chunks, instruction and leader weight, and the sort on
the PC. The VM cannot show the PC's P-core/E-core balance.

## Upstream

The change is `function_weight()`, `chunks()` and the unit sort in `library.c`, with no API change and no game
files. The profile logs (`TL_X64_PROFILE`) are our diagnostics and can stay out of an upstream patch.
