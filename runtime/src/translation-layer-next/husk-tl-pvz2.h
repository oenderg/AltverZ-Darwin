/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Drives Plants vs. Zombies 2: Reflourished (com.ea.game.pvz2_rfl) the way its
 * Java shell does.
 *
 * The game is a NativeActivity-less game: a Java Activity builds an
 * AndroidSurfaceView (a GLSurfaceView), an AndroidGameApp (PopCap's
 * SexyAppFramework bridge) and hands the native library a handful of Java
 * objects.  From then on the framework calls native methods on a GL thread and
 * the native side calls back into Java for paths, device facts and input.
 *
 * This header is the driver's small surface: start it, run the GL loop, send
 * touches.
 */
#ifndef HUSK_TL_PVZ2_H
#define HUSK_TL_PVZ2_H

#include <stdbool.h>
#include <stdint.h>

#include "husk-tl-gameactivity.h"          /* tl_ga_config: the same description of the game and its surface */

#ifdef __cplusplus
extern "C" {
#endif

/* Load the libraries, run JNI_OnLoad, the Reflourished constructors and
 * Native_GameAppInitialize.  Returns false (with the reason logged) on failure. */
bool tl_pvz2_start(const tl_ga_config *cfg);

/* The GL thread: surface created/changed, then a frame per turn. */
bool tl_pvz2_run(void);

/* A touch in surface pixels, y down.  phase 0 down, 1 move, 3 up, 4 cancel
 * (the numbering the engine's own dispatch handler switches on). */
void tl_pvz2_touch(int phase, int id, float x, float y);
void tl_pvz2_text(const char *utf8);
void tl_pvz2_text_action(int action, const char *utf8);
void tl_pvz2_key(int keycode, int down);

/* The keyboard. The guest asks for one through the activity
 * (Device_ShowKeyboard); the host raises it and sends the characters back as
 * text events (tl_pvz2_text_action). A host with no keyboard simply does not
 * register a hook. */
void tl_pvz2_set_keyboard_hook(void (*fn)(int show));
int tl_pvz2_keyboard_showing(void);
/* Report a host dismissal so the guest can end its edit on the game thread. */
void tl_pvz2_keyboard_dismissed(void);

/* The host opens web links on its UI thread and reports whether launch was accepted. */
void tl_pvz2_set_open_url_hook(bool (*fn)(const char *url));

typedef struct {
    int identifier;
    int group_identifier;
    int64_t fire_time_ms;
    int seconds_interval;
    const char *title;
    const char *body;
    const char *sound;
    const char *channel_id;
    const char *user_info;
} tl_pvz2_reminder;

typedef struct {
    void (*schedule)(const tl_pvz2_reminder *reminder);
    void (*cancel)(int identifier);
    void (*cancel_all)(const int *identifiers, int count);
    void (*create_channel)(const char *identifier, const char *name,
                           const char *description, const char *sound);
    void (*delete_channel)(const char *identifier);
    const char *(*timezone)(void);
} tl_pvz2_notification_hooks;

/* Platform notifications are scheduled by the host, never by the shared driver. */
void tl_pvz2_set_notification_hooks(const tl_pvz2_notification_hooks *hooks);
void tl_pvz2_clear_intent_extra_data(void);
void tl_pvz2_set_intent_extra_data(const char *key, const char *value);

/* The game's Import/Export menu (FilePickerHelper).  Android posts the picker
 * to the UI thread and returns; the driver does the same: the request is queued
 * on a worker and the game keeps drawing.  The host presents its own picker
 * and calls tl_pvz2_file_picker_answer() with the chosen path, or NULL when the
 * player cancels.  For exports the driver writes the file (or zip) first and
 * passes its path as `prepared`; the host copies it where the player wants, or
 * gives that path straight back if its picker (iOS) already exported it. */
enum {
    TL_PVZ2_PICK_IMPORT_FILE = 0,
    TL_PVZ2_PICK_IMPORT_TO_DIR = 1,
    TL_PVZ2_PICK_EXPORT_FILE = 2,
    TL_PVZ2_PICK_EXPORT_FOLDER = 3,
    TL_PVZ2_PICK_IMPORT_ZIP = 4,
};

typedef struct {
    void (*open)(int kind, const char *internal_path, const char *suggested_name,
                 const char *prepared_path);
} tl_pvz2_file_picker_hooks;

void tl_pvz2_set_file_picker_hooks(const tl_pvz2_file_picker_hooks *hooks);
void tl_pvz2_file_picker_answer(const char *path);

/* Test bypass, inert unless the variables are set: TL_PVZ2_PICK answers every
 * import with that file, TL_PVZ2_PICK_OUT writes every export there. */
void tl_pvz2_file_picker_configure(const char *files_dir);
void tl_pvz2_file_picker_import_file(const char *name);
void tl_pvz2_file_picker_import_file_to_dir(const char *dir);
void tl_pvz2_file_picker_export_file(const char *source);
void tl_pvz2_file_picker_export_folder(const char *folder);
void tl_pvz2_file_picker_import_zip(const char *dest_parent);

void tl_pvz2_set_paused(bool paused);

/* The surface is a new size: the guest is told, the way Android tells it when
 * the window is resized (onSurfaceChanged -> Native_onSurfaceChanged), and it
 * lays its UI out for the new size. This is the "adapt" mode; the letterbox
 * mode never calls it, because there the surface keeps the game's own size. */
void tl_pvz2_set_surface_size(int w, int h);

/* The frame rate. The display's own maximum (120 on an iPad Pro), and the
 * rate the host presents at -- which the game's own "Target 60 FPS" setting
 * chooses, through Graphics_SetFramerate. */
void tl_pvz2_set_max_fps(int fps);
int tl_pvz2_target_fps(void);

/* The guest's lifecycle. It saves when it is told the app is going away, so
 * the host must tell it: the app going to the background, and coming back.
 * The call is performed on the guest's own thread, and waits for it. */
void tl_pvz2_lifecycle_background(void);
void tl_pvz2_lifecycle_foreground(void);

/* The device turned. The guest's vocabulary: 4 = landscape, 3 = landscape
 * turned over (the 180-degree flip), 1 = portrait, 2 = portrait turned over.
 * The guest re-lays-out when the value changes, so the host must set it. */
void tl_pvz2_set_orientation(int orientation);
/* The part of the surface the screen's cutout covers (the Dynamic Island, the
 * rounded corners), in the surface's pixels; zero where nothing does. The game
 * keeps its buttons clear of it, as on an Android phone with a camera hole. */
void tl_pvz2_set_safe_insets(int left, int right, int top, int bottom);
unsigned long tl_pvz2_frames(void);
/* 1 once the game is past its start-up (its save loaded, the frames smooth). */
int tl_pvz2_started(void);

/* The native shovel: the guest's own "the shovel button was pressed"
 * (ShovelUI::OnMouseDown, guest 0x13ab0a0). Off by default: 0 is the
 * ordinary tap at the shovel's place, 1 is the touch at the button the guest
 * itself reports, and 2 is the guest's own function. The call happens on the
 * game's own thread, in the draw loop. */
/* A touch the HOST injects (a shortcut). It is tracked, so a finger that is
 * left down is released with a line in the log: a stuck finger makes every
 * later tap do nothing. */
void tl_pvz2_touch_injected(int phase, int id, float x, float y);

/* The native seed pick: the game's own "this card was pressed" (the card's
 * own pointer-down handler), on the game's own thread. The request is posted
 * and done there; no touch is injected. */
void tl_pvz2_set_seed_pick(int on);
void tl_pvz2_request_seed_pick(int slot);

/* Collect the current suns and real coins through their native click/flight
 * path, on the game thread. Ignored outside unpaused, unobscured live play. */
void tl_pvz2_request_collect_sun(void);

/* Escape: the game's pause button in live play, Android's Back anywhere else. */
void tl_pvz2_request_escape(void);

void tl_pvz2_set_native_shovel(int mode);
void tl_pvz2_request_native_shovel(void);

/* The memory warning: the guest's own LawnApp::LowMemoryWarning (guest
 * 0x93fcb0), which nobody calls. The request is posted and done on the game's
 * own thread. Off by default (the guest's function is experimental). */
void tl_pvz2_set_low_memory_warning(int on);
void tl_pvz2_request_low_memory(void);

/* The guest's own UI geometry, in the surface's space: the seed tray's rect and
 * the shovel button's rect, each x, y, w, h.
 *
 * The guest is asked for them (its own widget tree, on its own thread, twice a
 * second), so the shortcuts do not depend on a measurement of one level. False
 * when the guest has not told us -- before the first frame, on a build without
 * the converted libraries, or where the app object cannot be read -- and the
 * host then uses its measured numbers. */
bool tl_pvz2_ui_geometry(int *tray, int *shovel);

#ifdef __cplusplus
}
#endif

#endif
