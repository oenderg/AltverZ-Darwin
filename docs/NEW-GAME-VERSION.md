# Supporting a new Reflourished version

The apps support exactly one build of the game's code at a time. A new Reflourished release can
change any of the four libraries; how much work it is depends on which ones changed.

## 1. See what changed

```sh
unzip -o -q new.apk 'lib/arm64-v8a/*.so' -d new
shasum -a 256 new/lib/arm64-v8a/*.so
```

Compare with `libs_sha256` in `data/game-1.4.2.json`.

- **Only `libRFLib.so` changed** (the mod's own code; `libPVZ2.so` was identical between 1.4.0 and
  1.4.2): the driver's game addresses still hold. Re-record the hook table (step 3).
- **`libPVZ2.so` changed**: every `GUEST_*` address in `husk-tl-pvz2.c` and every offset in
  [GAME-NOTES.md](GAME-NOTES.md) must be found again (step 4), and the hooks re-recorded.
- **The OBB changed**: new name, size, SHA-256 and download links (step 2).

## 2. The game data file

Copy `data/game-1.4.2.json` to `data/game-<version>.json` and fill in: `version`, `obb_name`,
`cdn_url` (the mod's CDN archive), `libs_sha256` (all four), `apk_sha256`, `obb_sha256`, `obb_size`,
`obb_urls` (the mod's OBB releases are on github.com/lantern-fans/obbybackup, one tag per version).

The version is also written in these places (search for `1.4.2`):

- `build.sh`: the data and hook file names, and the CDN archive's name;
- `runtime/src/translation-layer-next/husk-tl-pvz2.c`, `runtime/tools/pvz2-macos.m`,
  `runtime/tools/pvz2-ios.m`, `runtime/tools/regress/build-*-app.sh`: the CDN archive's file name
  (`<version>.main.tar`) and the default app version;
- `.github/workflows/build.yml`: the input descriptions and the release text.

## 3. The hook table

`libRFLib` installs its 290 inline hooks into `libPVZ2` at start-up by writing branches over function
prologues. A converted library cannot be written at run time, so the final content of every byte the
mod writes is recorded once and baked in by the converter (`data/hooks-<version>.json`).

How it was recorded for 1.4.2: the game was run through Husk's translating loader (the unconverted
`.so` files, `TL_AOT_DIR` unset) on a Mac, with `TL_CODEWRITE_LOG=<file>`, which logs every code
store the mod makes; `converter/aot-hooks.py <log> --json` reduces the log to the final value per
address, and two runs are diffed to check the set is deterministic (`aot-hooks.py <logA> <logB>`).
The table's `meta` section records the address spaces used.

**Not yet re-verified from this repository**: the translating loader is still part of `runtime/`
but nothing here builds or tests that route today. Expect work here, and please document what it
took.

## 4. Game addresses

If `libPVZ2.so` changed, find each address again in the new build (Ghidra, or a disassembler and the
old build side by side: the functions are usually the same code at new offsets). The table in
[GAME-NOTES.md](GAME-NOTES.md) says what each one is for; the driver's comments say how each was
found. Check every vtable and offset by reading live objects in a test run before trusting it.
The driver guards every call it makes into the game, so a wrong address shows up as a refused action
and a log line, not as a crash. Look for those in the log.

## 5. Test

A full build (`./build.sh --apk new.apk`, no `--quick`), then on the Mac ([TESTING.md](TESTING.md)):
the first launch (Welcome screen), the title, the map, a level played for a minute with each keyboard
action (1-8, S, Space, Esc), going to the background and back, a resize and full screen after
start-up. Then the same on an iPhone or iPad by hand. Note what you could not test in the pull request.
