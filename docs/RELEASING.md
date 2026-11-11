# Releasing

Releases are built by GitHub Actions from the user-supplied APK link, and published as a GitHub
Release with slim and full apps and their release notes. The AltStore/SideStore source and the Mac
app's update feed are updated in the same run when the slim apps are built.

## Running the build

First write the release's What's new in `release-notes/<version>.md` and push it: the run stops at
its first step without it ([release-notes/README.md](../release-notes/README.md)). Then
Actions > **Build** > Run workflow, with:

| Input | Meaning |
|---|---|
| `apk_url` | a link to the PvZ2 Reflourished APK (direct, Google Drive or MEGA; the Windows jobs take a direct link only); empty = the repository's own copy (below) |
| `obb_url` | optional; empty = the mod's own OBB (only needed for full builds) |
| `platforms` | All (default: iPhone/iPad, Mac and Windows), iPhone/iPad and Mac, one of those, or Windows |
| `variants` | Slim and full (default), Slim, or Full |
| `release_tag` | optional; empty = `v<VERSION>` (for example `v1.0.0`) |

**The repository's own APK.** The mod's only download hosts (Google Drive and MEGA) are unreliable
for a machine, so this repository keeps the APK as an asset of a **draft** release named
"Build inputs (never publish)" (tag `build-inputs`, which a draft does not create). A draft is
visible only to people with write access to the repository, and to the workflow's own token, so
nothing is published. The workflow downloads `PvZ2-Reflourished-<game version>.apk` from it when
`apk_url` is empty. For a new game version, attach its APK to that draft (Releases > the draft >
Edit). **Never publish it.** A fork has no such draft: it passes a link.

The run uses GitHub's `xcode-27` runner (Xcode 27, the same SDK as a local build; a preview image, so a run can queue); expect the better part of an hour the first time. For both
variants it builds slim first, packages it, removes intermediate bundles, then builds full with
the same converted game libraries. The build number comes from the version, not the run number.
The release title is `Reflourished-Darwin <version> · PvZ2: Reflourished <game>`.

A release of all platforms and both variants holds:

```text
PvZ2-Reflourished-<game>-Darwin-<version>-iOS.ipa
PvZ2-Reflourished-<game>-Darwin-<version>-iOS-Full.ipa
PvZ2-Reflourished-<game>-Darwin-<version>-Mac.dmg
PvZ2-Reflourished-<game>-Darwin-<version>-Mac-Full.dmg
PvZ2-Reflourished-<game>-Darwin-<version>-Windows-Setup.exe
PvZ2-Reflourished-<game>-Darwin-<version>-Windows-x64.zip
PvZ2-Reflourished-<game>-Darwin-<version>-Windows-ARM64.zip
```

**Windows** runs beside the Apple job, in three jobs: `windows-arm64` (macOS) cross-builds the
ARM64 programs with llvm-mingw; `windows` (`windows-2025`) installs MSYS2's CLANG64 at the
versions `runtime/win/x64/msys2-clang64.txt` pins, builds ScarletNX with our patch (cached
between runs), the x86-64 programs, the setup with Inno Setup 7.1.1 and both portable zips;
`windows-publish` waits for the Apple job, attaches the three files to its release, rewrites the
release text so the table lists them (`release-notes.py --assets`), signs the setup and commits
the Windows update feed (below). The Windows files have no Full variant: the setup and the zips
download the game's data on first start. A Windows-only run with Publish creates the release
when it does not exist yet.

For example: `PvZ2-Reflourished-1.4.2-Darwin-1.0.0-iOS.ipa`. Slim is a small download; the app
downloads the game's data on first launch. Full has the data inside, for an offline first launch.
Selecting one platform or variant publishes just those files.

A run with **Publish** off creates no release, AltStore entry or Mac feed item: it keeps the same files
as artifacts of the run for 14 days instead, unzipped, so anyone signed in to GitHub can download a test
build from the run's page (for example for the reporter of an issue to check a fix).

`build.sh` keeps its plain local names in `out/`. After each build,
`scripts/package-release.sh out out/release slim` (or `full`) moves the IPA and DMG to their
release names. It keeps the app bundles for testing and refuses to replace
an existing release asset. Package each variant before building the next one. `--reuse-converted`
reuses converted libraries in the same output folder while still making the DMG; `--quick` skips it.
Use reuse only when the APK, converter and hooks have not changed.

The Mac DMG's window (the front yard, an "Install" panel, the app and an Applications link) is laid
out by `dmgbuild` (`pip install dmgbuild`; CI installs 1.6.5) from `scripts/dmg/settings.py` and
`scripts/dmg/background.tiff`. Without dmgbuild, `scripts/make-dmg.sh` warns and makes a plain DMG.
To change the picture, edit `scripts/dmg/dmg.html` and run `scripts/dmg/render.sh`; it needs the
game's art and UI font in `scripts/dmg/art/` (decoded from the OBB, never committed), and
`render.sh preview.png` draws the icons in to judge the layout. Finder counts the title bar in the
window height and draws the labels black: check a changed layout in a real Finder window.

Every push to main and every release run first runs `scripts/check-repo.sh` (seconds, no build): the release
notes for `VERSION`, the game version everywhere it is written, the x64 DLLs' licenses, the update key
and the release text. A release run stops there, before building, if anything is off.

The release's text is `scripts/release-notes.py`'s: a **Download:** line with one direct link per
platform (the small IPA, the small DMG, the Windows setup; a platform without a file is left out),
What's new from `release-notes/<version>.md`,
the feat/fix/perf commits since the previous tag (folded, with the compare link; the checkout fetches
the tags for this), and `release-notes/footer.md` with a table of the files the run built. The same
What's new is the AltStore changelog (`--format text`) and the Mac update window's notes
(`--format html`). The workflow uploads the files, then runs `scripts/update-altstore-source.py` and commits the
updated `altstore/source.json` (`chore(altstore): ...`) to the default branch, then signs the slim
Mac DMG and commits the Mac update feed `sparkle/appcast.xml` (`chore(sparkle): ...`).

## Versions

- `VERSION` holds our semver (`1.0.0`, the first release). `data/game-1.4.2.json` holds the game's version.
- Both apps use our version for `CFBundleShortVersionString` and the game's for `PVZ2GameVersion`.
  Mac Settings > About shows both.
- `CFBundleVersion` is `major*10000 + minor*100 + patch`: `1.0.0` is build `10000`. Minor and patch
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

## Mac updates (Sparkle)

The Mac app updates itself with [Sparkle](https://sparkle-project.org) 2: PvZ2 Reflourished >
Check for Updates…, a daily check (Settings > About > Updates turns it off), and Sparkle's own
window, which asks before it installs. `build.sh` fetches the pinned Sparkle
(`scripts/fetch-sparkle.sh`, checked by its SHA-256) and the app embeds it.

- **The feed** is `sparkle/appcast.xml` on the default branch, the app's `SUFeedURL`. It offers
  one update, the newest release's **slim** DMG, whatever variant a person installed: a full app
  clones its OBB into the data folder on its first launch, so after an update it is a slim app
  with its game data in place. A release with a new game version makes the updated app download
  the new OBB on its first start, and the old one is deleted. `scripts/update-appcast.py` writes
  the feed from the DMG; `--check` validates it.
- **The key.** Every update is signed with an EdDSA key; the app carries its public half
  (`sparkle/public-key`, the plist's `SUPublicEDKey`) and installs only updates signed with the
  private half. The private key is in the owner's login keychain (made with Sparkle's
  `bin/generate_keys`, which prints the public key again any time) and in the repository's Actions
  secret `SPARKLE_PRIVATE_KEY`. A run without the secret builds and publishes as usual and leaves
  the feed alone. **Never lose the key**: an app can only accept an update signed with it.
  Back it up with `generate_keys -x <file>` (and keep that file somewhere safe, not in the
  repository); `generate_keys -f <file>` imports it on another Mac.
- **Changing how the Mac app is signed** (a Developer ID and notarization later) is fine: Sparkle
  accepts an update whose code signature changed as long as the EdDSA key is the same. Never
  change the code signature and the EdDSA key in the same release.
- A build without `sparkle/public-key` (or `SPARKLE_PUBLIC_KEY`) has no updater and no menu item.
  `SPARKLE_FEED_URL` points a test build at another feed (see [TESTING.md](TESTING.md)).

## Windows updates (WinSparkle)

The installed Windows build (the setup, not the portable zips) updates itself with WinSparkle:
Check for Updates... in the window menu, a daily check (Settings > About > Updates turns it off),
and WinSparkle's window, which asks before it installs. docs/windows-shipping.md has the details.

- **The feed** is `sparkle/windows-appcast.xml` on the default branch, a separate file from the
  Mac feed so neither release can rewrite the other's. One item, the newest release's setup,
  with one enclosure for `windows-x64` and one for `windows-arm64` (the same file: the setup
  installs either build). `scripts/update-windows-appcast.py` writes it; `--check` validates it.
- **The key** is the Mac app's: the Windows program carries the same `sparkle/public-key`, so
  one private key signs both platforms. Nothing new to keep safe.
- **Publishing.** The release workflow's `windows-publish` job runs
  `scripts/publish-windows-setup.sh` with the `SPARKLE_PRIVATE_KEY` secret on stdin: it signs
  the setup, refuses a signature that does not verify with `sparkle/public-key`, writes the feed
  and commits it (`chore(sparkle-windows): ...`), and the job pushes it. Without the secret the
  feed is left as it is. For a setup made outside the workflow, run the script on the release
  Mac (`--upload --commit`, the key from the login keychain) and push. Until the first Windows
  release, the feed does not exist and an installed build's check reports an error.

## The AltStore / SideStore source

`altstore/source.json` lists the app and its versions, newest first; its icon is
`docs/banner/icon.png` on the default branch (releases carry no icon); the workflow adds each release
from the slim IPA. A Full-only run leaves the source alone. The download URL uses the IPA's actual
file name, and the changelog names both versions.

People add the raw URL of that file on the default branch to AltStore or SideStore once and get
updates from there. `scripts/update-altstore-source.py --check` validates the file. For a local
check, pass `--source` with a copy of the JSON so the real source stays unchanged.

## Before a release

- Build both apps locally from a clean clone (`./build.sh --apk ...`) and play a level on the Mac and
  on a device. Follow [TESTING.md](TESTING.md); a device test needs the owner's permission.
- Dump both Info.plists and check the ID, app version, build number and game version. Check the Mac
  icon and `SUFeedURL` / `SUPublicEDKey` too. Package each variant, then check its names and the AltStore download URL.
- Note in the release text what changed and anything untested.
