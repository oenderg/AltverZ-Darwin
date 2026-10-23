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

## Rendering and memory

- **The game never deletes its shaders.** It deletes programs but not the shaders attached to them,
  and it rebuilds three programs each time the app comes back from the background. `husk-tl-egl.c`
  deletes a program's shaders together with it.
- On macOS, Metal objects are autoreleased on the GL thread, which has no autorelease pool of its own:
  `husk-tl-egl-pool.m` drains one around every swap (without it, ~16 MB/h of growth).
- After coming back from the background the game shows its own pause menu.

## The mod's performance overlay

Reflourished's own (`libRFLib`, the `EnablePerfOverlay` setting). Its memory line asks the game's
platform layer for total memory (answered from `sysconf`: the real RAM) and used memory (live).
The runtime's made-up `/proc/meminfo` and `/proc/self/status` are not what it shows.

## Other things that cost time

- The name field's ACCEPT is enabled by the ENTER key event (Android keycode 66, down and up), not
  by the text.
- `Resources_GetAssetFileSize` must return -1 for a missing asset (0 hangs start-up).
- The game's ContentDownloader reads the CDN archive through zlib's `gz*` functions; the archive is
  bundled in the apps and staged into `No_Backup/1.4.2.main.tar`, so the mod's content applies offline.
