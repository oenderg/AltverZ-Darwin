# Releasing

Releases are built by GitHub Actions from the user-supplied APK link, and published as a GitHub
**prerelease** with slim and full apps and their release notes. The AltStore/SideStore source and the
Mac app's update feed are updated in the same run when the slim apps are built.

AltVerZ-Darwin is based on [Reflourished-Darwin](https://github.com/oenderg/Reflourished-Darwin);
its release procedure is the same. This repository's workflow builds the `altverz` profile only.

## Running the build

First write the release's What's new in `release-notes/<version>.md` and push it: the run stops at
its first step without it ([release-notes/README.md](../release-notes/README.md)). Then
Actions > **Build** > Run workflow, with:

| Input | Meaning |
|---|---|
| `apk_url` | a link to the PvZ2 AltVerZ 1.8.3f APK (direct, Google Drive or MEGA); empty = the repository's own copy (below) |
| `obb_url` | optional; empty = the game's own 1.8.3f OBB named in `data/game-1.8.3f.json` (only needed for full builds) |
| `platforms` | iPhone/iPad and Mac, or one of them |
| `variants` | Slim and full (default), Slim, or Full |
| `publish` | publish the prerelease and update the AltStore source and Mac feed; off = build only |
| `release_tag` | optional; empty = `v<app version>` (for example `v0.1.0`, from the profile) |

The workflow sets `PROFILE=altverz`, so the app name, bundle id, data folder, game manifest and
artifact prefix all come from `data/profiles/altverz.json` ([MOD-PROFILES.md](MOD-PROFILES.md)).
The app version is the profile's `app_version` (`0.1.0` for the first build), not `VERSION`; `VERSION`
remains the fallback for a build without a profile.

**The repository's own APK.** The mod's download hosts are unreliable for a machine, so this
repository keeps the APK as an asset of a **draft** release named "Build inputs (never publish)"
(tag `build-inputs`, which a draft does not create). A draft is visible only to people with write
access to the repository, and to the workflow's own token, so nothing is published. The workflow
downloads `PvZ2-AltVerZ-1.8.3f.apk` from it when `apk_url` is empty. For a new game version, attach
its APK to that draft (Releases > the draft > Edit). **Never publish it.** A fork has no such draft:
it passes a link.

The run uses a `macos-26` runner; expect the better part of an hour the first time. For both
variants it builds slim first, packages it, removes intermediate bundles, then builds full with
the same converted game libraries. The build number comes from the version, not the run number.
The release title is `AltVerZ-Darwin <version> · PvZ2 AltVerZ <game> (early build)`, and every
release is marked as a prerelease while the port is young.

A release of both platforms and variants holds:

```text
PvZ2-AltVerZ-<game>-Darwin-<version>-iOS.ipa
PvZ2-AltVerZ-<game>-Darwin-<version>-iOS-Full.ipa
PvZ2-AltVerZ-<game>-Darwin-<version>-Mac.dmg
PvZ2-AltVerZ-<game>-Darwin-<version>-Mac-Full.dmg
```

For example: `PvZ2-AltVerZ-1.8.3f-Darwin-0.1.0-iOS.ipa`. Slim is a small download; the app
downloads the game's data (about 1.1 GB) on first launch. Full has the data inside, for an offline
first launch. Selecting one platform or variant publishes just those files.

`build.sh` keeps its plain local names in `out/`. After each build,
`PROFILE=altverz scripts/package-release.sh out out/release slim` (or `full`) moves the IPA and DMG
to their release names. It keeps the app bundles for testing and refuses to replace an existing
release asset. Package each variant before building the next one. `--reuse-converted` reuses
converted libraries in the same output folder while still making the DMG; `--quick` skips it.
Use reuse only when the APK, converter and hooks have not changed.

The Mac DMG's window (the front yard, an "Install" panel, the app and an Applications link) is
laid out by `dmgbuild` (`pip install dmgbuild`; CI installs 1.6.5) from `scripts/dmg/settings.py`
and `scripts/dmg/background.tiff`. That art carries Reflourished's name, so the `altverz` profile
sets `dmg_art` to false and gets a plain DMG until AltVerZ art exists. Without dmgbuild,
`scripts/make-dmg.sh` warns and makes a plain DMG.

The release's text is `scripts/release-notes.py`'s: What's new from `release-notes/<version>.md`,
the feat/fix/perf commits since the previous tag (folded, with the compare link; the checkout
fetches the tags for this), and `release-notes/footer.md` with a table of the files the run built.
The same What's new is the AltStore changelog (`--format text`) and the Mac update window's notes
(`--format html`). The workflow uploads the files, then runs `scripts/update-altstore-source.py`
and commits the updated `altstore/source.json` (`chore(altstore): ...`) to the default branch, then
signs the slim Mac DMG and commits the AltVerZ Mac update feed `sparkle/appcast.xml`
(`chore(sparkle): ...`).

## Versions

- `data/profiles/altverz.json` holds AltVerZ's app version (`0.1.0`). `data/game-1.8.3f.json`
  holds the game's version. The `reflourished` profile still uses `VERSION` and
  `data/game-1.4.2.json` for local comparison builds; it is not what this workflow publishes.
- Both apps use the profile's version for `CFBundleShortVersionString` and the game's for
  `PVZ2GameVersion`. Mac Settings > About shows both.
- `CFBundleVersion` is `major*10000 + minor*100 + patch`: `0.1.0` is build `100`. Minor and patch
  stay below 100 so the numbers keep their order. `APP_BUILD` can override it with a positive
  integer for a development build; leave it unset for releases. Slim and full use the same number.
- Any new release on the same game version bumps patch or minor. A new game version bumps minor
  and resets patch to zero. Do not reuse a version for a different release.
- The default tag is `v<version>`. Running again with an existing tag uploads into that release;
  use this only to rebuild the same release.

## Signing

- **iPhone/iPad**: the IPA is ad-hoc signed. Every sideloader (AltStore, SideStore, Sideloadly,
  LiveContainer) re-signs it with the installing person's own Apple ID, so updates keep their data
  as long as the app ID (`io.github.oenderg.altverz-darwin`) stays the same.
- **Mac**: the same app ID, `io.github.oenderg.altverz-darwin`. Saves stay in the same data folder
  across updates. `BUNDLE_ID` overrides the ID on either platform for test or device builds.
- **Mac**: the app is ad-hoc signed and not notarized, so macOS refuses the first open; the player
  clicks Open Anyway in System Settings > Privacy & Security (right-click > Open no longer does it).
  With a Developer ID certificate and notarization that step goes away; settings and saves carry
  over (they do not depend on who signed the app).

## Mac updates (Sparkle)

The Mac app updates itself with [Sparkle](https://sparkle-project.org) 2: PvZ2 AltVerZ >
Check for Updates…, a daily check (Settings > About > Updates turns it off), and Sparkle's own
window, which asks before it installs. `build.sh` fetches the pinned Sparkle
(`scripts/fetch-sparkle.sh`, checked by its SHA-256) and the app embeds it.

- **The feed** is `sparkle/appcast.xml` on the default branch, at
  `https://raw.githubusercontent.com/oenderg/AltverZ-Darwin/main/sparkle/appcast.xml` (the
  profile's `sparkle_feed_url`, the app's `SUFeedURL`). An AltVerZ build never reads
  Reflourished's feed. It offers one update, the newest release's **slim** DMG, whatever variant a
  person installed: a full app clones its OBB into the data folder on its first launch, so after an
  update it is a slim app with its game data in place. A release with a new game version makes the
  updated app download the new OBB on its first start, and the old one is deleted.
  `scripts/update-appcast.py` writes the feed from the DMG; `--check` validates it, and accepts the
  empty feed a new repository starts with.
- **The key.** Every update is signed with an EdDSA key; the app carries its public half
  (`sparkle/public-key`, the plist's `SUPublicEDKey`) and installs only updates signed with the
  private half. **The key is the same as Reflourished-Darwin's and must never change**: an app can
  only accept an update signed with it. The private key is in the owner's login keychain and in the
  repository's Actions secret `SPARKLE_PRIVATE_KEY`. A run without the secret builds and publishes
  as usual and leaves the feed alone. Back it up with `generate_keys -x <file>` (and keep that file
  somewhere safe, not in the repository); `generate_keys -f <file>` imports it on another Mac.
- A build without `sparkle/public-key` (or `SPARKLE_PUBLIC_KEY`) has no updater and no menu item.
  `SPARKLE_FEED_URL` points a test build at another feed (see [TESTING.md](TESTING.md)); the
  profile's URL is the default.

## The AltStore / SideStore source

`altstore/source.json` lists the app and its versions, newest first. It starts with no versions;
the workflow adds each release from the slim IPA. A Full-only run leaves the source alone. The
download URL uses the IPA's actual file name, and the changelog names both versions. Until AltVerZ
has its own icon art, the source and app icons point at the owner's GitHub avatar; replace both
`iconURL` values when the art exists.

People add the raw URL of that file on the default branch to AltStore or SideStore once and get
updates from there. `scripts/update-altstore-source.py --check` validates the file. For a local
check, pass `--source` with a copy of the JSON so the real source stays unchanged.

## Before a release

- Build both apps locally from a clean clone (`./build.sh --profile altverz --apk ...`) and play a
  level on the Mac and on a device. Follow [TESTING.md](TESTING.md); a device test needs the owner's
  permission.
- Dump both Info.plists and check the ID, app version, build number and game version. Check the Mac
  icon and that `SUFeedURL` is `.../AltverZ-Darwin/main/sparkle/appcast.xml` and
  `SUPublicEDKey` is the repository's key. Package each variant, then check its names and the
  AltStore download URL.
- Note in the release text what changed and anything untested. Keep the "early build" note at the
  top until the port has had real testing on both platforms.
