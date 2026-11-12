/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's OBB on Windows: the same data the Mac and iPad slim apps fetch on
 * their first start. It goes where the game looks first,
 * <data>/sdcard/Android/obb/<the package>/, and it is checked (size and
 * sha256) before the game sees it.
 *
 * The values come from the profile's game manifest (data/game-<version>.json) through
 * pvz2-profile-generated.h: the first of obb_urls, obb_size, obb_sha256, obb_name, as
 * build.sh writes them into the Mac app's Info.plist.
 */
#ifndef PVZ2_OBB_WIN_H
#define PVZ2_OBB_WIN_H

#include "pvz2-profile.h"

#define PVZ2_OBB_NAME PVZ2_PROFILE_OBB_NAME
#define PVZ2_OBB_SIZE PVZ2_PROFILE_OBB_SIZE
#define PVZ2_OBB_SHA256 PVZ2_PROFILE_OBB_SHA256
#define PVZ2_OBB_URL PVZ2_PROFILE_OBB_URL

#endif
