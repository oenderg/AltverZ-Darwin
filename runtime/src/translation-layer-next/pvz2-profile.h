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
#endif

#endif /* PVZ2_PROFILE_H */
