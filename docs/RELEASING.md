# Releasing

Releases are built by GitHub Actions from the user-supplied APK link, and published as a GitHub
Release with slim and full apps and the app icon. The AltStore/SideStore source is updated in
the same run when a slim iOS app is built.

## Running the build

Actions > **Build** > Run workflow, with:

| Input | Meaning |
|---|---|
| `apk_url` | a link to the PvZ2 Reflourished APK (direct, Google Drive or MEGA) |
| `obb_url` | optional; empty = the mod's own OBB (only needed for full builds) |
| `platforms` | iPhone/iPad and Mac, or one of them |
| `variants` | Slim and full (default), Slim, or Full |
| `release_tag` | optional; empty = `v<VERSION>` (for example `v0.1.0`) |

The run uses a `macos-26` runner; expect the better part of an hour the first time. For both
variants it builds slim first, packages it, removes intermediate bundles, then builds full with
the same converted game libraries. The build number comes from the version, not the run number.
The release title is `Reflourished-Darwin <version> · PvZ2: Reflourished <game>`.

A release of both platforms and variants holds:

```text
PvZ2-Reflourished-<game>-Darwin-<version>-iOS.ipa
PvZ2-Reflourished-<game>-Darwin-<version>-iOS-Full.ipa
PvZ2-Reflourished-<game>-Darwin-<version>-Mac.dmg
PvZ2-Reflourished-<game>-Darwin-<version>-Mac-Full.dmg
PvZ2-Reflourished-icon.png
```

For example: `PvZ2-Reflourished-1.4.2-Darwin-0.1.0-iOS.ipa`. Slim is a small download; the app
downloads the game's data on first launch. Full has the data inside, for an offline first launch.
Selecting one platform or variant publishes just those files and the icon.

`build.sh` keeps its plain local names in `out/`. After each build,
`scripts/package-release.sh out out/release slim` (or `full`) moves the IPA and DMG to their
release names and extracts the icon. It keeps the app bundles for testing and refuses to replace
an existing release asset. Package each variant before building the next one. `--reuse-converted`
reuses converted libraries in the same output folder while still making the DMG; `--quick` skips it.
Use reuse only when the APK, converter and hooks have not changed.

The workflow uploads the files, then runs `scripts/update-altstore-source.py` and commits the
updated `altstore/source.json` (`chore(altstore): ...`) to the default branch.

## Versions

- `VERSION` holds our semver (`0.1.0` initially). `data/game-1.4.2.json` holds the game's version.
- Both apps use our version for `CFBundleShortVersionString` and the game's for `PVZ2GameVersion`.
  Mac Settings > About shows both.
- `CFBundleVersion` is `major*10000 + minor*100 + patch`: `0.1.0` is build `100`. Minor and patch
  stay below 100 so the numbers keep their order. `APP_BUILD` can override it with a positive integer
  for a development build; leave it unset for releases. Slim and full use the same number.
- Any new release on the same game version bumps patch or minor. A new game version bumps minor
  and resets patch to zero. Do not reuse a version for a different release.
- The default tag is `v<version>`. Running again with an existing tag uploads into that release;
  use this only to rebuild the same release.

## Signing

- **iPhone/iPad**: the IPA is ad-hoc signed. Every sideloader (AltStore, SideStore, Sideloadly,
  LiveContainer) re-signs it with the installing person's own Apple ID, so updates keep their data
  as long as the app ID (`io.github.oenderg.reflourished-darwin`) stays the same.
- **Mac**: the same app ID, `io.github.oenderg.reflourished-darwin`. The app imports the pre-release
  Mac settings once before reading them, leaving the old settings untouched. Background tests
  skip this import unless they set `TL_PVZ2_IMPORT_FROM` (see [TESTING.md](TESTING.md)). Saves stay
  in the same data folder. `BUNDLE_ID` overrides the ID on either platform for test or device builds.
- **Mac**: the app is ad-hoc signed and not notarized, so macOS refuses the first open; the player
  clicks Open Anyway in System Settings > Privacy & Security (right-click > Open no longer does it).
  With a Developer ID certificate and notarization that step goes away; settings and saves carry
  over (they do not depend on who signed the app).

## The AltStore / SideStore source

`altstore/source.json` lists the app and its versions, newest first; the workflow adds each release
from the slim IPA. A Full-only run leaves the source alone. The download URL uses the IPA's actual
file name, and the changelog names both versions.

People add the raw URL of that file on the default branch to AltStore or SideStore once and get
updates from there. `scripts/update-altstore-source.py --check` validates the file. For a local
check, pass `--source` with a copy of the JSON so the real source stays unchanged.

## Before a release

- Build both apps locally from a clean clone (`./build.sh --apk ...`) and play a level on the Mac and
  on a device. Follow [TESTING.md](TESTING.md); a device test needs the owner's permission.
- Dump both Info.plists and check the ID, app version, build number and game version. Check the Mac
  icon too. Package each variant, then check its names and the AltStore download URL.
- Note in the release text what changed and anything untested.
