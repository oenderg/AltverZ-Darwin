/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The mod this build is for. The build scripts write pvz2-profile-generated.h from
 * data/profiles/<id>.json and put its directory on the include path; without one (a
 * hand compile of a single file) these are Reflourished's values, the build this
 * repository started as.
 */
#ifndef PVZ2_PROFILE_H
#define PVZ2_PROFILE_H

#if __has_include("pvz2-profile-generated.h")
#include "pvz2-profile-generated.h"
#else
#define PVZ2_PROFILE_ID "reflourished"
#define PVZ2_PROFILE_APP_NAME "PvZ2 Reflourished"
#define PVZ2_PROFILE_ARTIFACT "PvZ2-Reflourished"
#define PVZ2_PROFILE_BUNDLE_ID "io.github.oenderg.reflourished-darwin"
#define PVZ2_PROFILE_DATA_FOLDER "PvZ2 Reflourished"
#define PVZ2_PROFILE_PKG "com.ea.game.pvz2_rfl"
#define PVZ2_PROFILE_VERSION_CODE 675
#define PVZ2_PROFILE_VERSION_NAME "1.4.0"
#define PVZ2_PROFILE_MOD_LIBRARY "libRFLib.so"
#define PVZ2_PROFILE_MOD_LIBRARY_BASE "libRFLib"
#define PVZ2_PROFILE_MOD_JNI_ONLOAD 0
#define PVZ2_PROFILE_CDN_ARCHIVE "1.4.2.main.tar"
#define PVZ2_PROFILE_CDN_PATH "/cdn/1.4.2.main.tar"
#define PVZ2_PROFILE_CDN_HOSTS "reflourished.ducnguyen.org"
#define PVZ2_PROFILE_WIN_STEM "PvZ2Reflourished"
#define PVZ2_PROFILE_AUMID "io.github.oenderg.reflourished"
#define PVZ2_PROFILE_REPO "oenderg/Reflourished-Darwin"
#define PVZ2_PROFILE_GAME_VERSION "1.4.2"
#define PVZ2_PROFILE_OBB_NAME "main.675.com.ea.game.pvz2_rfl.obb"
#define PVZ2_PROFILE_OBB_SIZE 1341358080LL
#define PVZ2_PROFILE_OBB_SHA256 "aa747ac0c55f8645cffdc2c93b1469a26ed9ada22a718da8eeb833fffa4c7148"
#define PVZ2_PROFILE_OBB_URL "https://github.com/lantern-fans/obbybackup/releases/download/1.4.2-R1/main.675.com.ea.game.pvz2_rfl.obb"
#endif

#endif /* PVZ2_PROFILE_H */
