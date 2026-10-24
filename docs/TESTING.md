# Testing

How to check a change on the Mac without a device, without touching your own game, and without
disturbing whatever else you are doing on the Mac. Then: what to look at on an iPhone/iPad.

## The test runner

```sh
scripts/test-run.sh --seconds 40 --shots "15 35" -- -resizeMode Fixed -fixedAspect game
```

It starts `out/PvZ2 Reflourished.app` with a **data folder of its own** (`--save <folder>` copies one
in, so a test can start from a profile that is already past the tutorial), runs a **tap script**
(`--tap`), captures **only its own window** at the given seconds (`shot-<t>.png`), and closes the app
at the end. Everything lands in `out/runs/<time>/` (`run.log` is the app's log). `--help` lists the
options. Make a save once: run with an empty folder, play past the first screens by hand
(`--seconds 600`), and keep the run's `data/` folder.

What it sets (you can set these yourself when running the binary by hand):

| Variable | Meaning |
|---|---|
| `TL_DATA` | the data folder (default: `~/Library/Application Support/PvZ2 Reflourished`) |
| `TL_PVZ2_LOG` | where the log goes |
| `TL_PVZ2_IMPORT_FROM=<domain>` | import settings once from this domain; background tests otherwise skip import. Use a test bundle ID and test domains only. |
| `TL_PVZ2_TAP`, `TL_PVZ2_TAP_DELAY` | the tap script, and the seconds before it starts (default 3) |
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

## Settings import

The public Mac app imports its pre-release settings once, before any setting is read. The
`importedFrom` key in its own persistent domain records that the attempt happened, even if the
source was empty. The source is only read. Background test launches skip the import entirely
unless `TL_PVZ2_IMPORT_FROM` names a source domain. The runner passes through environment variables.

Build a test copy with `BUNDLE_ID=io.github.oenderg.reflourished-darwin.importtest` and use only
test domains. For example, after building that copy:

```sh
defaults write io.github.oenderg.importtest.src resizeMode -string Adapt
defaults write io.github.oenderg.importtest.src closeAction -string pause
TL_PVZ2_IMPORT_FROM=io.github.oenderg.importtest.src scripts/test-run.sh --seconds 15
defaults read io.github.oenderg.reflourished-darwin.importtest
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
export BUNDLE_ID=io.github.oenderg.reflourished-darwin.updatetest DATA_FOLDER="PvZ2 Reflourished Update Test"
export SPARKLE_PUBLIC_KEY="$(cat test-public.txt)" SPARKLE_FEED_URL=http://127.0.0.1:8799/appcast.xml
APP_BUILD=99 ./build.sh --mac --obb <obb> --apk <apk> --out out/update-old      # full, older
./build.sh --mac --slim --apk <apk> --out out/update-new                        # slim, newer
sig="$(out/update-new/.work/sparkle/bin/sign_update --ed-key-file test-private.txt -p out/update-new/PvZ2-Reflourished.dmg)"
python3 scripts/update-appcast.py out/update-new/PvZ2-Reflourished.dmg --tag v0-test \
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
./build.sh --ios-sim --slim --apk "PvZ2 Reflourished 1.4.2.apk"
```

This builds `out/PvZ2-Reflourished-sim.app`, ad-hoc signed with no entitlements (a signature
that carries `get-task-allow` is refused: "Security policy issue"). The host is compiled for the
simulator and bundle copies of the converted game libraries are relabelled `IOSSIMULATOR`. ANGLE
is its own simulator build (`prebuilt/ios-simulator`; `REBUILD_ANGLE=1` rebuilds it with
`ANGLE_SDK=iphonesimulator runtime/scripts/build_angle_ios.sh`): a device ANGLE aborts on the
game's first 16-bit texture, which the simulator's Metal does not have.

Install it with the OBB in place **before the first launch** (or the slim app downloads it), and
optionally a save (`$C/Documents/data` takes a test run's `data/` folder as it is):

```sh
xcrun simctl boot "iPhone 18 Pro"
xcrun simctl install booted out/PvZ2-Reflourished-sim.app
C="$(xcrun simctl get_app_container booted io.github.oenderg.reflourished-darwin data)"
mkdir -p "$C/Documents/obb"
cp out/.work/game.obb "$C/Documents/obb/main.675.com.ea.game.pvz2_rfl.obb"
SIMCTL_CHILD_TL_PVZ2_TAP="1310,1100;wait:14" xcrun simctl launch booted io.github.oenderg.reflourished-darwin
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
`--mask=black` draws the island and the corners into the screenshot. The log is
`$C/Documents/pvz2.log`.

## On an iPhone or iPad

There is no scripted input on a device; test by hand and read `Documents/pvz2.log` (Files app, or
`xcrun devicectl device copy from --domain-type appDataContainer ...` for a copy you signed).
Worth checking after a change that touches iOS: the first launch (OBB download, Welcome screen), a
level with touch, the hardware keyboard (1-8, S, Space, Esc), going to the Home Screen and back
(the game shows its pause menu), and the frame rate in the game's own performance overlay
(Settings in the game).
