# Game notes (Reflourished 1.4.2)

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
| 0x64f998 | a collectable's own "clicked" request (the game picks the flight and awards the value) | Space collects suns (vtable 0x2395208) and real coins (0x23bb9b8) only |
| 0x75a96c | a sun's value | the log line |
| 0x5beb44, 0x5c1788 | find a UI widget by name, and its rect in the app's design space | the pause button (`UIPauseButton`), the shovel, the seed tray |
| 0x24bcf00 | the pause menu (`PauseDialog`); its rect is at +0x44 | Escape on the pause menu taps RESUME (82% across, 90% down) |

- **Android's Back does nothing in a level** in this game, so Escape presses the pause button
  instead (and RESUME on the pause menu). Elsewhere it is Back.
- Every action is refused, with a log line saying why, unless the board is live, nothing is paused
  and no modal is up. Never write the game's sun, currency or object state directly: call the game's
  own functions and let it do its bookkeeping.

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
