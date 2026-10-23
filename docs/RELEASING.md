# Releasing

Releases are built by GitHub Actions from the user-supplied APK link, and published as a GitHub
Release with the IPA, the Mac disk image and the app icon. The AltStore/SideStore source is updated
in the same run.

## Running the build

Actions > **Build** > Run workflow, with:

| Input | Meaning |
|---|---|
| `apk_url` | a link to the PvZ2 Reflourished APK (direct, Google Drive or MEGA) |
| `obb_url` | optional; empty = the mod's own OBB (only needed for full, non-slim builds) |
| `platforms` | iPhone/iPad and Mac, or one of them |
| `slim` | slim apps (default): about 60 MB to download, they download the OBB on first launch |
| `release_tag` | the tag of the release that receives the files (it is created if missing) |

The run (on a `macos-26` runner; expect the better part of an hour the first time) does `./build.sh` with the matching options,
uploads `PvZ2-Reflourished.ipa`, `PvZ2-Reflourished.dmg` and `PvZ2-Reflourished-icon.png` to the
release, then runs `scripts/update-altstore-source.py` and commits the updated `altstore/source.json`
(`chore(altstore): ...`) to the default branch.

## Versions

- The app version is the game's (`APP_VERSION`, default `1.4.2`); the build number is the workflow's
  run number (`APP_BUILD`). Sideloaders and AltStore compare build numbers, so never reuse one.
- One tag per release (`v1.4.2-<n>` style is fine). Running again with an existing tag uploads into
  that same release.

## Signing

- **iPhone/iPad**: the IPA is ad-hoc signed. Every sideloader (AltStore, SideStore, Sideloadly,
  LiveContainer) re-signs it with the installing person's own Apple ID, so updates keep their data
  as long as the app ID (`com.reflourished.darwin`) stays the same.
- **Mac**: the app is ad-hoc signed and not notarized, so macOS refuses the first open; the player
  clicks Open Anyway in System Settings > Privacy & Security (right-click > Open no longer does it). With a Developer ID
  certificate and notarization that step goes away; settings and saves carry over (they do not depend
  on who signed the app).

## The AltStore / SideStore source

`altstore/source.json` lists the app and its versions, newest first; the workflow adds each release.
People add the raw URL of that file on the default branch to AltStore or SideStore once and get
updates from there. `scripts/update-altstore-source.py --check` validates the file.

## Before a release

- Build both apps locally from a clean clone (`./build.sh --apk ...`) and play a level on the Mac and
  on a device.
- Note in the release text what changed and anything untested.
