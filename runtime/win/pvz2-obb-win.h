/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's OBB on Windows: the same data the Mac and iPad slim apps fetch on
 * their first start. It goes where the game looks first,
 * <data>/sdcard/Android/obb/com.ea.game.pvz2_rfl/, and it is checked (size and
 * sha256) before the game sees it.
 *
 * The values come from data/game-1.4.2.json, the build the port supports, and
 * they are the ones build.sh writes into the Mac app's Info.plist (the first
 * of obb_urls, obb_size, obb_sha256, obb_name). A new game version changes the
 * JSON and this header together.
 */
#ifndef PVZ2_OBB_WIN_H
#define PVZ2_OBB_WIN_H

#define PVZ2_OBB_NAME "main.675.com.ea.game.pvz2_rfl.obb"
#define PVZ2_OBB_SIZE 1341358080LL
#define PVZ2_OBB_SHA256 "aa747ac0c55f8645cffdc2c93b1469a26ed9ada22a718da8eeb833fffa4c7148"
#define PVZ2_OBB_URL "https://github.com/lantern-fans/obbybackup/releases/download/1.4.2-R1/main.675.com.ea.game.pvz2_rfl.obb"

#endif
