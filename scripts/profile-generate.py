#!/usr/bin/env python3
"""Write one mod profile as the C header and Swift constants the app compiles against.

    profile-generate.py data/profiles/altverz.json <dir>

The generated pvz2-profile.h is found before the checked-in defaults through -I, so
every string the driver or a host needs comes from the profile and nowhere else.
"""
import json
import os
import sys


def cstr(value):
    return json.dumps(str(value), ensure_ascii=False)


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: profile-generate.py <profile.json> <out-dir>")
    profile = json.load(open(argv[1]))
    out = argv[2]
    os.makedirs(out, exist_ok=True)

    macros = {
        "PVZ2_PROFILE_ID": cstr(profile["id"]),
        "PVZ2_PROFILE_APP_NAME": cstr(profile["app_name"]),
        "PVZ2_PROFILE_ARTIFACT": cstr(profile["artifact_name"]),
        "PVZ2_PROFILE_BUNDLE_ID": cstr(profile["bundle_id"]),
        "PVZ2_PROFILE_DATA_FOLDER": cstr(profile["data_folder"]),
        "PVZ2_PROFILE_PKG": cstr(profile["package_name"]),
        "PVZ2_PROFILE_VERSION_CODE": str(int(profile["android_version_code"])),
        "PVZ2_PROFILE_VERSION_NAME": cstr(profile["android_version_name"]),
        "PVZ2_PROFILE_MOD_LIBRARY": cstr(profile["mod_library"]),
        "PVZ2_PROFILE_MOD_LIBRARY_BASE": cstr(profile["mod_library"][:-3] if profile["mod_library"].endswith(".so") else profile["mod_library"]),
        "PVZ2_PROFILE_MOD_JNI_ONLOAD": "1" if profile.get("mod_jni_onload") else "0",
        "PVZ2_PROFILE_CDN_ARCHIVE": cstr(profile["cdn_archive"]),
        "PVZ2_PROFILE_CDN_PATH": cstr(profile.get("cdn_path", "")),
        "PVZ2_PROFILE_CDN_HOSTS": cstr("|".join(profile.get("cdn_hosts", []))),
    }
    with open(os.path.join(out, "pvz2-profile-generated.h"), "w") as f:
        f.write("/* Generated from %s by scripts/profile-generate.py -- do not edit. */\n" % argv[1])
        f.write("#ifndef PVZ2_PROFILE_GENERATED_H\n#define PVZ2_PROFILE_GENERATED_H\n")
        for name, value in macros.items():
            f.write("#define %s %s\n" % (name, value))
        f.write("#endif\n")

    with open(os.path.join(out, "Pvz2Profile.swift"), "w") as f:
        f.write("// Generated from %s by scripts/profile-generate.py -- do not edit.\n" % argv[1])
        f.write("let pvz2ProfileAppName = %s\n" % json.dumps(profile["app_name"]))
        f.write("let pvz2ProfileArtifact = %s\n" % json.dumps(profile["artifact_name"]))
        f.write("let pvz2ProfileId = %s\n" % json.dumps(profile["id"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
