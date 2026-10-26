# Contributing

Thanks for helping. This page is how to build, change and test the project, and what a good pull
request looks like. If you use an AI agent, point it at [AGENTS.md](AGENTS.md).

## What you need

- A Mac with Apple silicon and Xcode (with its iOS platform, for iPhone/iPad builds), python3,
  about 8 GB free.
- The **PvZ2 AltVerZ 1.8.3f APK** from the mod's official downloads. `build.sh` checks that it
  is exactly that build. The game's OBB (about 1.1 GB) is downloaded for you, or by the slim apps
  themselves.
- For trying things on an iPhone/iPad: a sideloader (AltStore, SideStore, Sideloadly, LiveContainer)
  or your own signing identity.

## The loop

```sh
./build.sh --profile altverz --mac --apk "AltVerZ_1.8.3f.apk"           # first time: converts the game's code
# ... edit runtime/ ...
./build.sh --profile altverz --mac --quick --apk "AltVerZ_1.8.3f.apk"   # rebuilds the app only, in seconds
```

- `--quick` reuses the converted libraries in `out/.work`. Drop it when you change `converter/` or
  `data/`, or when in doubt.
- Run the Mac app from the command line with a data folder of its own, so your own save and settings
  are never touched, and drive it with a tap script: [docs/TESTING.md](docs/TESTING.md).
- For iPhone/iPad, `./build.sh --profile altverz --ios --slim --apk ...` gives
  `out/PvZ2-AltVerZ.ipa`; sideload it. To sign it yourself, `SIGN_IDENTITY`, `SIGN_PROFILE` (and
  `SIGN_KEYCHAIN`) are passed through to `runtime/tools/regress/build-ios-app.sh`; `BUNDLE_ID` sets
  the app ID for a test copy on either platform. The public ID is
  `io.github.oenderg.altverz-darwin` on both.
- The app's log: `~/Library/Application Support/PvZ2 AltVerZ/pvz2.log` on the Mac (a test run
  writes to its `TL_PVZ2_LOG` instead), and `Documents/pvz2.log` on iOS (Files app: On My iPhone >
  PvZ2 AltVerZ). Each launch starts a new one; the run before is `pvz2.previous.log` next to it.

AltVerZ's app version is in `data/profiles/altverz.json`; the game version is in
`data/game-1.8.3f.json`. Local file names stay the same. For versioned release names and packaging,
see [docs/RELEASING.md](docs/RELEASING.md). `--profile reflourished` builds the base mod for
comparison; it is not what this repository publishes.

## Where to make a change

| You want to... | Look in |
|---|---|
| change what the game is told or how it is driven (lifecycle, input, keys, pause, collect) | `runtime/src/translation-layer-next/husk-tl-pvz2.c` |
| change the Mac window, menus, settings, keyboard | `runtime/tools/pvz2-macos.m`, `pvz2-macos-settings.swift`, `pvz2-keys.h` |
| change the iPhone/iPad app | `runtime/tools/pvz2-ios.m` |
| fix an Android function the game calls (libc, JNI, GL, audio) | the matching `runtime/src/translation-layer-next/husk-tl-*.c` |
| change how the game's code is converted | `converter/` ([docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), "The converter") |
| add a mod or support a new game version | [docs/MOD-PROFILES.md](docs/MOD-PROFILES.md), [docs/NEW-GAME-VERSION.md](docs/NEW-GAME-VERSION.md) |
| change the CI build or a release | `.github/workflows/build.yml`, [docs/RELEASING.md](docs/RELEASING.md) |

The runtime is derived from Husk, which supports many engines; this repo carries only the files PvZ2
needs (`runtime/tools/regress/pvz2-sources.sh`, `runtime/tools/pvz2-standins.c`).

## A good pull request

- **One change**, described in plain words: what was wrong or missing, what you changed, why.
- **Proof it works**: a screenshot or log line from the Mac app, and from a device if iOS is touched.
  "It builds" is not proof. Say what you could not test.
- Both apps still build: `./build.sh --profile altverz --apk ...` (without `--quick`).
- No game files, no personal data (paths under your home folder, device IDs, signing identities).
- The title follows [Conventional Commits](https://www.conventionalcommits.org): `fix(mac): ...`,
  `feat(keys): ...`, `perf(gl): ...`, `docs: ...`. Scopes in use: `mac`, `ios`, `keys`, `driver`,
  `gl`, `audio`, `converter`, `build`, `ci`, `altstore`, `sparkle`.
- Comments explain *why*, in full sentences, like the code around them.

## Reporting a bug

Use the issue form: device and OS, the app's version (Mac: Settings > About), what you did, what
happened, and the `pvz2.log` from right after it happened (`pvz2.previous.log` if the app crashed or
was opened again since).
