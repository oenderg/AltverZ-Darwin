# Testing

How to check a change on the Mac without a device, without touching your own game, and without
disturbing whatever else you are doing on the Mac. Then: what to look at on an iPhone/iPad.
The examples use the `altverz` profile, the one this repository publishes; add
`--profile reflourished` to build the comparison mod.

## The test runner

```sh
scripts/test-run.sh --seconds 40 --shots "15 35" -- -resizeMode Fixed -fixedAspect game
```

It starts `out/PvZ2 AltVerZ.app` with a **data folder of its own** (`--save <folder>` copies one
in, so a test can start from a profile that is already past the tutorial), runs a **tap script**
(`--tap`), captures **only its own window** at the given seconds (`shot-<t>.png`), and closes the app
at the end. Everything lands in `out/runs/<time>/` (`run.log` is the app's log). `--help` lists the
options. Make a save once: run with an empty folder, play past the first screens by hand
(`--seconds 600`), and keep the run's `data/` folder.

What it sets (you can set these yourself when running the binary by hand):

| Variable | Meaning |
|---|---|
| `TL_DATA` | the data folder (default: `~/Library/Application Support/PvZ2 AltVerZ`) |
| `TL_PVZ2_CDN=offline` | skip CDN networking and replay the local archive with a fresh device-clock UTC Date |
| `TL_PVZ2_OPEN_URL=log` | validate game web links and log `pvz2: open URL <url> (test: not opened)` without opening another app; use for every automated link test |
| `TL_PVZ2_NOTIFY=log` | log reminders with their computed UTC fire time and text; never ask permission or schedule a notification |
| `TL_PVZ2_NOTIFY=soon` | schedule reminders for real, a minute after the game asks, plus one test reminder a minute after permission is known: to see one arrive on a test device (asks for permission like a normal run) |
| `TL_PVZ2_MAIN_TAR` | override the writable CDN cache path; skips bundle staging (use a test copy) |
| `TL_PVZ2_FRAMES`, `TL_PVZ2_FRAME_EVERY` | save only this app's rendered frame as `latest.bmp` in an existing directory, every N frames; works with the lid closed |
| `TL_PVZ2_LOG` | where the log goes |
| `TL_PVZ2_IMPORT_FROM=<domain>` | import settings once from this domain; background tests otherwise skip import. Use a test bundle ID and test domains only. |
| `TL_PVZ2_TAP`, `TL_PVZ2_TAP_DELAY` | the tap script, and the seconds before it starts (default 3) |
| `TL_PVZ2_STATE` | write the current screen, modal and every clickable widget as one JSON line to this file, on change (test builds only) |
| `TL_PVZ2_PRESS` | press one widget by name or label 6 s after the game has started, once (for a one-line test) |
| `TL_PVZ2_GOTO` | `map` (the world map) by the game's own button after the game has started; `almanac`, `store` and `settings` are attempted with their map buttons; `level:<name>` starts a level |
| `TL_PVZ2_DEVCMD` | the game's built-in developer commands, `;` separated, run after start-up (e.g. `startlevel egypt2`); a level start gets 5 s before the next command |
| `TL_PVZ2_QUIET=1` | a test window: small, in the corner, never takes the focus, never remembered |
| `TL_KEEP_RUNNING_IN_BACKGROUND=1` | keep drawing while another app is in front |
| `TL_AUDIO_MUTE=1` | play at a device's pace without sound |
| `TL_PVZ2_TEST_SCREEN=n` | start on display n |
| `TL_PVZ2_UPDATES=1` | start the updater (Sparkle) in a background test; it otherwise stays off there. Use a test bundle ID. |
| `TL_PVZ2_OBB_URLS` | where a slim build downloads the OBB from (space-separated; e.g. a local server) |
| `TL_PVZ2_SAFE_INSETS=l,r,t,b` | pretend the screen has a cutout, in surface pixels (an iPhone 16 Pro's island on the 1748x804 test surface: `124,124,0,0`) |

Launch arguments after `--` override the app's settings for that run only, without changing them:
`-resizeMode Fixed|Adapt`, `-fixedAspect display|game|16:9|...`, `-renderSharp YES|NO`,
`-closeAction quit|pause`, `-confirmQuit NO`. Pass them in tests: your own settings may differ.

Reminder logs include the Android notification ID, first-fire delay, repeat interval, title and
body. `pvz2-reminders.tsv` and `nimble-persistence.tsv` in the test data folder back reminders and
Nimble values across launches.
The Mac app shows no reminders and never asks for permission; `TL_PVZ2_NOTIFY=log` makes it log
each reminder the game sets. On iOS, the normal path asks for permission at launch and schedules for
real, so use `soon` (or `log`) on a test device.

## Web links

Game web links must be tested with `TL_PVZ2_OPEN_URL=log` so another app never
opens or takes focus. Settings → Help should log its real HTTP(S) URL and
`(test: not opened)`. Scheme rejection, failed/throwing openers and UI-thread
dispatch can be checked without opening any external app:

```sh
clang -fobjc-arc runtime/tools/regress/test-open-url.m -framework Foundation -o /tmp/test-open-url
/tmp/test-open-url
```

## Import/Export

This is Reflourished's FilePickerHelper; AltVerZ never asks for it (zero calls in its logs), so
moving an AltVerZ save is the file copy in the README. For the `reflourished` profile, the rows
are in title Settings -> Import/Export Files (scroll the settings list to the bottom; on a 4:3
window the row is under the performance switches). For a scripted run, two environment switches
replace only the panel, never the copy:

| Variable | Meaning |
|---|---|
| `TL_PVZ2_PICK=<file>` | every import answers with this file, as if the player chose it |
| `TL_PVZ2_PICK_OUT=<dir>` | every export writes its file or zip into this folder |

Both are inert unless set. Without them the real `NSOpenPanel`/`NSSavePanel` or
`UIDocumentPickerViewController` is shown, so a normal run always gets the panel.

The log names the operation and both paths, for example
`pvz2: file picker: exported No_Backup/pp.dat -> .../pp_20261025_183555.dat (9739 bytes)`,
`pvz2: file picker: imported ... -> .../No_Backup/pp.dat.imported (9739 bytes)` and
`pvz2: file picker: unzipped ... into .../No_Backup (3 files)`.

What to check:

- **Export Save**: `cmp` the exported `.dat` with
  `data/sdcard/Android/data/com.ea.game.pvz2_rfl/files/No_Backup/pp.dat` in the same run.
- **Export Quests**: `unzip -l` the `.zip`; it starts with `activequests/` and its files.
- **Import Save**: import another profile's `pp.dat`, quit, then start the same data folder
  again. The next start logs that it read `pp.dat.imported`, and the title shows the imported
  profile. The imported file is consumed and a new `pp.dat` is written.
- **Import Quests**: import the exported zip; unzip it elsewhere and `diff -r` the two
  `activequests` folders (the game may rewrite its quest save on exit, so compare right after
  the import or use the log's file count).
- **Import Level**: import any JSON; it lands byte-identical at
  `No_Backup/CDN.1.4.2/levels/egypt1.json`. No valid level JSON ships with the repo, so the
  file placement is what a Mac test can show.

On the iOS Simulator, install the app with an OBB and a save, launch with `SIMCTL_CHILD_`
versions of the switches, and capture the picker with `xcrun simctl io booted screenshot`.
The Files sheet takes a few seconds to appear; `TL_PVZ2_NOTIFY=log` keeps the notification
permission alert out of the picture.

## CDN paths

Use a full test build and a copied save. An online launch should log
`CDN live ... -> 200, ... bytes, Date ...`, `CDN cache updated local copy ...`, and
`HTTP delivered ... on the GL thread`. Restart with the **same data folder** to check
`CDN cache kept local copy ...`; another live request still refreshes it. With
`TL_PVZ2_CDN=offline`, expect `CDN fallback (forced offline) -> local copy, Date ...`
and no network request. Check both the world-map badges and the Promoted shop offers;
plant ownership, profile unlocks and previous purchases still govern availability.
Compare the cache's gzip hash with the guest's `CDN.1.4.2/` files after extraction.

With the lid closed, allow extra time for frames and taps. Use `TL_PVZ2_FRAMES` if window
capture is unavailable, and inspect `latest.bmp` yourself; low fps alone is not a failure.

A **locked login session** is its own case: the app keeps running, but the window server
releases its Metal drawables at about 10-15 fps, so the `frames=` lines fall from the game's
rate to that plateau after half a minute and `screencapture -l` has no picture. That is the
lock, not the game: Reflourished shows the same plateau, and `sample <pid>` puts the GL
thread in `CAMetalLayer nextDrawable`. `test-run.sh` prints a note when it sees
`CGSSessionScreenIsLocked`; unlock the session for frame-rate measurements.

The cache/allowlist regression test needs no network or game assets:

```sh
clang -fobjc-arc -fsanitize=address -Iruntime/src/translation-layer-next \
  -Iruntime/src/translation-layer runtime/tools/regress/test-cdn.m \
  runtime/src/translation-layer-next/husk-tl-cdn.m -framework Foundation -lz -o /tmp/test-cdn
/tmp/test-cdn
```

## Settings import

The public Mac app imports its pre-release settings once, before any setting is read. The
`importedFrom` key in its own persistent domain records that the attempt happened, even if the
source was empty. The source is only read. Background test launches skip the import entirely
unless `TL_PVZ2_IMPORT_FROM` names a source domain. The runner passes through environment variables.

Build a test copy with `BUNDLE_ID=io.github.oenderg.altverz-darwin.importtest` and use only
test domains. For example, after building that copy:

```sh
defaults write io.github.oenderg.importtest.src resizeMode -string Adapt
defaults write io.github.oenderg.importtest.src closeAction -string pause
TL_PVZ2_IMPORT_FROM=io.github.oenderg.importtest.src scripts/test-run.sh --seconds 15
defaults read io.github.oenderg.altverz-darwin.importtest
```

The log names the source, destination and key count. Run again with the same source: the log
says the import was skipped, and changing a source key does not change the destination. Delete
both test domains afterwards. Never write or delete a player's real defaults domain. Use a
test `BUNDLE_ID` for any test that writes settings (`mode:`, `aspect:`, or settings controls).

## Updates (Sparkle) and the OBB

An update test needs two builds with a test `BUNDLE_ID`, a test `DATA_FOLDER` (the data folder's
name in Application Support; Sparkle's relaunched app has no environment, so `TL_DATA` cannot
point it elsewhere), a throwaway key and a local feed. Never sign a test with the real key.

```sh
openssl genpkey -algorithm ed25519 -outform DER -out k.der
tail -c 32 k.der | base64 > test-private.txt
openssl pkey -inform DER -in k.der -pubout -outform DER | tail -c 32 | base64 > test-public.txt
export BUNDLE_ID=io.github.oenderg.altverz-darwin.updatetest DATA_FOLDER="PvZ2 AltVerZ Update Test"
export SPARKLE_PUBLIC_KEY="$(cat test-public.txt)" SPARKLE_FEED_URL=http://127.0.0.1:8799/appcast.xml
APP_BUILD=99 ./build.sh --profile altverz --mac --obb <obb> --apk <apk> --out out/update-old  # full, older
./build.sh --profile altverz --mac --slim --apk <apk> --out out/update-new                    # slim, newer
sig="$(out/update-new/.work/sparkle/bin/sign_update --ed-key-file test-private.txt -p out/update-new/PvZ2-AltVerZ.dmg)"
python3 scripts/update-appcast.py out/update-new/PvZ2-AltVerZ.dmg --tag v0-test \
    --ed-signature "$sig" --url http://127.0.0.1:8799/new.dmg --appcast feed/appcast.xml
```

Serve `feed/` (the appcast and the DMG as `new.dmg`) on that port. So that no Sparkle window
takes the focus, let it update silently: `defaults write <test id> SUAutomaticallyUpdate -bool YES`,
`SUHasLaunchedBefore -bool YES` and an old `SULastCheckTime` (`-date "2020-01-01 00:00:00 +0000"`).
Start a copy of the old app with `TL_KEEP_RUNNING_IN_BACKGROUND=1 TL_PVZ2_UPDATES=1`. Its log says
`cloned the bundled OBB into the data folder` and `updates from ...`; the server sees the feed and
the DMG fetched, and Sparkle's `Autoupdate` waits. Quit the app (`kill <pid>`): the bundle becomes
the slim build (`CFBundleVersion` 100, no `Game/obb`), and started again it plays from the cloned
OBB without downloading. An `.obb` of another name in the data folder's obb folder is deleted
(`removed another version's OBB`). Afterwards delete the test domain, the test data folder and
`~/Library/Caches` / `~/Library/HTTPStorages` entries of the test ID.

On iOS the same holds through the simulator: install a full `--ios-sim` build, launch it (the log
says `staged the bundled OBB into Documents/obb`, in well under a second: a clone), then install a
slim one over it, the way an AltStore update does, and launch: no download.

## The tap script

Commands separated by `;`, run in order on their own thread:

| Command | Does |
|---|---|
| `x,y` | a tap at the surface's pixel (x, y). The surface is the window's size in pixels: a Fixed-mode test window at the game's shape (`-fixedAspect game`) is 874x402 points, so 1748x804 on a Retina display and 874x402 on a standard one (halve the numbers there) |
| `drag:x1,y1,x2,y2` | a drag, in the same pixels |
| `w:x,y`, `wp:x,y` | a tap in window points, or at a percentage of the window |
| `click:<name or label>` | press a widget by the game's own name, its class, or a label it carries (`click:Profile_Play`, `click:PVZ2UIButton#1`, `click:PurpleButton`). The driver finds it on the game thread and taps the middle of its rect: no coordinates in the script |
| `goto:<dest>` | the `TL_PVZ2_GOTO` destination language: `goto:map`, `goto:level:<name>` |
| `devcmd:<command> [args]` | one of the game's built-in developer commands on the game thread (see "The game's built-in developer commands") |
| `until:<state>` | wait (up to 60 s) for `title`, `map`, `level`, `modal:<class>` or `screen`; needs `TL_PVZ2_STATE` |
| `wait:N` | wait N seconds (taps themselves take about 2 s each) |
| `press:<chars>` | type those keys through the real keyboard path (`press:1s` = key 1, then S; a space is Space) |
| `escape` | the Escape key's action (pause / resume / Back) |
| `key:N` | an Android key code straight to the game (66 ENTER, 67 DEL, 4 BACK) |
| `size:WxH` | resize the window so the surface is about W x H pixels |
| `screen:N` | move the window to display N |
| `minimize:`, `restore:`, `hide:`, `unhide:`, `sleep:`, `wake:` | the ways the app goes away and comes back (the app does it to itself; nothing is sent to the system) |
| `close`, `reopen`, `quit` | the close button, a Dock click, quitting (tests never get the quit prompt) |
| `settings`, `settings:<pane>` | open the settings window (general, keyboard, about) |
| `cursor:x,y` | where the pointer is (the person's mouse is never moved) |
| `mode:`, `aspect:`, `fit`, `keep` | window-mode tests. **`mode:` and `aspect:` write the app's saved settings** |

## Keyboard shortcuts

Start a level from the map and use the tap script's `press:` to exercise the keys. The log
names each action and why it was refused:

- `press:` (Space) logs `pvz2: the collect shortcut: ... requested N sun(s) ... N coin(s),
  N gem(s), N plant food, scanned=N`; no crash with zero drops is a pass.
- `press:f` logs `pvz2: the fast-forward shortcut: pressed the game's own button (state 0 -> 1)`;
  press it again for `1 -> 0`. The button's picture changes with it.
- `press:q` logs `pvz2: the plant food shortcut: picked up from the game's own counter at (x,y)`
  and then `the touch was let go`; with plant food in stock the cursor carries one.
- `press:z`, `press:x`, `press:c` log `pvz2: the power-up shortcut: selected power-up N of M`
  and then `the touch was let go`; click the lawn to use it. Only a level that offers power-ups
  has the buttons: Reflourished's adventure levels do, AltVerZ's do not, and AltVerZ's
  Vasebreaker levels do. Elsewhere the keys log `ignored: this level has no power-up buttons`.
- Outside a level, paused or behind a popup, each logs `ignored: ...` and does nothing.
### Driving the game by name

`TL_PVZ2_STATE=<file>` makes the driver write the current screen and its clickable widgets
as one JSON line whenever they change:

```sh
TL_PVZ2_STATE=/tmp/pvz2-state.jsonl scripts/test-run.sh --save "$SAVE" --seconds 20 \
  --tap "wait:14;click:Profile_Play;until:map" -- -resizeMode Fixed -fixedAspect game
```

The line has `state` (`title`, `map`, `level`, `level:<board state>`, `modal:<class>`, `screen`),
`mainmenu`, `transition`, `modal`, and `widgets`: `{d, cls, rect, surf, flags, labels}` for every
WidgetContainer child, depth first. `rect` is the game's design space (3339x1536); `surf` is the
same rect in surface pixels, already scaled for the run (the surface is 874x402 or 1748x804
depending on the display, and the scale is taken each time). `labels` are the strings the widget
points at (an image path, a sound event such as `Play_UI_Menu_Button_Profile_Play_Press`, or a
text); they are what makes `click:` work when the game's own widget name is not known.

`click:` matches, in order: the game's own widget name (the lookup the keyboard shortcuts use),
then the RTTI class name or any label, case-insensitively; `Class#N` picks the Nth match. The
press is a touch at the centre of the widget's anchored rect through the normal input queue, so
the game's own handlers run exactly as for a finger. It is refused (with a log line) when the
widget has no rect.

`TL_PVZ2_GOTO=map` presses the title's own play button after start-up; with a profile that was
saved on the world map this lands on the map (`until:map`). `TL_PVZ2_GOTO=level:<name>` starts the
game's own developer start command
with the name: its factory builds the command object, the name goes into its argument string, and
its run schedules the game's own level start. That is the same object the game's developer menu
uses, so the game loads the level and takes the map off the screen through its normal transition. It loads levels from other worlds, too. On the event save,
`egypt2` and `modern1` both start from the title (frames in the evidence folder):

```sh
TL_PVZ2_GOTO='level:modern1' scripts/test-run.sh --save "$SAVE" --seconds 40 \
  -- -resizeMode Fixed -fixedAspect game
```

If the developer command cannot be built or run (a new game build, a bad name), the hook falls back to
the two calls the map makes: it sets the level name in `GameStateMgr`, creates the board and calls
`Board::LoadLevel`, then unlinks the map widgets. That fallback only resolves levels in the current
world's registry, which on the event save is the event's own path
`lod/holidayevents26/halloween/lod_halloween_005`; other names can fault. With either path,
`level_name` in the state line is the board's own name (from `board+0xe8`) and is the check that
the jump loaded what it asked for.

The event level's intro has a `PVZ2UIButton` (CONTINUE); pressing it reaches board state 5:

```sh
TL_PVZ2_GOTO='level:lod/holidayevents26/halloween/lod_halloween_005' \
  scripts/test-run.sh --save "$SAVE" --seconds 45 \
  --tap "wait:22;click:PVZ2UIButton;until:level;wait:8" -- -resizeMode Fixed -fixedAspect game
```

The seed chooser on a normal level is drawn by the board's own UI, not a `WidgetContainer` in the
state tree, so its "LET'S ROCK!" button is not listed and `click:` cannot press it yet (open).
The tap script also has `goto:<dest>` and `devcmd:<command>` steps, so both can run after the game
is already up.

### The game's built-in developer commands

`TL_PVZ2_DEVCMD` and a tap `devcmd:` step build one of the game's developer command objects (the
binary names them the developer-command classes), put the argument in its `std::string`, and run the game's own method
on the game thread, in the frame loop. Each command is logged with its object and
`ran`/`faulted`; nothing runs when the switch is absent.

| Command | Does | Example |
|---|---|---|
| `startlevel <name>` | the game's own level start; works from the title or the map, and for another world | `devcmd:startlevel egypt2` |
| `collect <type>` | drops a collectable | `devcmd:collect dinoankylo` |
| `zombie <type>` | spawns a zombie | `devcmd:zombie halloween` |
| `seedpacket <plant>` | a collectable seed packet | `devcmd:seedpacket sunflower` |
| `seedpacketxp <plant>` | the same, with XP | `devcmd:seedpacketxp sunflower` |
| `rock` | fills the seed chooser and presses its Let's Rock button; takes no argument | `devcmd:rock` |

The argument is the **level's own type key**, not a class name. The collectable table on the 005
event level holds `dinoankylo`, `dinoptero`, `dinostego`, `dinoraptor`, `dinotyranno`; its zombie
table holds the level's `ZombieTypes`: `halloween`, `halloween_armor1`, `halloween_armor2`,
`halloween_breakdancer`, `halloween_octopus`. The action looks the key up in that table and spawns
the resulting object. `gem`, `coin`, `plantfood` and `sun` return no object on 005 because that
level declares no such collectable; the normal-level tables are reachable with `devcmd:rock`,
but they still hold no coin, gem or plantfood keys. Start the 005 level and run the commands
after the board is playing (state `level`, board state 5):

```sh
TL_PVZ2_GOTO='level:lod/holidayevents26/halloween/lod_halloween_005' \
  scripts/test-run.sh --save "$SAVE" --seconds 50 \
  --tap "wait:26;click:PVZ2UIButton;until:level;devcmd:collect dinoankylo;wait:2;devcmd:zombie halloween;wait:6" \
  -- -resizeMode Fixed -fixedAspect game
```

`TL_PVZ2_DEVCMD='startlevel egypt2'` starts that level from the title at about 10 s (run 131). The
commands run in order; a `startlevel` gets five seconds before the next one. Every command logs its
object, `ran`/`faulted`, and the board's object count (`objects N -> M`); a spawn changes the
count even when the camera is showing another part of the lawn.

`rock` finds the live `SeedChooser` by its vtable, fills its required slots through the
chooser's own add method (`0x977468(chooser, entry)`), then queues the same touch a player makes
at the centre of the Let's Rock button's own rect. The game's own UI path runs the chooser's
four start functions (`0x976aa0`, `0x976c7c`, `0x976d90`, `0x976e84`); the board changes from
state 3 to 5 by itself. It does not write `board+0xe0`. Runs 238 (modern1) and 239 (egypt2)
show the running lawn with the chooser gone and a zombie walking.

**What is proved:** `startlevel` (egypt2, modern1, the 005 event level; frames 110/130).
`zombie halloween` spawns on 005 (objects 0 -> 3; frame 167) and every one of the five dino
collectable keys spawns (the list grows by 6-9 per key; frame 169). `coin`, `gem`, `plantfood`, `sun` and
`coin_silver` return no object on 005 (run 160); the normal-level tables that hold them need the
seed chooser (open). The `seedpacket`/`seedpacketxp` objects run, but their effect is not isolated
yet. There is no exposed command object for adding sun, coins, gems or plant food directly, for
power-up stock, or for killing all zombies/winning a level; the developer command system's static
helpers are not command objects. `LET'S ROCK!` is handled by `devcmd:rock` above.



## Reading the result

- Look at the shots yourself. A log that says "drawn" is not a picture of the game.
- Useful log lines: `pvz2: the game has started` (past start-up, ~9 s), `macos: frames=..., last 5s=..
  fps`, `egl: swap #N, actual surface WxH`, `pvz2: the seed pick: ... handled=1`,
  `pvz2: ...: ignored: <reason>` (a keyboard action refused, and why), `Escape presses ...`,
  `lifecycle -> background done` / `foreground done`, and `CRASH: signal=...` with the fault address.
- The log is written in chunks, and the runner stops the app at the end, so the last seconds of a
  log can be missing. End the tap script with `quit` to close the app properly (the game saves, and
  the log is complete); the runner then reports that the app exited.

## Rules for tests on a Mac someone is using

- Never post input to the system (no `CGEventPost` to the HID tap, no AppleScript keystrokes), never
  bring an app to the front, never capture the whole screen. The tap script, `CGEventPostToPid` to the
  test's own pid, and `screencapture -l <window>` are fine.
- Kill only the test's own pid. Never by app name: the person may be playing.
- Use your own data folder and launch arguments, never the app's real settings.

## iOS Simulator

The iPhone's screen (the Dynamic Island, the rounded corners) without an iPhone, on an
Apple-silicon Mac with Xcode's iOS Simulator and an iOS runtime installed:

```sh
./build.sh --profile altverz --ios-sim --slim --apk "AltVerZ_1.8.3f.apk"
```

This builds `out/PvZ2-AltVerZ-sim.app`, ad-hoc signed with no entitlements (a signature
that carries `get-task-allow` is refused: "Security policy issue"). The host is compiled for the
simulator and bundle copies of the converted game libraries are relabelled `IOSSIMULATOR`. ANGLE
is its own simulator build (`prebuilt/ios-simulator`; `REBUILD_ANGLE=1` rebuilds it with
`ANGLE_SDK=iphonesimulator runtime/scripts/build_angle_ios.sh`): a device ANGLE aborts on the
game's first 16-bit texture, which the simulator's Metal does not have.

Install it with the OBB in place **before the first launch** (or the slim app downloads it), and
optionally a save (`$C/Documents/data` takes a test run's `data/` folder as it is):

```sh
xcrun simctl boot "iPhone 18 Pro"
xcrun simctl install booted out/PvZ2-AltVerZ-sim.app
C="$(xcrun simctl get_app_container booted io.github.oenderg.altverz-darwin data)"
mkdir -p "$C/Documents/obb"
cp out/.work/game.obb "$C/Documents/obb/main.675.com.ea.game.pvz2_auz.obb"
SIMCTL_CHILD_TL_PVZ2_TAP="1310,1100;wait:14" xcrun simctl launch booted io.github.oenderg.altverz-darwin
xcrun simctl io booted screenshot --mask=black out/ios-sim.png
```

`SIMCTL_CHILD_` passes a variable to the app. A phone is always in landscape; an iPad
simulator starts upright, and `TL_FORCE_LANDSCAPE=1` turns its window. `TL_PVZ2_TAP` is the Mac runner's tap language
(`x,y`, `drag:x1,y1,x2,y2`, `wait:N`) in the game's own pixels (2622x1206 on an iPhone 18 Pro),
starting `TL_PVZ2_TAP_DELAY` seconds (default 3) after the game has started.
For name-field tests, `press:<text>` calls the UITextField input delegate with committed
text, `keyboard:hide` resigns its first responder (exercising editing-end and UIKit's
keyboard notifications), and `return` calls its Return delegate. These exercise host
callbacks; they do not simulate a physical tap on the on-screen keyboard's hide key.
`TL_PVZ2_TEST_HARDWARE_KEYBOARD=1` gives this app's UITextField an empty input view
while keeping it first responder. Use it with `press:` to test touch dismissal
without a software keyboard; it does not change Simulator's global keyboard settings.
`--mask=black` draws the island and the corners into the screenshot. The log is
`$C/Documents/pvz2.log`.

## On an iPhone or iPad

There is no scripted input on a device; test by hand and read `Documents/pvz2.log` (Files app, or
`xcrun devicectl device copy from --domain-type appDataContainer ...` for a copy you signed).
Worth checking after a change that touches iOS: the first launch (OBB download, Welcome screen), a
level with touch, the hardware keyboard (1-8, S, Space, Esc), going to the Home Screen and back
(the game shows its pause menu), and the frame rate in the game's own performance overlay
(Settings in the game).
