# Game notes (PvZ2 8.3.0; recorded on Reflourished 1.4.2, shared with AltVerZ 1.8.3f)

What the game does that is not obvious, and the places in its code the driver relies on. Every
address here is a **guest virtual address in `libPVZ2.so` of the 1.4.2 build** (the one
`data/game-1.4.2.json` pins); a different build moves them all ([NEW-GAME-VERSION.md](NEW-GAME-VERSION.md)).
The driver turns them into callable pointers with `guest_code()` (code) and
`tl_aot_guest_data_ptr_any()` (data, e.g. vtables), and calls the game only on its own thread,
through a guard that turns a fault into a log line instead of a crash.

## Start-up

- **Do not change the screen size while the game starts.** The game's "original screen size"
  (a `{w, h}` pair at app object +0x86c) is the root of its geometry; it builds screen-sized things
  from it while it starts. If the host writes a new size there before the game is done, a few seconds
  later the game copies from an image that was never made (a null image in the copy at 0x1550050,
  fault 0x48) and crashes. The driver's `tl_pvz2_started()` turns true once the game has read the
  `prevent_load_game` setting (every start, right before it loads the save) and then drawn smoothly
  for 2 s (~9 s on a recent Mac); the Mac host holds the window's size until then, scaling the
  picture meanwhile. Holding back only that one write is not enough: the clicks then miss.
- **The game picks its art once**, from the size it starts at: the HD set (1536 lines) when the
  surface is tall enough (750 and 804 px tall did), the standard set (768) when it is small (551 did).
  The Mac host starts at least 804 px tall for that reason, then goes to the display's scale.
- `LawnApp::SetWidthHeight` (0x93ead0) runs once at start-up and derives the app's size from the
  original screen size; it never runs again. The resize path writes the new size into the app object
  (+0x86c, +0x744, and the app size at +0xf4/+0xf8 from the design height at +0x9c4).

## The board and the keyboard features

| Address | What it is | Used for |
|---|---|---|
| 0xce4d8c | the current board (null outside a level) | every keyboard action |
| board +0xe0 | board state; 5 = playing | the live-play check |
| board +0x6c8, 0x7dec64 | the pause registry, and the game's own "any pause reason" check | refuse while paused |
| widget manager (board +0x20) +0xe0 | the modal widget (null when nothing covers the board; +0xc8 is the keyboard focus, not the modal) | refuse behind popups |
| 0xaa1be8, 0x12d2374 | the seed bank and one packet in it | key 1-8 picks a packet |
| packet vtable +0x90 | the packet's own pointer-down handler (normal packet vtable 0x24b2940) | the pick itself |
| 0x761074 | the game's release decision for a new cursor (PlantCursor vtable 0x2491a60) | a second press cancels, like a second tap |
| 0x13ab0a0, vtable 0x24d8b60 | the shovel's pointer-down, and the shovel cursor | S |
| 0x11beef0, 0x14ea3cc / 0x14ea364 / 0x14ea3b0 / 0x14ea1fc | the iterator over the board's collectables (pool 0x48) | Space |
| 0x64f998 | a collectable's own "clicked" request (the game picks the flight and awards the value) | Space collects suns (vtable 0x2395208), real coins (0x23bb9b8), gems (0x2399d48), plant food (0x24250b8) and sprouts (0x24d5ce0) only; fake coins (0x23bbc18) and rewards are not accepted |
| 0x75a96c | a sun's value | the log line |
| 0x5beb44, 0x5c1788 | find a UI widget by name, and its rect in the app's design space | the pause button (`UIPauseButton`), the shovel, the seed tray, fast-forward, plant food and power-ups |
| 0x24b2dc0, +0x88 = 0x12300ec, +0x19a | FastForwardButton's vtable, its toggle (flips the byte at +0x19a and tells the game), and that state | F (`UIFastForwardButton`) |
| 0x24c21b0, +0x90 = 0x1311f90 | PlantfoodUI's vtable and its pointer-down (starts the counter's pickup) | Q (`UIPlantfood`) |
| 0x249dfa0, +0x90 = 0x1162138 | PowerupTimeUI's vtable and its pointer-down (selects the power-up) | Z, X, C (`UIPowerupHolder`, buttons named `UIPowerup`; 73 px wide, 98 apart, holder 308 wide for three). Reflourished's adventure levels offer them, AltVerZ's do not; AltVerZ's Vasebreaker levels do |
| 0x24bcf00 | the pause menu (`PauseDialog`); its rect is at +0x44 | Escape on the pause menu taps RESUME (82% across, 90% down) |

- **Android's Back does nothing in a level** in this game, so Escape presses the pause button
  instead (and RESUME on the pause menu). Elsewhere it is Back.
- **Space's exact vtables are what keep rewards out.** Gems, plant food and sprouts share the
  sun's and coin's `Collectable` layout and their `Collectable::OnMouseDown` path (0x64f804 to
  0x64f998); their own classes only add the value/type members. A sprout is the level drop whose
  advice says "You found a sprout!" and sends it to the Zen Garden; the game's collected-listener
  for it (0x83d35c) adds one to a sprout counter at +0xd8. Level rewards, presents,
  stars, keys, notes and plant unlocks have different vtables and are never requested. Fake coins
  are a subclass of real coins but have their own vtable, so a tap's refusal is kept.
- **Fast-forward is the button's own toggle**, not a pixel: `FastForwardButton` vtable +0x88
  flips its state byte at +0x19a. Plant food and power-ups are taps on their own widgets, with
  the down now and the up a frame later. A power-up level with fewer than three buttons is
  detected from the holder's width compared with one button's (1.5, 2.9 or 4.2 buttons wide), so
  a key beyond the level's set is refused instead of tapping the lawn.
- Every action is refused, with a log line saying why, unless the board is live, nothing is paused
  and no modal is up. Never write the game's sun, currency or object state directly: call the game's
  own functions and let it do its bookkeeping.

## Test hooks (test builds only)

The driver's `TL_PVZ2_STATE`, `TL_PVZ2_PRESS` and `TL_PVZ2_GOTO` switches (TESTING.md) read the UI
and press by name. They are inert without the variables, and every read/call runs on the game
thread in the frame loop, after the frame. The addresses are 1.4.2-only, like every other address
here; the live values were read in the test runs named in the evidence folder (see TESTING.md).

| Address / offset | What it is |
|---|---|
| 0x25a4e50 | the driver global -> `AndroidAppDriver` |
| driver +0x88 | `LawnApp` (`+0x98` is `AndroidGraphicsDriver`, not the app) |
| LawnApp + 0x380 | `Sexy::WidgetManager` |
| LawnApp + 0xaf8, +0xb00 | the `TransitionOverlayWidget`, and `MainMenu` (non-null on the title) |
| WidgetManager +0xe0 | the base modal widget (null when nothing covers the screen) |
| WidgetManager +0xc8 | the keyboard focus widget, not the modal |
| 0xce4d8c | `LawnApp::GetBoard`; board +0xe0 is its state (3 = intro, 5 = playing) |
| 0x5beb44 | find a UI widget by the game's own name (the string object described below) |
| 0x5c1788 | a widget's anchored rect (out pointer in x8, widget in x0) |

The widget tree (`Sexy::Widget`, read live; the RTTI class comes from `vtable[-1]` -> typeinfo +8):

- `widget+0x00`: vtable; `vtable[-1]` is the typeinfo, `typeinfo+8` the class name. Names are
  Itanium RTTI (`8MainMenu`, `N4Sexy13WidgetManagerE`, `25PVZ2UIScrollingPaneWidget`).
- `widget+0x08` / `+0x10`: the child list's **sentinel** (`prev` / `next`); the sentinel is
  `widget+0x08` itself. A node is `{prev, next, Widget*}` with the child at `node+0x10`, and
  `node+0x08` is the next node. Walk until the node is `widget+0x08`.
- `widget+0x20`: the `WidgetManager`; `widget+0x28`: the parent `WidgetContainer` (null at the top).
- `widget+0x40` / `+0x48` / `+0x50`: the rect, `x = high32(+0x40)`, `y = low32(+0x48)`,
  `w = high32(+0x48)`, `h = low32(+0x50)`, in the game's design space (3339x1536). A child's rect
  is relative to its parent; add the parents for the screen position.
- `widget+0x58..+0x6f`: flag bytes. `+0x64` is 1 on buttons and 0 on containers, `+0x65` is 1 on
  the enabled title buttons and 0 on the two disabled ones, and `+0x68` is 1 on every visible
  widget. The exact names are not proved yet, so the dump reports the raw bytes.
- Labels: the strings a widget points at, one object down (an image path
  `images\\1536\\initial\\UI\\Generic\\PurpleButton`, a sound event
  `Play_UI_Menu_Button_Profile_Play_Press`, or a text). They identify a button when the game's own
  name is not known.

The screens in 1.4.2: the title is `MainMenu`; the world map is a top-level `UniverseMap` with
`UniverseWorldButton`s whose labels are `Play_UI_Map_Select_<world>_Press` (egypt, pirate, ...);
a level has a top-level `Board` and the board state at +0xe0. The title's play button carries
`Play_UI_Menu_Button_Profile_Play_Press`, the profile button `Play_UI_Button_Click_Press`, the two
settings buttons `Play_UI_Button_Settings_Press`, and the profile dialog's close button
`Play_UI_Button_X_Close_Press`.

A press is a touch at the middle of the widget's anchored rect, converted to the surface with the
app's own design height (the same `P.h / design_h` the shovel uses), through `tl_pvz2_touch`: the
game's normal input path, no handler called behind its back. A widget with no layout entry falls
back to its own rect (parents added); a parent with no rect (a layout holder such as
`Widget_ButtonList`) contributes nothing and does not stop the walk, so a button inside a dialog
gets the dialog's origin added too. The surface changes size once, after start-up (the host sets
it to the display's scale), so the scale must be read at the moment of the press, never cached.

### Starting a level

The map's own level start was traced live (a watchpoint on `LawnApp+0x9a8`, runs 43/44, and the
level intros in runs 63/83/84). The pieces the jump uses:

| Address / offset | What it is |
|---|---|
| 0x2574520 | the global that holds the `GameStateMgr` pointer |
| GameStateMgr +0x1348 | a libc++ `std::string`: the level name the start reads |
| GameStateMgr +0x1200 / +0x1208 / +0x1340 | screen/state ints (map 5/11/9, level 5/10/2) |
| GameStateMgr +0x13c0 | the byte the level-start handler tests (0 on a normal start) |
| 0x5afa28 | libc++ `std::string::assign(const char*, size_t)` |
| 0x940ce0 | `LawnApp`'s board create: `new Board`, `LawnApp+0x9a8 = board`, widget setup |
| 0xaa9870 | `Board::LoadLevel(const std::string&)`: copies the name to the board |
| Board +0xe8 | the board's own copy of the level name (a `std::string`) |
| 0xcb26b4 | `GameStateMgr` vtable slot 29, the update the map calls to start |

The map's own path (the state handler at `0xcb26b4` -> `0x940ce0` -> `0xaa9870`) also performs the
screen transition that takes the map widget off the manager's list. The test hook now uses the
game's own developer start command first (below): it performs that transition itself, so no map unlinking is needed.
The direct two calls remain as a fallback and still need `test_remove_map_widgets`.

**The developer-command start is the general jump.** Its run method
loads the name through the game's own level loader and takes the map off the screen through the normal
transition. It works from the title and from the map, and it knows levels outside the current
world's registry: runs 130/131 started `egypt2` and `modern1` from the event save's title, and run
110 started `egypt2` from its map (frames 110 and 130 show the Egypt and Modern Day lawns). The
event level `lod/holidayevents26/halloween/lod_halloween_005` also starts; its intro's CONTINUE is
a `PVZ2UIButton`, and pressing it with `click:PVZ2UIButton` reaches board state 5 (runs 121/122).

**Limitation of the fallback only:** `Board::LoadLevel` resolves the name in the world's current
level registry, which holds only the level the world is on. On the level save that is
`lod/holidayevents26/halloween/lod_halloween_005`; other event paths (001..004) and normal names
(`egypt1`, `egypt2`) returned no definition and faulted the guest call (runs 65/67/71/73/94). The
developer-command start avoids this registry.

## The game's built-in developer commands (test hooks)

`libPVZ2` has a developer command system behind the game's developer menu; the binary names them with its own prefix. A command is a small object: a factory allocates 0x80 bytes (vtable, +0x20 =
0xff, +0x60 a libc++ `std::string` for the argument), and a run method acts on it. The base-class vtable is at 0x2388978; each class registers itself
at start-up from `0x58ec80` through `[class vtable + 0x40]` with its RTTI name. The globals
`0x258d3e0..0x258d428` hold the **class objects**, not instances; a test constructs its own
instance with the factory and runs it on the game thread, exactly where the game's developer menu
runs.

| Developer command | Factory (1.4.2) | Run | Argument / result |
|---|---|---|---|
| the developer start command | 0x11ccbb0 | 0x11ccc20 | the level name; schedules the game's start |
| the collectable spawn command | 0x11cc514 | 0x11cc2e8 | a collectable type key; calls `0xaaf5d8(board, key, -1)` |
| the zombie spawn command | 0x11cbcc8 | 0x11cbd38 | a zombie type key; calls `0xaafe4c(board, key, -1)` |
| the seed-packet command | 0x11cc6bc | 0x11cc584 | a plant/seed-packet key; calls `0xaa4180` |
| the seed-packet XP command | 0x11cc894 | 0x11cc730 | the same, plus the XP amount |
| the plant unlock command | 0x11cb980 | not mapped | a plant name |
| the costume toggle command | 0x11cb59c | not mapped | costume/plant/layer fields |
| the creature spawn command | 0x11cc278 | not mapped | a creature name |
| the save-load command | 0x11cca74 | not mapped | the save name |

Other registered developer commands cover player segments, plant selection, spawning plants on the cursor, plant boosts, sprout IDs and hot-UI loading; the UI-line classes are the UI-line developer commands. The strings
also name static helpers (world unlock, extended unlock, quest, version and narrative), but they are
not command objects and are not exposed.

**The argument is the level's own key, not a class name.** The collect and zombie actions pass
the argument through a per-level type table (collectables: the global at `0x256d0f98`; zombies:
`0x256a620`), then call the spawn function with the resulting `String` ID. A table holds only the
types the current level declares. On the 005 event level the collectable table holds the five
Piñata Party dinos -- `dinoankylo`, `dinoptero`, `dinostego`, `dinoraptor`, `dinotyranno` -- and
the zombie table holds the level's `ZombieTypes` list -- `halloween`, `halloween_armor1`,
`halloween_armor2`, `halloween_breakdancer`, `halloween_octopus`.

**What is proved:** `startlevel` (above). `zombie halloween` spawns: the board's object list at
`board+0x128` goes 0 -> 3 and frame 167 shows the added halloween zombies (run 167). Every one of
the five dino collectable keys spawns: the list grows by 6-9 per key (0 -> 27 for four keys) and
frame 169 shows the dino collectables (runs 161/166/169). `coin`, `gem`, `plantfood`, `sun` and `coin_silver` return no
object on 005 because that level declares none of them (run 160); the normal-level table is the
one to use once its running board is reachable, but that board is behind the seed chooser (open,
below). The spawn objects append to the same list, so the list count is the cheapest proof when
the camera shows another part of the lawn.

The generic type directory behind the spawn functions is
`0x14ead90` -> `0x14f39cc` -> `0x14f31a4` -> `0x14ede00`; the per-level tables above are the
ones to inspect for a key, and a class name like `CollectableGem` is not a key. The arguments are
now passed unchanged; the old friendly mapping (`gem` -> `CollectableGem`) is gone.

`rock` finds the live `SeedChooser` (vtable `0x23b7db0`) by scanning the heap around the
board, fills the required slots with `0x977468(chooser, entry)` from the `+0x208` list, and
queues a touch at the centre of the Let's Rock button (`chooser+0x248`, app-space rect
`2884,1381,435,130`, converted to surface pixels). The chooser's own update then calls
`0x976aa0`, `0x976c7c`, `0x976d90`, `0x976e84`; the board reaches state 5 without a write.
The selected/required counts are `+0x238`/`+0x23c`; modern1 and egypt2 need 7. Runs 238/239
are the running-lawn frames. The old state write at `0xaa473c` is not used.
`seedpacket` / `seedpacketxp` build and run, but do not fill that count; they are collectable
seed packets, not chooser entries.

**Collectable drops (open).** The normal collectable spawn is `0xaaf94c(board, key, column,
row)`: it resolves the level's type key through the level table. The modern1 and egypt2 tables
do not hold `coin`, `gem` or `plantfood`, which is why `collect` returns objects 0 -> 0. The
falling sky sun has its own caller: `0xab5ae8` passes the owner's loot string at `+0x3c0` to
`0xaa3bc0`, which creates a sun through the factory at `0x8ae4cc` with class id `0x48`, then
positions it from `+0x440/+0x444/+0x448/+0x44c`; the class id is hardcoded for sun. The
collectable classes and factory registrations are `CollectableCoin` 0x9c8f7c/0x559328,
`CollectableGem` 0x78f374/0x54b4a8 and `CollectablePlantfood` 0xcef9a8/0x56e988. Their
coin/gem/plantfood creator wrappers and class ids are not mapped, so no game-path drop is
exposed yet. Hand-built board entries were tried and drew nothing (frame 237); that path was
discarded.
No
direct add-sun/coins/gems/plant-food/power-up-stock and no kill-all/win-level command class was
found; the game's static helpers has static helpers such as `CompleteAllArcadeLevels` and `NonFatalError`, but no
exposed "win" object.

The seed chooser (the intro a level shows before its waves) is drawn by the board's own UI, not a
`WidgetContainer` in the app tree, so `TL_PVZ2_STATE` does not list its "LET'S ROCK!" button and
`click:` cannot press it yet. The event level's intro dialog **is** a widget: `click:PVZ2UIButton`
presses its CONTINUE and reaches state 5. The in-level HUD and seed chooser are a separate
`WidgetManager` at `Board+0x20`; it holds six top-level widgets and no seed chooser in 1.4.2.


## Generated IDs

`Util_GetUUIDString` returns a fresh lowercase version-4 UUID per call, like
Android's `UUID.randomUUID().toString()`. The guest uppercases it where it needs
to and owns persistence. Existing saves keep their stored PCPID and quest IDs,
including IDs made by the old constant stub; there is no identity migration.
`TL_JNI_TRACE=2` logs generated UUIDs for testing.

## Local reminders

iPhone and iPad only: the Mac shows no reminders (no permission prompt, nothing scheduled; the
driver still keeps their data, as Android does when notifications are not allowed).

Android's `AndroidNotification.scheduleBasicNotification` stores `source`, `groupid` and the
game's serialized `userInfo` by notification ID. `fireAt` is an absolute Unix timestamp in
seconds (`0` means now); a positive `secondsInterval` repeats from that first fire time. If a
repeating time is already past, Android advances it to the next interval boundary. A past
one-shot is discarded. The receiver uses `title`, `message`, and its channel's sound, and puts
`userInfo` plus `notificationType=LN` in `data.fields` for the activity. The scheduled-data getter
returns those raw `userInfo` fragments joined with commas inside `[...]`, including `[]` when
empty; they are already serialized JSON, so they are not quoted again.

The driver keeps that list atomically in `pvz2-reminders.tsv` under the app data folder. Removing
one ID or all IDs cancels their pending local notifications and removes the list entries. The
Android `removeScheduledNotificationsBySource` implementation only removes matching list entries;
it does not cancel the AlarmManager requests, so the port preserves that behavior. The Android
group ID maps to Apple's notification thread; channel IDs select channel sound. Android's
channel names/descriptions, activity class, action and drawable icon have no direct UserNotifications
equivalent; the notification opens the app and its `data.fields` are restored through
`Info_SysGetIntentExtraDataString`.
The game's observed channels have empty sound names and use the system default. A named Android
raw sound is passed to Apple as a `.caf` resource name, but custom sound playback is unverified
because the app build does not bundle a converted sound resource.

At launch, the host reads notification authorization. If it is still undetermined, the host asks
for alert, sound and badge authorization once the game creates its notification channels (at
launch, while the app is active): Android 13 asks at that moment for apps that target SDK 31, as
this one does. A reminder scheduled before the answer waits for it. A denial leaves the reminder unscheduled and is logged. On iOS, a short background task
keeps asynchronous notification adds alive until UserNotifications accepts or rejects them.
`TL_PVZ2_NOTIFY=log` stores the reminder data and logs the computed UTC fire time and text, but
neither prompts nor schedules.
UserNotifications cannot repeat at an Android reminder's future absolute first-fire time and then
keep its interval. The port schedules the first fire only and logs the dropped repeat once per
reminder ID; the game re-schedules reminders on later background transitions.

## Nimble persistence

Nimble's `Persistence` stores Java-serialized `Serializable` values per
`nimble.persistence.<component>` scope and delays disk writes by 500 ms. The active game path stores
the birthdate as `java/lang/String`. The driver writes supported strings and boxed scalar values
to `nimble-persistence.tsv` in the host data folder with atomic replacement, and restores their
original Java types at the next process start. `setEncryption` remains a no-op, as in the port
before persistence was added.

## Import/Export (FilePickerHelper)

The title's Settings -> Import/Export Files menu calls five static methods on
`FilePickerHelper` through JNI. Android posts each call to its UI thread, opens the system
document picker and returns. The port queues the same request for a worker
(`husk-tl-pvz2-picker.c`), so the game keeps drawing, and the host presents its own picker
(`NSOpenPanel`/`NSSavePanel` on the Mac, `UIDocumentPickerViewController` on iPhone and iPad).

The paths are the ones libRFLib passes, relative to Android's
`getExternalFilesDir(null)`. Here that is
`<data>/sdcard/Android/data/com.ea.game.pvz2_rfl/files`.

| Menu row | Call | Path |
|---|---|---|
| Import Save | `importFile` | `No_Backup/pp.dat.imported` (the chosen file is copied there) |
| Export Save | `exportFile` | `No_Backup/pp.dat` (suggested name `pp_<timestamp>.dat`) |
| Import Quests | `importZip` | `No_Backup` (the zip's entries are extracted there) |
| Export Quests | `exportFolder` | `No_Backup/activequests` (zipped, suggested `activequests_<timestamp>.zip`, entries `activequests/...`) |
| Import Level | `importFile` | `No_Backup/CDN.1.4.2/levels/egypt1.json` |

Import Quests follows Android's `importZipToDir`: for each top-level name in the zip it
deletes that name under `No_Backup` before extracting, so a quests backup replaces
`activequests` rather than merging into it. Entries with a `..` component or a backslash are
skipped with a log line; a leading slash is treated as relative to `No_Backup`, as Android's
`new File(parent, child)` does.

After an import:

- **Save.** The profile loader in libRFLib (`libRFLib+0x404b44`) looks for
  `<save>.imported`, then `<save>`, then `<save>.bak`. `pp.dat.imported` therefore wins over
  the current `pp.dat` at the next load. It is not reloaded into a running title screen:
  the game reads it at the next start, then consumes it and writes a fresh `pp.dat` and
  `pp.dat.bak`. Restarting (or closing and reopening the app) shows the imported profile.
- **Quests.** The files are on disk as soon as the copy finishes; the game reads them when
  it next loads its quest state. Importing while the title is open does not rebuild the
  open quest UI by itself.
- **Level.** `egypt1.json` is a level override for Egypt level 1. The game reads the JSON
  when that level is loaded. The test data and the pinned APK contain no JSON level to
  show, and the schema is not documented in the repo, so the port proves the byte-exact
  placement only.

The suggested export name and the zip format match Android's `SimpleDateFormat`
(`yyyyMMdd_HHmmss`) and `ZipOutputStream`: directories have their own entries, files are
deflated when that is smaller and stored otherwise, and names are UTF-8.

## Web links

`Web_SysOpenURL` calls a host hook: AppKit's `NSWorkspace openURL:` on Mac,
UIKit's `openURL:options:completionHandler:` on iOS, on the main thread. Only
valid HTTP(S) URLs are accepted. The game's Help and legal URLs use those schemes;
email intents are a separate Java path, so this hook does not accept `mailto`.
`TL_PVZ2_OPEN_URL=log` validates and logs without opening another app.

The JNI result is false for refused URLs, failed launches or exceptions. The APK
actually returns true even in its catch block; the host deliberately reports those
failures. On iOS, true means the handler exists and the request was submitted;
a later UIKit completion failure is logged. Waiting for that completion would
hold up the guest while the main thread asks it to save on backgrounding.

## Profile name input

Android's `EditInputConnection` queues text records through `UI_ProcessEvents`: type 6,
action 0 = committed UTF-8, 1 = composing text, 2 = finish composition (empty payload),
3 = backspace (empty payload). Action 2 ends composition, not the field's edit. Return
is Android keycode 66. `HandleLostFocus` / `HandleGotFocus` concern app focus and touch
cancellation; they do not queue a text-field focus event.

Dismissing Android's keyboard goes through `AndroidSurfaceView.onKeyPreIme` to
`SexyAppFrameworkActivity.onKeyboardDismissedWithBackButton`, which only clears
`AndroidGameApp.mbIsKeyboardShowing`. Show sets it true; hide, dismissal and app focus
loss set it false. The native field polls `Device_IsKeyboardShowing`: false ends the
edit, validates the name and enables ACCEPT / SAVE. An empty field then shows
"TAP TO ENTER NAME". Sending characters alone leaves the edit open. On first
launch, ending a nonempty edit also opens Reflourished's "Skip Base Game Content"
prompt; answer it before pressing the now-enabled Welcome ACCEPT button.

The hosts must report dismissal through `tl_pvz2_keyboard_dismissed`, which clears
an atomic flag; only the game thread polls it and ends the edit. iOS reports both
editing-end and keyboard-hide notifications (the iPad hide key can leave the hidden
UITextField as first responder). On Mac, a click ends the previous edit before the
same click reaches the game, including a click on ACCEPT. Clicking the field can
start another edit. Return keeps its existing keycode 66 path.

iOS also ends the edit on any touch that begins inside the game, including when
a hardware keyboard supplies text without an on-screen keyboard. It clears the
flag before queuing the touch, then waits 180 ms before resigning first responder
(also for a guest hide request). A tap on the field makes the guest request the
keyboard again in that interval, so UIKit keeps editing without a hide/show flicker.

## Rendering and memory

- **The game never deletes its shaders.** It deletes programs but not the shaders attached to them,
  and it rebuilds three programs each time the app comes back from the background. `husk-tl-egl.c`
  deletes a program's shaders together with it.
- On macOS, Metal objects are autoreleased on the GL thread, which has no autorelease pool of its own:
  `husk-tl-egl-pool.m` drains one around every swap (without it, ~16 MB/h of growth).
- After coming back from the background the game shows its own pause menu.
- **The screen's cutout.** The game keeps its buttons clear of a camera hole or the Dynamic Island
  through `Graphics_GetSafeRect`: four insets (left, right, top, bottom), not a rectangle (a rectangle
  pushes the title's controls off screen). It reads them once, while it starts, in its own layout
  units: 600 to the surface's height (156 moved the map's buttons 208 px on an 804 px tall surface).
  Android passes half the larger side inset in both left and right. Hosts give the driver pixels
  (`tl_pvz2_set_safe_insets`) and the driver converts; the iOS host passes the part of the view's
  safe area that overlaps the game, left and right only.

## The mod's performance overlay

Reflourished's own (`libRFLib`, the `EnablePerfOverlay` setting). Its memory line asks the game's
platform layer for total memory (answered from `sysconf`: the real RAM) and used memory (live).
The runtime's made-up `/proc/meminfo` and `/proc/self/status` are not what it shows.

## Other things that cost time

- `Resources_GetAssetFileSize` must return -1 for a missing asset (0 hangs start-up).
- The game's ContentDownloader reads the CDN archive through zlib's `gz*` functions. The
  archive is gzip despite its `.tar` suffix; the HTTP bridge must pass its bytes unchanged.

Android's package metadata in the pinned APK is version code `675`, version name `1.4.0` (separate
from the mod's `VERSION`). File-system resource getters return block count, available blocks and
block size for the volume containing the app data, matching Android `StatFs` units.

## Live CDN and the server clock

The shared `husk-tl-cdn.m` helper fetches HTTPS requests only to
`reflourished.ducnguyen.org` (also checked on redirects), using an ephemeral NSURLSession
with a 30-second idle timeout (as Android) and a 60-second total, since start-up waits on
the loading screen for the reply. The worker queues status, headers and body; `husk-tl-pvz2.c`
delivers the game's response/data/completion callbacks on its GL thread between frames,
with the same 16 KiB data chunks as Android. Other hosts still fail immediately.

**The response's `Date` is required even when the archive has not changed.** In 1.4.2,
`0x15398fc` reads that header, parses RFC 1123 GMT, and records the server timestamp;
`0x111b0d8` consumes it when `UseHTTPTime` is enabled. Clock state is at the singleton
pointer `0x256a010`: server epoch +0x48, synchronization frame +0x50, device-time mode
+0x64. `0x111b26c` checks synchronization; `0x111b408` reads server time. Thymed Events
check synchronization/device-time mode (`0x13ff3f0`); the market scheduler also uses this
clock. Serving only the tar bytes with an empty Date leaves timed content unavailable.

A successful gzip download atomically replaces `No_Backup/1.4.2.main.tar`; its real
headers and body hash are kept in `.cdn.plist` beside it. Startup retains that cache unless
a different bundled archive has a newer modification time. The last bundled hash in the
sidecar prevents the same old bundle from replacing a downloaded update on every launch.
A failed/timed-out download, invalid gzip or invalid Date falls back to the local tar,
with a fresh **device-clock UTC Date**, so offline events follow the device clock. Cached
headers are reused only when their body hash matches. `TL_PVZ2_CDN=offline` forces this
path without a network request. `TL_PVZ2_MAIN_TAR` overrides the writable cache path and
skips bundle staging, for tests with their own archive.
