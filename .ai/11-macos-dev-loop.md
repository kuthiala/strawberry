# 11. macOS Development Loop

> Audience: developers on Apple Silicon or Intel macOS Sequoia (15) or later.
> Captures the install/build/debug helpers, the layout of the bundled app,
> and the patches a contributor needs to know about when iterating quickly.

---

## 11.1 One-shot Install: `install-macos.sh`

The repo ships a helper at the repo root, [`install-macos.sh`](../install-macos.sh),
that consolidates the multi-step macOS workflow into a few subcommands:

| Subcommand            | What it does                                                                                              |
| --------------------- | --------------------------------------------------------------------------------------------------------- |
| `deps`                | Verifies Homebrew + every dependency from [`CMakeLists.txt`](../CMakeLists.txt) is installed. Installs anything missing via `brew install`. Idempotent. |
| `configure`           | Runs `cmake -S . -B build -G Ninja` with the correct prefix paths (`/opt/homebrew` on ARM, `/usr/local` on Intel). |
| `build`               | `cmake --build build -j$(sysctl -n hw.ncpu)`. Fast incremental rebuild.                                   |
| `bundle`              | Runs `macdeployqt` + ad-hoc codesign so `build/strawberry.app` is fully **self-contained** (every dylib reference rewritten from `/opt/strawberry_macos_<arch>_release/lib/...` to `@loader_path/...`). **Required after**: first build, `clean`, anything that changed `CMakeLists.txt` link lists, dependency upgrades, and re-pulled deps tarball. **Not required** for pure source edits that touched only `.cpp`/`.h`/`.ui` of already-linked targets. When in doubt, run it — it's idempotent (~2-4 min). See [§11.13 row 1](#1113-common-pitfalls) for the symptom when you skip it and shouldn't have. |
| `install [-y]`        | Copies `build/strawberry.app` → `/Applications/strawberry.app`. `-y` skips the "overwrite?" prompt.       |
| `uninstall [-y]`      | Removes `/Applications/strawberry.app` and the app's settings directories under `~/Library/Application Support/Strawberry` and `~/Library/Preferences/com.strawberrymusicplayer.*`. |
| `clean`               | `rm -rf build/`. Forces a from-scratch configure next time.                                               |
| `all`                 | `deps → configure → build → bundle → install` in one shot.                                                |

### Typical iteration loop

```bash
# Edit src/...
./install-macos.sh build && ./install-macos.sh install -y \
  && open -a Strawberry
# (no `bundle` needed for code-only changes — incremental)
```

> **ⓘ When `install` auto-invokes `bundle`** — as of the most recent script
> revision, `./install-macos.sh install` runs the §11.3 dirty-bundle audit
> against `build/strawberry.app` before copying it to `/Applications/` and
> auto-invokes `bundle` if any Mach-O file still references
> `/opt/strawberry_macos_<arch>_release/lib/...`. You'll see this auto-run
> kick in whenever **any** of the following is true (these are the same
> situations that used to require you to remember `bundle` yourself, with
> the duplicate-QtCore crash in [§11.15](#1115-post-mortem-the-duplicate-qtcore-crash-read-this-first-if-the-app-wont-launch)
> as the punishment for forgetting):
>
> - You just ran `./install-macos.sh clean` (or `rm -rf build/`).
> - `build/strawberry.app/` did not exist before this `build` (first build ever, or after `clean`).
> - You upgraded any brew or deps-tarball library (Qt, GStreamer, TagLib, glib, …).
> - You touched `CMakeLists.txt` in a way that changed a `target_link_libraries(...)` list.
> - You re-pulled the deps tarball (`./install-macos.sh deps --refresh-deps`).
>
> If you want to skip the safety net (e.g. you've already bundled and are
> just iterating on the `install` step itself), pass `--no-bundle`.
>
> 2-second sanity check you can still run by hand:
> ```bash
> otool -L build/strawberry.app/Contents/MacOS/strawberry | grep /opt/strawberry_macos && echo "DIRTY — run bundle"
> ```
> If that prints anything, `install` will now auto-bundle for you; but
> running `bundle` manually first is still faster because the auto-bundle
> can't start `rsync`ing into `/Applications/` until the bundle finishes.
> See [§11.3](#113-verifying-the-bundle-is-self-contained) for a fuller
> audit and [§11.13 row 1](#1113-common-pitfalls) for the historical
> failure mode this safety net replaces.

### Full rebuild when something Qt or library-related changed

```bash
./install-macos.sh clean
./install-macos.sh all
```

The script is intentionally **idempotent** and **fast** so you can run it
unconditionally in test loops.

---

## 11.2 Build Directory Layout

CMake is configured as an **out-of-tree** build with everything under
`build/`:

```
build/
├── CMakeCache.txt
├── compile_commands.json    ← symlinked from repo root for clangd
├── strawberry.app/
│   └── Contents/
│       ├── MacOS/strawberry          ← the executable
│       ├── Frameworks/               ← bundled Qt + GStreamer .frameworks
│       ├── PlugIns/                  ← Qt plugins (sqldrivers, imageformats…)
│       ├── Resources/
│       │   ├── gstreamer-1.0/        ← bundled GStreamer plugins
│       │   ├── translations/         ← *.qm
│       │   └── icons/                ← embedded icon resources
│       └── Info.plist
├── strawberry-tagreader              ← the out-of-process tag reader (executable)
└── tests/strawberry_tests            ← GoogleTest runner (if `BUILD_TESTING=ON`)
```

`compile_commands.json` is generated by CMake; symlink it into the repo root
if your editor needs it:

```bash
ln -sf build/compile_commands.json compile_commands.json
```

---

## 11.3 Verifying the Bundle is Self-Contained

After `./install-macos.sh bundle`, every Mach-O file in the `.app` should
reference its dependencies via `@loader_path/...`, `@executable_path/...`, or
`@rpath/...` only. Any leftover absolute reference to
`/opt/strawberry_macos_<arch>_release/lib/...` means dyld will load *both*
the bundled copy and the dev-time `/opt/...` copy at runtime — and if the
duplicated library is Qt, you get the [§11.13 row 1](#1113-common-pitfalls)
crash.

### Single-file check (fast)

```bash
otool -L /Applications/strawberry.app/Contents/MacOS/strawberry \
  | grep /opt/strawberry_macos
# (no output = clean; any line = re-run bundle)
```

### Full bundle audit (slower, ~10 s)

```bash
APP=/Applications/strawberry.app
find "$APP" -type f \( -name '*.dylib' -o -name '*.so' \
                       -o -name 'strawberry' -o -name 'gst-plugin-scanner' \) \
  -exec sh -c 'otool -L "$1" 2>/dev/null | grep -q /opt/strawberry_macos && echo "DIRTY: $1"' _ {} \;
echo done
```

A clean bundle emits only the trailing `done`. A successful `bundle` run
finishes by printing `✓ <path> is self-contained` — that line is the
install-script's built-in version of this check; trust it but verify
occasionally with the commands above (the script's two concurrent
`macdeployqt` invocations have a known race that can leave one or two
plugins dirty — see [§11.13 row 7](#1113-common-pitfalls)).

---

## 11.4 Why `/opt/strawberry_macos_x86_64_release` Appears in rpaths

The linker emits a warning during build:

```
ld: warning: duplicate -rpath '/opt/strawberry_macos_x86_64_release/lib' ignored
```

This is **expected** — Strawberry's CI builds against a custom-built Qt/GStreamer
stack in that path (see the Linux CI for the canonical layout). On dev machines
the path doesn't exist; the rpath is ignored. The warning is harmless.

If you want to silence it locally:

```cmake
# CMakeLists.txt — append to existing CMAKE_BUILD_RPATH:
# (do NOT commit; this is dev-only)
list(REMOVE_ITEM CMAKE_BUILD_RPATH "/opt/strawberry_macos_x86_64_release/lib")
```

---

## 11.5 Dependency Map (macOS specifics)

| Brew formula              | Used for                                              |
| ------------------------- | ----------------------------------------------------- |
| `qt`                      | Qt 6 (Core, Widgets, Network, SQL, Concurrent, DBus). |
| `cmake` + `ninja`         | Build system + generator.                             |
| `gstreamer`               | Audio engine and transcoder pipelines.                |
| `gst-plugins-base/-good/-bad/-ugly` | Codecs + filters. The `-bad` set provides `alacenc`. |
| `gst-libav`               | FFmpeg-backed encoders (`avenc_alac`). **Critical** — without this, iPod transcoding has no usable ALAC encoder. |
| `taglib`                  | Tag read/write.                                       |
| `protobuf`                | Tag reader IPC.                                       |
| `chromaprint`             | MusicBrainz fingerprinting (optional).                |
| `libebur128`              | EBU R128 loudness (optional).                         |
| `libmtp`                  | MTP devices (optional).                               |
| `libgpod` (custom)        | iPod Classic. **Strawberry uses its own fork** — see §11.7. |
| `kdsingleapplication`     | Single-instance management.                           |
| `glib`, `libsoup`         | Pulled in transitively by gstreamer.                  |
| `sparkle`                 | macOS auto-updates (optional, ignored when running unsigned). |

`./install-macos.sh deps` performs all the `brew install` calls; you should
not need to type them by hand.

---

## 11.6 Running with Verbose Logging

Strawberry honours `--log-level` and several GStreamer/GLib env vars.

### Strawberry log

```bash
/Applications/strawberry.app/Contents/MacOS/strawberry \
  --log-level=2:GPodDevice,3:* \
  2>&1 | tee /tmp/strawberry.log
```

`--log-level` uses the format `level:category[,level:category]*` with `*` as
the wildcard. Levels: `0` (error), `1` (warning), `2` (info), `3` (debug).

The default log file (when you run the bundled `.app` via Finder, without a
terminal) goes to:

```
~/Library/Logs/Strawberry/strawberry-stdout.txt
```

— but only if you launch via `open -a Strawberry`; double-clicking through
Finder dumps logs to `/dev/null`.

### GStreamer

```bash
GST_DEBUG=3 GST_DEBUG_NO_COLOR=1 \
  /Applications/strawberry.app/Contents/MacOS/strawberry \
  2>&1 | tee /tmp/gst.log
```

For the transcoder specifically:

```bash
GST_DEBUG="3,GST_PIPELINE:4,alac*:5,mp4mux:5" ...
```

---

## 11.7 The libgpod Fork

Strawberry pins its own `libgpod` (rather than using Homebrew's, which is
abandoned and lacks fixes). The fork lives at
[`.idea/strawberry-libgpod/`](../.idea/strawberry-libgpod/) as a git submodule
referenced by [`.gitmodules`](../.gitmodules).

When you change a Strawberry-side file that touches the libgpod ABI, you may
also need to rebuild the submodule's static library:

```bash
git submodule update --init --recursive
cd .idea/strawberry-libgpod
./autogen.sh && ./configure --prefix=/opt/strawberry-libgpod && make -j8 && sudo make install
```

The CMake config in this repo picks the prefix up via `pkg-config`. The brew
formula `libgpod` is shadowed when `/opt/strawberry-libgpod/lib/pkgconfig` is
ahead of brew's `pkgconfig` on `PKG_CONFIG_PATH`.

See [`10-ipod-sync.md`](./10-ipod-sync.md) for the actual sync flow and the
libgpod symbols of interest.

---

## 11.8 macOS-only Source Files

| File / dir                                         | Why it's macOS-only                                            |
| -------------------------------------------------- | -------------------------------------------------------------- |
| `src/core/mac_startup.{h,mm}`                      | Cocoa setup at process start (NSApplicationDelegate).          |
| `src/core/macsystemtrayicon.{h,mm}`                | NSStatusItem-backed tray icon.                                 |
| `src/device/macosdevicelister.{h,mm}`              | DiskArbitration + IOKit notifications for USB plug/unplug.     |
| `src/engine/macosdevicefinder.{h,cpp}`             | Core Audio device enumeration.                                 |
| `src/utilities/macosutils.{h,mm}`                  | NSBundle / NSWorkspace helpers.                                |
| `src/globalshortcuts/globalshortcutbackend-macos.{h,mm}` | Carbon-style global hotkeys via NSEvent monitors.        |
| `src/scrobbler/scrobblingapi20-macos.{h,mm}`       | macOS keychain access for stored tokens.                       |

Look for the `Q_OS_MACOS` macro in any cross-platform `.cpp` for further
per-OS branches.

---

## 11.9 Debugging the App in Xcode

The CMake `Xcode` generator works but is rarely used because of the
GStreamer plugin loading mess. Instead:

1. Build normally (`./install-macos.sh build`).
2. Launch from Terminal under `lldb`:

   ```bash
   lldb -- /Applications/strawberry.app/Contents/MacOS/strawberry --log-level=3:*
   ```

3. To attach to an already-running instance: `lldb -p $(pgrep -x strawberry)`.

Setting symbolic breakpoints is straightforward:

```
(lldb) breakpoint set --name GPodDevice::CopyToStorage
(lldb) breakpoint set --name Organize::ProcessSomeFiles
```

For Qt slot tracing, set:

```
QT_LOGGING_RULES="qt.qpa.*=true"
```

---

## 11.10 Where the App Stores State

| Path                                                                   | What's there                                                |
| ---------------------------------------------------------------------- | ----------------------------------------------------------- |
| `~/Library/Application Support/Strawberry/Strawberry/strawberry.db`   | Main SQLite collection DB.                                  |
| `~/Library/Application Support/Strawberry/Strawberry/devicealbumcovers/` | Cached cover thumbnails for devices.                      |
| `~/Library/Application Support/Strawberry/Strawberry/gst-registry-*-bin` | GStreamer plugin registry cache (regenerate by deleting). |
| `~/Library/Preferences/com.strawberrymusicplayer.strawberry.plist`     | `QSettings` data (all settings except passwords).           |
| `~/Library/Keychains/...`                                              | OAuth tokens (Tidal, Qobuz, Spotify, ListenBrainz).         |
| `~/Library/Caches/Strawberry/`                                         | Web request cache, thumbnail cache.                         |
| `~/Library/Logs/Strawberry/strawberry-stdout.txt`                      | Log when launched via `open -a`.                            |

For a clean-slate test:

```bash
./install-macos.sh uninstall -y      # removes app + settings
./install-macos.sh install -y        # back to factory defaults
```

Or surgically, just the SQLite DB:

```bash
rm -f "$HOME/Library/Application Support/Strawberry/Strawberry/strawberry.db"
```

The app will re-create it at next start (schema `data/schema/schema-23.sql`).

---

## 11.11 Inspecting the Collection DB

```bash
DB="$HOME/Library/Application Support/Strawberry/Strawberry/strawberry.db"

sqlite3 "$DB" ".schema songs"
sqlite3 "$DB" "SELECT count(*) FROM songs;"
sqlite3 "$DB" "SELECT title, art_embedded, art_automatic, art_manual
               FROM songs WHERE artist LIKE '%coldplay%' LIMIT 5;"
```

Don't run `VACUUM` while the app is open (Qt's connection holds a write lock).

The schema version in the live DB:

```bash
sqlite3 "$DB" "SELECT version FROM schema_version;"
```

Should match the highest-numbered `data/schema/schema-NN.sql` shipped.

---

## 11.12 Code Signing / Notarization

The bundled `.app` produced by `./install-macos.sh` is **unsigned**. macOS
Gatekeeper will block first launch:

```bash
xattr -dr com.apple.quarantine /Applications/strawberry.app
```

Or right-click → Open → Open in System Settings → Privacy & Security → "Open
Anyway".

The official release builds are signed and notarized by the project owner;
that path is not automated in this repo.

---

## 11.13 Common Pitfalls

| Symptom                                                                              | Cause                                                                                 | Fix                                                                  |
| ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| **App crashes within ~100 ms of launch**, terminal log shows `objc[NNN]: Class QT_ROOT_LEVEL_POOL... is implemented in both /opt/strawberry_macos_<arch>_release/lib/QtCore.framework... and .../Contents/Frameworks/QtCore.framework` followed by `Could not load the Qt platform plugin "cocoa"` | **Duplicate-QtCore load.** The installed binary still has absolute `/opt/strawberry_macos_<arch>_release/lib/...` references for non-Qt libs (GStreamer, TagLib, SQLite, ICU, glib, …); those libs transitively pull in the dev-time `QtCore.framework` *in addition* to the bundled one from `@rpath/`. Two QtCore images → duplicate ObjC class registration → `cocoa` plugin init fails. Historically meant `bundle` was skipped between `build` and `install`. **As of the current script this should be unreachable** because `cmd_install` runs the §11.3 audit and auto-invokes `bundle` when dirty. If you still see this, it means you ran `install --no-bundle` or hand-rsync'd the .app, or `bundle` itself errored out without exiting (see row below). | 1. **Verify** with [§11.3](#113-verifying-the-bundle-is-self-contained). 2. `./install-macos.sh bundle && ./install-macos.sh install -y`. **Do not** assume any source revert is at fault — this crash happens in `QApplication` construction, before any Strawberry code runs. |
| App launches then immediately crashes with `dyld: Library not loaded: @rpath/QtCore.framework/...` (note: **different** from the duplicate-class crash above) | `macdeployqt` was never run — the bundled `Contents/Frameworks/` is missing entirely or incomplete. Common after first build or `clean`. **Should also be unreachable** via this script for the same reason as the row above. | `./install-macos.sh bundle && ./install-macos.sh install -y`         |
| GStreamer logs "no element \"alacenc\""                                              | `gst-plugins-bad` or `gst-libav` not installed/bundled                                | `brew install gst-plugins-bad gst-libav && ./install-macos.sh bundle` |
| Settings appear missing after `make install` (defaults shown)                        | `QSettings` reads from a new bundle ID because Info.plist's `CFBundleIdentifier` changed | Don't change the bundle ID, or run `defaults read` to verify which plist is active |
| iPod-related symbols missing from binary                                             | `libgpod` not found at configure time, `HAVE_GPOD` is `0` in `config.h`               | `brew install libgpod && ./install-macos.sh configure`               |
| First sync after install leaves iPod showing "No Music."                             | Pre-fix Strawberry didn't seed `SysInfo`; the file from a prior run is stale          | Delete `iPod_Control/Device/SysInfo` and re-sync                     |
| `clang-tidy` / clangd not finding headers                                            | `compile_commands.json` symlink missing or stale                                      | `ln -sf build/compile_commands.json compile_commands.json`           |
| `bundle` log contains `ERROR: "fatal error: install_name_tool: cannot rename .../libgstXXX.dylib to .../libgstXXX.dylib.YYYYYY (No such file or directory)"` | Known race: the script invokes `macdeployqt` twice concurrently (main pass + `-executable=gst-plugin-scanner` recursive pass). Both processes try to mutate the same plugin's load commands at the same time; one loses. The losing pass's edits are lost on whichever files it touched last. **As of the current script `cmd_bundle` now detects residual `/opt/...` references after `macdeployqt` exits and auto-runs `install_name_tool -change` + ad-hoc resign on each surviving file, then re-audits.** You'll still see the `ERROR: cannot rename ...` lines in the bundle log because they come from `macdeployqt`'s own stderr; what's changed is the script no longer trusts them silently — look for `Repairing residual /opt references in-place` in the script output. | Nothing — the auto-repair runs unconditionally. If you see `Refusing to mark bundle self-contained` (the post-repair audit still found dirty files), re-run `./install-macos.sh bundle`. If two runs in a row both fail, manually patch the one file: `install_name_tool -change /opt/strawberry_macos_x86_64_release/lib/<libname> @loader_path/../../Frameworks/<libname> <plugin-path>` then `codesign --force --sign - <plugin-path>`. |
| You ran `./install-macos.sh clean && ./install-macos.sh all` and the app *still* crashes on launch | `all` runs `bundle` as part of its sequence, so this should be impossible — unless `bundle` errored out and the script kept going. | `cat /tmp/sb-bundle.log 2>/dev/null \| grep -iE '^error\|^fatal' \| head -20`. If that's empty, retry; if not, run `bundle` standalone and read the failure. |
| Runtime behavior reflects the *old* version of a dependency you've already rebuilt and dropped into `${DEPS_PREFIX}/lib/` (e.g. you patched libgpod for the `pack_RGB_565` overflow-guard bug, rebuilt + installed it under `/opt/...`, but the iPod sync still emits `pack_RGB_565: assertion 'dest_width < (gint)G_MAXUINT/2' failed` and produces an `ArtworkDB` with no matching `.ithmb` files) | **Stale bundled library shadowing the rebuilt source.** `macdeployqt` only copies a dylib into the bundle if no file of that name is already there — it never compares timestamps or hashes — so once a lib is in `Contents/Frameworks/`, subsequent rebuilds of the corresponding `${DEPS_PREFIX}/lib/<x>.dylib` get silently shadowed. **As of the current script `cmd_bundle` and `cmd_install`'s pre-flight check both run a SHA-256 audit (`bundle_collect_stale`) against every `Contents/Frameworks/*.dylib` and `Contents/PlugIns/*.dylib`, and auto-refresh any whose contents drift from `${DEPS_PREFIX}/lib/...`.** Look for `STALE:` lines and `Refreshing stale bundled libs` in the script output. | **2-second manual repro**: `shasum -a 256 /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib ${DEPS_PREFIX}/lib/libgpod.dylib` — different hashes means stale. **2-second manual fix**: `cp ${DEPS_PREFIX}/lib/libgpod.dylib /Applications/strawberry.app/Contents/Frameworks/`, then for each `/opt/...` line emitted by `otool -L` on the copied file, `install_name_tool -change /opt/.../lib/<libname> @loader_path/<libname> .../Frameworks/libgpod.dylib`, then `codesign --force --sign - .../Frameworks/libgpod.dylib`. Or just `./install-macos.sh install -y` — the safety net catches and repairs this automatically now. |
| Runtime behavior reflects the *old* version of a dependency you rebuilt **under `.idea/`** (e.g. you patched `.idea/strawberry-libgpod/src/itdb_itunesdb.c`, ran `cmake --build .idea/strawberry-libgpod/build`, saw fresh bytes, ran `./install-macos.sh install`, its audit reported "self-contained", but `strings /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib \| grep <your-new-symbol>` is empty and the runtime still crashes with the pre-patch fingerprint). This is **Bug #12** — related to but distinct from the row above. | **The audit compares the bundle against `${DEPS_PREFIX}/lib/libgpod.dylib`, NOT against `.idea/strawberry-libgpod/build/libgpod.dylib`.** When you rebuild libgpod under `.idea/`, the `/opt/` tree stays stale. The audit sees bundle-matches-opt and marks "self-contained" — both copies are pre-patch. Row 8 above only catches the case where `/opt/` has been refreshed and the bundle hasn't; it can't help when *both* are stale. | **3-step manual fix.** (1) `cp .idea/strawberry-libgpod/build/libgpod.dylib ${DEPS_PREFIX}/lib/libgpod.dylib`. (2) `cp .idea/strawberry-libgpod/build/libgpod.dylib /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib`. (3) `for DEP in $(otool -L /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib \| awk '/\/opt\/strawberry_macos/{print $1}'); do install_name_tool -change "$DEP" "@loader_path/$(basename "$DEP")" /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib; done && install_name_tool -id "@rpath/libgpod.dylib" /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib && codesign --force --sign - /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib`. Verify with `strings /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib \| grep <your-new-symbol>`. Full deployment gotcha writeup at [`10-ipod-sync.md §10.14`](./10-ipod-sync.md#1014-bug-11-mach_vm_allocate_kernel-failed-at-1886-songs--unchecked-g_realloc-in-libgpod) ("Deployment gotcha (Bug #12)"). **TODO for install-macos.sh:** extend `bundle_collect_stale` to also compare `${DEPS_PREFIX}/lib/libgpod.dylib` against `.idea/strawberry-libgpod/build/libgpod.dylib` and auto-refresh the `/opt/` copy from the `.idea/` build when newer. |

---

## 11.14 Cross-References

- [`03-build-and-test.md`](./03-build-and-test.md) — generic build (Linux + macOS + Windows).
- [`07-common-workflows.md`](./07-common-workflows.md) — adding a feature step-by-step.
- [`10-ipod-sync.md`](./10-ipod-sync.md) — iPod sync internals (what runs once the bundle launches).
- [`docs/INSTALL-macOS.md`](../docs/INSTALL-macOS.md) — user-facing install doc (matches what `install-macos.sh` does).

---

## 11.15 Post-Mortem: The Duplicate-QtCore Crash (Read This First If The App Won't Launch)

If you've been asked to debug a "Strawberry crashes immediately on launch on
macOS" report, **do not start by inspecting recently-changed source files.**
The overwhelmingly most likely cause has nothing to do with the source tree.

### Triage in 30 seconds

```bash
# 1. Get the real crash signature — never trust the user's framing:
pkill -x strawberry 2>/dev/null; sleep 1
/Applications/strawberry.app/Contents/MacOS/strawberry 2>&1 | head -20
```

Look for these two lines (they always appear together, in this order):

```
objc[NNNN]: Class QT_ROOT_LEVEL_POOL__... is implemented in both
  /opt/strawberry_macos_<arch>_release/lib/QtCore.framework/.../QtCore  (0x...)
  and /Applications/strawberry.app/Contents/Frameworks/QtCore.framework/.../QtCore  (0x...).
  This may cause spurious casting failures and mysterious crashes. One of the
  duplicates must be removed or renamed.
[... 4 more objc[] duplicates for KeyValueObserver, RunLoopModeTracker,
 QDarwinPermissionHandler, QMetalLayer ...]
Could not load the Qt platform plugin "cocoa" in "" even though it was found.
This application failed to start because no Qt platform plugin could be
initialized. Reinstalling the application may fix this problem.
```

If you see that pattern, **stop debugging source code.** The fix is two
commands; see "Recovery" below. The crash happens inside
`QGuiApplicationPrivate::createPlatformIntegration()`, which runs before
`QApplication`'s constructor returns — well before `main()` reaches any
Strawberry code, any `MainWindow`, any `DeviceManager`, any `GPodDevice`,
any plugin. **Whatever the user thinks they recently changed is innocent
by construction.**

### Why this happens (one-paragraph mental model)

The binary's load commands include both `@rpath/QtCore.framework/...` (rewritten
by `macdeployqt` to point at the bundled Qt under `Contents/Frameworks/`) **and**
`/opt/strawberry_macos_<arch>_release/lib/libgstreamer-1.0.0.dylib`,
`.../libtag.2.dylib`, `.../libsqlite3.dylib`, etc. — absolute paths that
**only** get rewritten if `macdeployqt` ran. Each of those non-Qt libs was
itself linked against `/opt/.../libQt6Core.6.dylib`, so dyld faithfully
loads the dev-time `QtCore.framework` *in addition to* the bundled one.
The Objective-C runtime sees two copies of every Qt-defined class and warns;
then the `cocoa` platform plugin (loaded by the bundled QtGui) fails to bind
against the bundled QtCore because some of its symbols resolved into the
`/opt/...` copy instead. Plugin init returns NULL → `qFatal` → exit.

### Recovery (the fix is almost always two commands)

```bash
./install-macos.sh bundle      # rewrites every /opt/... ref to @loader_path/...
./install-macos.sh install -y  # copies the now-self-contained .app to /Applications
```

Then verify ([§11.3](#113-verifying-the-bundle-is-self-contained)):

```bash
otool -L /Applications/strawberry.app/Contents/MacOS/strawberry | grep /opt/strawberry_macos
# (no output = fixed)
```

Then launch and confirm it stays up:

```bash
/Applications/strawberry.app/Contents/MacOS/strawberry 2>&1 | head -10
# Expect: "Strawberry <hash> Qt 6.x.y" then "Creating GLib main event loop"
# then "Registered URL handler for..." — no objc[] warnings, no plugin errors.
```

### When the recovery does NOT work

If `bundle` + `install` + verify still leaves you with the crash, then **and
only then** start considering other causes:

1. Read `cat /tmp/sb-bundle.log | grep -iE '^error|^fatal' | head` for
   `install_name_tool` failures (the §11.13 row 7 race). Re-run `bundle` or
   manually patch the one offending file.
2. Confirm the binary you launched is the one you just installed:
   `stat -f '%Sm %N' /Applications/strawberry.app/Contents/MacOS/strawberry`
   should show a timestamp from seconds ago, not days.
3. `xattr -dr com.apple.quarantine /Applications/strawberry.app` in case
   Gatekeeper re-flagged after the copy.
4. *Then* — and not before — look at any source changes.

### Anti-patterns to avoid

- **Don't** `git checkout -- src/` as a first reaction. The crash isn't in
  source. Reverting destroys ongoing work for no benefit.
- **Don't** trust `[100%] Built target strawberry` as a sign the bundle is
  installable. The build succeeds whether the load commands are bundle-safe
  or not — `cmake --build` has no awareness of `macdeployqt`'s contract.
- **Don't** assume a recent `replace_in_file` revert is "subtly malformed"
  without first checking whether the crash happens *before* any of the
  reverted code could possibly run. Read the crash log first; speculate
  second.
- **Don't** skip `bundle` because you "only changed one line of C++". If
  you `clean`ed or first-built, the resulting `.app` has dev-time load
  paths regardless of how trivial the source delta is.

---

## 11.16 Crash Logs

If Strawberry crashes (or is killed by a signal Strawberry didn't catch),
two artefacts get written to disk. Both should be consulted; they
complement rather than replace each other.

### 1. Strawberry-side crash log (small, human-readable)

Written by [`src/core/crashreporter.cpp`](../src/core/crashreporter.cpp).
Path:

```
~/Library/Logs/Strawberry/strawberry-crash-<ISO-datetime>-<pid>.log
```

— same directory as the regular Strawberry stdout log
(`strawberry-stdout.txt`), so a one-shot `ls -lat ~/Library/Logs/Strawberry/`
shows them interleaved with the run that produced them.

Contents are deliberately small (a few KB):

```
================ Strawberry crash log ================
Strawberry version: 1.2.20-18-g25d5bf97
Process ID: 12345
Epoch: 1719789308
Signal: SIGSEGV (segmentation fault)
--- Backtrace ---
0   strawberry  0x107a28000 _ZN10GPodDevice13WriteDatabaseER7QString + 1465
1   strawberry  0x107a28000 _ZN8Organize16ProcessSomeFilesEv + 9653
2   strawberry  0x107a28000 _Z11doActivateILb0EEvP7QObjectiPPv + 1533
…
--- End of crash log; re-raising signal so the OS can take its own snapshot. ---
```

The backtrace is from `backtrace_symbols_fd(3)` — function names are
mangled (use `c++filt` if needed) and offsets are within the strawberry
binary's `__TEXT` segment. For the iPod sync example above the mangled
`_ZN10GPodDevice13WriteDatabaseER7QString` translates to
`GPodDevice::WriteDatabase(QString&)` and is the immediate caller of the
crash.

For non-fatal but interesting Strawberry-level failures, code can call
`CrashReporter::WriteSyntheticCrashLog(QStringLiteral("reason"))` to drop
an entry in the same directory without actually terminating the process.

### 2. macOS-native crash report (large, authoritative)

Written by macOS's `ReportCrash` daemon. Path (one file per crash,
filename = `strawberry-<YYYY-MM-DD>-<HHMMSS>.ips`):

```
~/Library/Logs/DiagnosticReports/
```

These are gzipped-JSON-on-the-second-line:

```bash
LATEST=$(ls -t ~/Library/Logs/DiagnosticReports/strawberry-*.ips | head -1)
# Top-level metadata is on line 1; JSON payload on line 2 onwards.
{ head -1 "$LATEST"; tail -n +2 "$LATEST" | python3 -m json.tool; } | less
```

Useful one-liner to extract just the faulting thread's backtrace:

```bash
python3 -c "
import sys, json
raw = open(sys.argv[1]).read().split('\n', 1)
d = json.loads(raw[1])
t = next(x for x in d['threads'] if x.get('triggered'))
print('Signal:', d['exception']['signal'], '   Subtype:', d['exception']['subtype'])
print('ktriageinfo:', d.get('ktriageinfo', '').strip())
print('---')
for f in t['frames']:
    print(f.get('symbol', '(no symbol)'),
          'at offset', f.get('imageOffset', '?'),
          'in image', f.get('imageIndex', '?'))
" "$LATEST" | head -30
```

What the .ips contains that the Strawberry-side log doesn't:

- All threads' backtraces, not just the faulting one — essential when the
  crash is caused by something on a background thread.
- Full register state at the moment of the crash.
- Loaded images with `base` addresses (lets `atos -arch x86_64 -o <binary> -l <base> <addr>` symbolize anything not already symbolized).
- `vmRegionInfo` (which mapping the bad address fell in — `__TEXT` vs
  heap vs guard page).
- `ktriageinfo` — Mach-level hints. For Bug #8 this said
  `mach_vm_allocate_kernel failed within call to vm_map_enter` ×5,
  which was the smoking gun that pointed at VM exhaustion as the root
  cause; without that single line the SIGSEGV at `0xc` would have looked
  like a generic pointer-chasing bug.

### Which one to look at first

Always look at **both**. The Strawberry-side log is fast to find and
read (one `cat`), tells you which symbol crashed, and is enough for
~80% of regressions. The .ips is what you want when the Strawberry-side
log is *not* enough — i.e. when:

- The crash is on a non-main thread (the Strawberry-side log only
  captures the thread that received the signal, which is the same thread
  on macOS but the .ips lets you see all the others' state too).
- You suspect a memory-pressure / OOM / VM-exhaustion failure mode (the
  Strawberry-side log doesn't include `ktriageinfo`).
- You need to symbolize against a stripped binary (use `atos` with the
  image bases from `usedImages` in the .ips).
- The user reported the crash but couldn't email the Strawberry-side log
  because they uninstalled before sending diagnostics (the .ips persists
  through uninstall).

### Worked example: Bug #8 / Bug #9 (per-song `WriteDatabase` crash)

See [`10-ipod-sync.md §10.12`](./10-ipod-sync.md#1012-bug-8--bug-9-per-song-writedatabase-crashed-at-1888-songs-and-scrambled-cover-art)
for the full story. The .ips diagnosis path was:

1. `cat $(ls -t ~/Library/Logs/DiagnosticReports/strawberry-*.ips | head -1)`
2. Read `triggered` thread's frames: top symbol is `write_mhsd_playlists`
   inside libgpod, called from `GPodDevice::WriteDatabase` called from
   `Organize::ProcessSomeFiles`.
3. Read `exception.subtype`: `KERN_INVALID_ADDRESS at 0x000000000000000c`
   (offset 12 from a NULL pointer — struct field deref).
4. Read `ktriageinfo`:
   `VM - (arg = 0x3) mach_vm_allocate_kernel failed within call to vm_map_enter`
   repeated 5×.
5. Conclusion: VM exhaustion in libgpod after too many `itdb_write`
   calls. That's not visible from the symbol name alone — the
   `ktriageinfo` line is what closes the case.

### Disable / re-enable the per-strawberry log

The signal handler installs unconditionally in `main()` after
`logging::Init()`. To disable (e.g. you're running under `lldb` and want
the debugger to catch SIGSEGV first), stop the handler from running by
launching with `STRAWBERRY_DISABLE_CRASHLOG=1` set:

```bash
# (NOT YET IMPLEMENTED — TODO if developer demand appears.)
# For now, comment out the CrashReporter::Init() call in src/main.cpp
# for your local dev build.
```

The handler chains to the default disposition after writing its log, so
even with the handler installed `lldb` still stops at the faulting
instruction *after* the log is written — there's normally no reason to
disable it.
