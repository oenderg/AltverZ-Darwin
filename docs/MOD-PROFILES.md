# Mod profiles

The apps run one PvZ2 mod at a time. Reflourished and AltVerZ share the same EA
8.3.0 engine (AltVerZ's `libPVZ2.so` differs only in the build id and five
rodata strings), so almost everything is shared: the converter, the AOT loader,
the Java contract, the driver's game addresses. What differs is the mod profile.

## One source of truth

`data/profiles/<id>.json` names:

| Field | What it decides |
|---|---|
| `game_data`, `hooks` | the pinned game build and the mod's baked hook table |
| `app_name`, `artifact_name`, `bundle_id`, `data_folder`, `app_version` | the app the user installs and the folder its saves live in |
| `package_name`, `android_version_code/name` | what the guest is told it runs on |
| `mod_library`, `mod_jni_onload` | `libRFLib.so` (no `JNI_OnLoad`) or `libSnowie.so` (has one) |
| `cdn_hosts`, `cdn_archive`, `cdn_path` | which live-content host the HTTP bridge may reach and which response is the cacheable archive |
| `icon_path` | the icon taken from the APK |
| `sparkle`, `sparkle_feed_url` | whether the Mac build gets an updater, and the feed it reads (AltverZ and Reflourished each have their own) |
| `dmg_art` | whether the Mac disk image uses the window art in `scripts/dmg/` (Reflourished's carries its name, so AltVerZ uses a plain image until it has its own) |

`scripts/profile.sh` reads it for the shell, and `scripts/profile-generate.py`
writes `pvz2-profile-generated.h` (found by `pvz2-profile.h` through
`__has_include`) plus `Pvz2Profile.swift`. The C driver, the CDN helper, both
hosts, the settings pane and the build scripts all read those values. A build
without a generated header uses Reflourished's values, so compiling one file by
hand still works.

## Build

```sh
./build.sh --profile altverz --apk "AltVerZ_1.8.3f.apk" --obb "main.675.com.ea.game.pvz2_auz.obb" --mac
./build.sh --profile reflourished --apk "PvZ2 Reflourished 1.4.2.apk" --obb "main.675.com.ea.game.pvz2_rfl.obb" --mac
```

`--profile` defaults to `reflourished`. `data/game-<mod version>.json` pins the
APK's library hashes and the OBB; `build.sh` refuses any other build.

## What is still shared

Every `GUEST_*` address in `husk-tl-pvz2.c` and every vtable in
[GAME-NOTES.md](GAME-NOTES.md): the base game's code is identical. AltVerZ's
`libSnowie.so` is loaded after `libPVZ2`'s `JNI_OnLoad`, its constructors run,
then its own `JNI_OnLoad` (which re-runs libPVZ2's constructors, as Android's
`System.loadLibrary` does). Its only Java request, `SnowieConfig`'s feature map,
is answered in the driver.

## Live content

The HTTP bridge only fetches the profile's hosts over HTTPS, including on
redirects. AltVerZ's own first boot named `possthree.pythonanywhere.com` in its
`cdn_config.json.8.3` request; the base ends in `/`, so its paths start with a
double slash. The archive it serves is gzip despite the `.tar` suffix, as
Reflourished's is, so the same validator, cache and fallback are used.
