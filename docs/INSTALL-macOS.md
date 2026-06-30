# Installing Strawberry on macOS

This repository ships an installer script — [`install-macos.sh`](./install-macos.sh) —
that builds the [Strawberry music player](./README.md) from source and installs
it as `/Applications/strawberry.app`, **with full feature parity to the official
sponsor DMG**.

It's designed for **macOS Sequoia (15.x)** on both **Intel (x86_64)** and
**Apple Silicon (arm64)** machines. It is **idempotent**, **robust**, and
**fast to undo**, so you can install → tweak code → reinstall → verify your
changes without thinking about state.

---

## How it achieves feature parity

The official build pipeline (see [`.github/workflows/build.yml`](./.github/workflows/build.yml))
doesn't use Homebrew formulas — it downloads a curated tarball from
`strawberrymusicplayer/strawberry-macos-dependencies[-legacy]` that contains
every C/C++ runtime dep (Qt, GStreamer, libgpod, sparsehash, Sparkle,
qtsparkle, KDSingleApplication, TagLib, ICU, …) plus the project's private
tooling (`macdeployqt`, `macdeploycheck`). The tarball extracts into
`/opt/strawberry_macos_<arch>_release/`.

This script does the same. The result is a build with **every feature the
official DMG ships with**:

```
Devices: Audio CD support             Streaming: Subsonic
Devices: MTP support                  Streaming: Tidal
Devices: iPod classic support         Streaming: Spotify
Discord Rich Presence                 Streaming: Qobuz
EBU R 128 loudness normalization      Moodbar
MusicBrainz integration               Moodbar: Fast spectrum with FFTW3
QPA Platform Native Interface         Song fingerprinting and tracking
Sparkle integration (auto-updater)    Stream tagreader
Translations                          Waveform
```

The only thing the official build does that the script doesn't is sign with
Apple's Developer ID. We use **ad-hoc codesigning** (identity `-`) instead,
which satisfies macOS Sequoia's "every dylib must be signed" requirement
locally without needing an Apple developer cert.

---

## Quick start

```bash
# Full install end-to-end (~100 MB download + ~10 min compile on first run)
./install-macos.sh

# Launch
open -a Strawberry
```

Re-running the same command later only does incremental work
(skips dep download, incremental CMake build, etc.).

To verify a code change quickly:

```bash
./install-macos.sh build    # incremental compile
./install-macos.sh bundle   # re-run macdeployqt + ad-hoc codesign
./install-macos.sh install -y
open -a Strawberry
```

To reset to a clean slate:

```bash
./install-macos.sh purge -y
```

---

## Commands

| Command           | What it does                                                       |
| ----------------- | ------------------------------------------------------------------ |
| `deps`            | Install `cmake` + `pkgconf` via Homebrew, then download & extract  |
|                   | the upstream `strawberry-macos-dependencies` tarball into `/opt`.  |
| `configure`       | Run `cmake -S . -B build` with the same flags as the upstream CI.  |
| `build`           | Configure (if needed) + `cmake --build build` (incremental).       |
| `bundle`          | `make deploy` (macdeployqt + GStreamer plugins) **+ ad-hoc codesign** |
|                   | of every embedded mach-o (166 files in this build).                |
| `install`         | `rsync` `strawberry.app` to `/Applications` (deletes stale files). |
| `uninstall`       | Remove `/Applications/strawberry.app` (prompts about user data).   |
| `clean`           | Delete the build directory only.                                   |
| `purge`           | uninstall + clean + remove user prefs + remove cached tarball +    |
|                   | remove `/opt/strawberry_macos_<arch>_release`.                     |
| `status`          | Show current install/build state and resolved tool paths.          |
| `doctor`          | Diagnose the environment — lists missing dependencies.             |
| `all` *(default)* | `deps → build → bundle → install`.                                 |
| `help`            | Show the embedded usage block.                                     |

## Options

| Option              | Default                             | Notes                                              |
| ------------------- | ----------------------------------- | -------------------------------------------------- |
| `--build-dir DIR`   | `./build`                           | CMake build directory.                             |
| `--prefix DIR`      | `/Applications`                     | Where to copy the `.app` for `install`.            |
| `--build-type TYPE` | `Release`                           | Any valid CMake build type.                        |
| `--jobs N` / `-j N` | `sysctl -n hw.ncpu`                 | Parallel compile jobs.                             |
| `--no-bundle`       | bundle on                           | Skip `macdeployqt` + codesign (faster dev loop).   |
| `--refresh-deps`    | off                                 | Re-download the upstream tarball even if cached.   |
| `--yes` / `-y`      | off                                 | Don't prompt before destructive actions.           |
| `--verbose` / `-v`  | off                                 | `set -x` + extra logging.                          |

---

## What gets installed

### Files under `/opt`

| Location                                                | Created by   | Removed by    |
| ------------------------------------------------------- | ------------ | ------------- |
| `/opt/strawberry_macos_<arch>_release/`                 | `deps`       | `purge` (with `-y`) — needs sudo |

Contains the whole upstream dep tree: Qt 6.11.1, GStreamer 1.28, TagLib,
libgpod, libsparsehash, Sparkle.framework, qtsparkle, KDSingleApplication,
plus `bin/macdeployqt` and `bin/macdeploycheck` etc. ~500 MB on disk.

### Homebrew (only build tools)

`cmake` ≥ 3.13 and `pkgconf` (provides `pkg-config`). Nothing else — all
runtime libraries come from the tarball above. The script deliberately
sets `PKG_CONFIG_LIBDIR=/opt/strawberry_macos_<arch>_release/lib/pkgconfig`
during `cmake configure` so that pkg-config never accidentally picks up a
Homebrew formula (which would cause version drift / runtime crashes).

### Caches & app files

| Location                                                                                     | Created by   | Removed by    |
| -------------------------------------------------------------------------------------------- | ------------ | ------------- |
| `./build/`                                                                                   | `build`      | `clean` / `purge` |
| `./build/strawberry.app/`                                                                    | `build`      | `clean` / `purge` |
| `/Applications/strawberry.app/`                                                              | `install`    | `uninstall` / `purge` |
| `~/Library/Caches/strawberry-install/strawberry-macos-<arch>-release.tar.xz` (~96 MB)        | `deps`       | `purge` |
| `~/Library/Preferences/org.strawberrymusicplayer.strawberry.plist`                           | first launch | `uninstall` (with confirmation) / `purge` |
| `~/Library/Application Support/Strawberry/`                                                  | first launch | `uninstall` (with confirmation) / `purge` |
| `~/Library/Caches/Strawberry/`                                                               | first launch | `uninstall` (with confirmation) / `purge` |
| `~/Library/Saved Application State/org.strawberrymusicplayer.strawberry.savedState`          | first launch | `uninstall` (with confirmation) / `purge` |

---

## Verifying a change

```bash
# 1. Make your code change
# 2. Incremental rebuild + reinstall:
./install-macos.sh build && \
./install-macos.sh bundle && \
./install-macos.sh install -y
# 3. Launch:
open -a Strawberry
```

A typical change → reinstall cycle is **a minute or two** (just the compile of
changed files + re-linking + codesign), not the full ~15 minute first-run cost.

`./install-macos.sh deps` itself runs in ~4 seconds when everything is
already in place.

If you want a guaranteed clean slate:

```bash
./install-macos.sh purge -y && ./install-macos.sh
```

---

## Troubleshooting

Run the doctor first — it explains exactly what's missing:

```bash
./install-macos.sh doctor
```

| Symptom                                                                   | Fix                                                                    |
| ------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| `Extraction did not produce ${DEPS_PREFIX}…`                              | The tarball download is corrupt. `./install-macos.sh --refresh-deps deps`. |
| `Could not find a package configuration file provided by "qtsparkle-qt6"` | Warning only — qtsparkle is the Windows alternative to Sparkle, we explicitly disable it (`-DENABLE_QTSPARKLE=OFF`). |
| `code object is not signed at all`                                        | macdeployqt's final codesign step fails on dylibs it didn't sign. The script catches this and runs an ad-hoc codesign pass over all 166 embedded mach-o files. If the error persists, re-run `./install-macos.sh bundle`. |
| App launches but immediately quits ("damaged")                            | `xattr -dr com.apple.quarantine /Applications/strawberry.app` (the script does this for you, but Gatekeeper sometimes re-flags). |
| `sudo` prompt during `deps` / `purge`                                     | Required — `/opt` and `/Applications/strawberry.app` are root-owned.  |
| First launch fails with "Library not loaded" pointing at `/opt/…`         | You ran with `--no-bundle`. Re-run without it: `./install-macos.sh bundle && ./install-macos.sh install -y`. |

---

## What the script does *not* do

- Apple-Developer-ID codesigning or App Store notarisation (the official
  sponsor builds use cert `383J84DVB6` — we use ad-hoc identity `-`).
- DMG packaging (`make dmg` requires the `create-dmg` Homebrew formula
  plus an Apple Developer ID. If you want one anyway:
  `brew install create-dmg && cmake --build build --target dmg`).
- Anything to your music library, database, or external services.

---

## Architecture diagram

```text
                                   ┌─────────────────────────────────────┐
                                   │ github.com/strawberrymusicplayer/   │
                                   │  strawberry-macos-dependencies[-legacy] │
                                   │  releases/latest/.tar.xz (~100 MB)  │
                                   └─────────────────┬───────────────────┘
                                                     │   curl + sudo tar
                                                     ▼
   brew install cmake pkgconf          /opt/strawberry_macos_<arch>_release/
            │                          ├── bin/macdeployqt, macdeploycheck
            │                          ├── lib/Qt*, gstreamer-1.0/*, …
            │                          ├── lib/cmake/KDSingleApplication-qt6/
            │                          ├── lib/Sparkle.framework
            │                          ├── lib/pkgconfig/*.pc
            │                          └── lib/{libgpod,libsparsehash,…}
            │                                       │
            └─────────────┬─────────────────────────┘
                          ▼
                  cmake --build build
                  cmake --build build --target deploy
                          │
                          ▼
              build/strawberry.app   ─── ad-hoc codesign --deep ───►
                          │
                          ▼
              /Applications/strawberry.app