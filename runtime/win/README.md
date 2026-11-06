# Windows (ARM64)

The runtime built for Windows on ARM: the game's own ARM64 code runs natively, loaded at run
time by the runtime's ELF loader (`husk-tl-ld.c`), with no conversion step. Work in progress.

## What is different from the Darwin build

- **Loading.** Windows lets a process run memory it wrote, so the libraries are loaded from the
  APK at start-up into one read-write-execute region (`husk-tl-xmem.c`), as Husk does with a JIT.
  The mod installs its hooks itself, as on Android.
- **x18.** Windows keeps the thread's TEB in x18. The loader rewrites every guest instruction that
  names x18 to use a "virtual x18" in a TEB TLS slot (the same rewrite the Darwin loader does, with
  the slot found through x18 instead of `tpidrro_el0`).
- **Caches.** User code may not run `dc cvau`/`ic ivau` on Windows: the guest's reads of CTR_EL0
  answer "coherent", and the host flushes with `FlushInstructionCache`.
- **The C library** (`husk-tl-bionic-win*.c`): written for Windows' LLP64 ABI from the four
  libraries' import list. `long` is 32 bits and `wchar_t` 16 on Windows, so nothing whose
  signature or struct uses them is passed through; stat, dirent, tm, timespec, FILE, the pthread
  objects, jmp_buf, z_stream, errno values and open() flags are all converted.
- **Assembly** in the shared files spells symbols with `TL_ASM_NAME` (no underscore on COFF).

## Build

```sh
export TL_WIN_TOOLCHAIN=~/somewhere/win-toolchain   # an empty folder the first time
runtime/win/setup-toolchain.sh                      # llvm-mingw, and the two zlibs
runtime/win/build-win.sh starttest                  # out/win/rfl-starttest.exe
```

`rfl-starttest.exe <apk> <data dir>` loads the four libraries, runs their constructors and
JNI_OnLoad, and the mod's library, and logs each step.
