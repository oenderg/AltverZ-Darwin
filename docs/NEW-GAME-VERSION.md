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
- **The OBB changed**: new name, size, SHA-256 and download links (step 2). The builds take the
  name from the data file; an updated app downloads the new OBB on its first start (a full app
  carries it) and deletes the old one, and the iOS app replaces the APK it staged in Documents.

## 2. The game data file

Copy `data/game-1.4.2.json` to `data/game-<version>.json` and fill in: `version`, `obb_name`,
`cdn_url` (the mod's CDN archive), `libs_sha256` (all four), `apk_sha256`, `obb_sha256`, `obb_size`,
`obb_urls` (the mod's OBB releases are on github.com/lantern-fans/obbybackup, one tag per version).
The app identity and the mod library are in `data/profiles/<id>.json`; point its `game_data`
at the new file ([MOD-PROFILES.md](MOD-PROFILES.md)).

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

The test hooks in `husk-tl-pvz2.c` add their own 1.4.2 addresses (GAME-NOTES.md, "Test hooks"):
the driver global and the LawnApp/WidgetManager offsets, the widget list and rect fields, the
board getter and the widget/rect lookups. They are reads and guest calls on the game thread, so a
wrong one is a refused or empty dump rather than a crash; check the class names and rects against
a frame before trusting a dump.

The level jump adds more (GAME-NOTES.md, "Starting a level"): the `GameStateMgr` global
`0x2574520`, its level-name string at `+0x1348`, libc++'s `std::string::assign` at `0x5afa28`,
`LawnApp`'s board create at `0x940ce0`, `Board::LoadLevel` at `0xaa9870`, and the board's name at
`+0xe8`. The hook now tries the game's own developer start command first (factory `0x11ccbb0`, run `0x11ccc20`); the two
direct calls are its fallback.

The built-in developer commands (GAME-NOTES.md, "The game's built-in developer commands") add the
base-class vtable `0x2388978`, the class registration at `0x58ec80`, and the
factories/runs for
the collectable spawn command (`0x11cc514`/`0x11cc2e8`), the zombie spawn command
(`0x11cbcc8`/`0x11cbd38`), the seed-packet command
(`0x11cc6bc`/`0x11cc584`) and the seed-packet XP command
(`0x11cc894`/`0x11cc730`). A developer command object is 0x80 bytes, its argument is a libc++
`std::string` at `+0x60`, and the run happens on the game thread. Re-read a live object's vtable and its
argument before trusting a factory, and check the log's `ran`/`faulted` line.

The spawn actions add the per-level type tables (collectables at global `0x256d0f98`, zombies at
`0x256a620`), the tree find at `0xa6f684`, the spawn functions `0xaaf5d8` (collectable) and
`0xaafe4c` (zombie), and the board object list at `board+0x128` / `+0x130` whose count the hook
logs. The argument is the level's own key, not a class name: on the 005 level the collectable keys
are the five dino names (`dinoankylo`, `dinoptero`, `dinostego`, `dinoraptor`, `dinotyranno`) and
the zombie keys are the level's `ZombieTypes` (`halloween`, `halloween_armor1/2`,
`halloween_breakdancer`, `halloween_octopus`). A key from another level is not in the table;
inspect the level's table or its level file, and do not guess class names.

`0xaa473c` is a bare playing-state setter (`str w8=5` at `board+0xe0`); it was used by an
earlier version of the test hook and is not the normal path. The normal path is the chooser's
Let's Rock button: fill via `0x977468(chooser, entry)`, then touch the centre of the button's
own rect; the chooser's update calls `0x976aa0`, `0x976c7c`, `0x976d90`, `0x976e84` and the
board reaches state 5 by itself. The selected/required counts are `+0x238`/`+0x23c`.

The same warning as the level jump applies: a wrong factory, run or argument can fault the guest
call and kill the test run, so use a copied save and `TL_PVZ2_FRAMES` on the first try.

## 5. Test

A full build (`./build.sh --apk new.apk`, no `--quick`), then on the Mac ([TESTING.md](TESTING.md)):
the first launch (Welcome screen), the title, the map, a level played for a minute with each keyboard
action (1-8, S, Space, Esc), going to the background and back, a resize and full screen after
start-up. Then the same on an iPhone or iPad by hand. Note what you could not test in the pull request.
