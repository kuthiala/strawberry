# 8. Quick Reference

A skim-friendly cheat-sheet. Use **Ctrl-F** on this page to find a keyword fast.

## Reverse Lookup: "I want to change X — where is it?"

| Symptom / Feature                              | Start here                                                                |
| ---------------------------------------------- | ------------------------------------------------------------------------- |
| Playback (play/pause/stop/seek)                | `src/core/player.{h,cpp}` → `src/engine/gstengine.{h,cpp}`                |
| Gapless / crossfade                            | `src/engine/gstenginepipeline.{h,cpp}`                                    |
| Audio output device list                       | `src/engine/devicefinder*.{h,cpp}`                                        |
| Loudness normalisation (EBU R128)              | `src/engine/ebur128analysis.{h,cpp}`                                      |
| Fingerprinting                                 | `src/engine/chromaprinter.{h,cpp}` + `src/musicbrainz/`                   |
| Local library scan                             | `src/collection/collectionwatcher.{h,cpp}`                                |
| Library tree (artist/album/genre)              | `src/collection/collectionmodel.{h,cpp}`                                  |
| Library filter bar                             | `src/collection/collectionfilter*.{h,cpp,ui}` + `src/filterparser/`       |
| Playlists                                      | `src/playlist/playlist*.{h,cpp,ui}`                                       |
| Smart/dynamic playlists                        | `src/smartplaylists/`                                                     |
| Saved playlists (DB persistence)               | `src/playlist/playlistbackend.{h,cpp}`                                    |
| Reading playlist files                         | `src/playlistparsers/`                                                    |
| Play queue                                     | `src/queue/`                                                              |
| Reading tags from files                        | `src/tagreader/tagreadertaglib.{h,cpp}` (TagLib) or `tagreadergme.{h,cpp}`|
| Edit Tag dialog                                | `src/dialogs/edittagdialog.{h,cpp,ui}`                                    |
| Fetch missing tags via MusicBrainz             | `src/musicbrainz/` + `src/dialogs/trackselectiondialog.{h,cpp,ui}`        |
| Album cover providers                          | `src/covermanager/`                                                       |
| Lyrics providers                               | `src/lyrics/`                                                             |
| Last.fm / ListenBrainz scrobbling              | `src/scrobbler/`                                                          |
| Streaming services (Tidal/Spotify/Qobuz/Subsonic) | `src/streaming/` + `src/tidal/` `src/spotify/` `src/qobuz/` `src/subsonic/` |
| Internet radio                                 | `src/radios/`                                                             |
| Devices (USB / MTP / iPod / CD)                | `src/device/` — see [`10-ipod-sync.md`](./10-ipod-sync.md) for iPod details   |
| iPod sync data flow / libgpod / SysInfo / hash58 | [`10-ipod-sync.md`](./10-ipod-sync.md)                                       |
| Transcoder (incl. ALAC pipeline for iPod)      | `src/transcoder/` — see [`10-ipod-sync.md §10.6`](./10-ipod-sync.md#106-transcoding-for-ipod-flac--alac) |
| Organize / copy-to-collection / copy-to-device | `src/organize/`                                                           |
| macOS build / install / debug / log paths      | [`11-macos-dev-loop.md`](./11-macos-dev-loop.md) + [`install-macos.sh`](../install-macos.sh) |
| **macOS: app crashes immediately on launch** (`objc[]: Class ... is implemented in both`, `Could not load the Qt platform plugin "cocoa"`) | [`11-macos-dev-loop.md §11.15`](./11-macos-dev-loop.md#1115-post-mortem-the-duplicate-qtcore-crash-read-this-first-if-the-app-wont-launch) — duplicate-QtCore load, fix is `./install-macos.sh bundle && ./install-macos.sh install -y` |
| Equalizer                                      | `src/equalizer/`                                                          |
| Audio analyzer (visualisation)                 | `src/analyzer/`                                                           |
| Moodbar                                        | `src/moodbar/` + `src/engine/gstfastspectrum.{h,cpp}`                     |
| Waveform                                       | `src/waveform/`                                                           |
| MPRIS2 (Linux)                                 | `src/mpris2/`                                                             |
| Discord Rich Presence                          | `src/discord/`                                                            |
| Global hotkeys                                 | `src/globalshortcuts/`                                                    |
| Settings dialog                                | `src/settings/settingsdialog.{h,cpp,ui}` + per-page files                 |
| Settings keys                                  | `src/constants/<feature>settings.h`                                       |
| Main window layout                             | `src/core/mainwindow.{h,cpp,ui}`                                          |
| About / Console / Error dialogs                | `src/dialogs/`                                                            |
| System tray                                    | `src/systemtrayicon/`                                                     |
| OSD (track-change popup)                       | `src/osd/`                                                                |
| Command-line options                           | `src/core/commandlineoptions.{h,cpp}`                                     |
| Logging                                        | `src/core/logging.{h,cpp}`                                                |
| Database / schema                              | `src/core/database.{h,cpp}` + `data/schema/`                              |
| SQL helpers                                    | `src/core/sqlquery.{h,cpp}`, `sqlrow.{h,cpp}`, `scopedtransaction.{h,cpp}`|
| Network access                                 | `src/core/networkaccessmanager.{h,cpp}`, `httpbaserequest.{h,cpp}`        |
| OAuth                                          | `src/core/oauthenticator.{h,cpp}` + `localredirectserver.{h,cpp}`        |
| Build configuration                            | `CMakeLists.txt`, `cmake/`, `src/config.h.in`                             |
| Tests                                          | `tests/CMakeLists.txt`, `tests/src/*_test.cpp`                            |

## Common Macros & Identifiers

| Name                          | Purpose                                                       |
| ----------------------------- | ------------------------------------------------------------- |
| `Q_OBJECT`                    | Required in any class with signals/slots / `tr()`             |
| `Q_SIGNALS` / `Q_SLOTS` / `Q_EMIT` | Replacements for `signals`/`slots`/`emit` (kept out by `QT_NO_KEYWORDS`) |
| `u"..."_s`                    | `QString` literal                                             |
| `u"..."` (no suffix)          | `QStringView` literal                                         |
| `"..."_L1`                    | `QLatin1String` literal                                       |
| `"..."_ba`                    | `QByteArray` literal                                          |
| `QStringLiteral("...")`       | Older `QString` literal — still seen, equivalent              |
| `qLog(Level) << ...`          | Strawberry logging (`Debug`, `Info`, `Warning`, `Error`, `Fatal`) |
| `Q_INIT_RESOURCE(name)`       | Initialise a `.qrc` resource at runtime                       |
| `HAVE_DBUS`, `HAVE_MPRIS2`, `HAVE_MOODBAR`, `HAVE_WAVEFORM`, `HAVE_TRANSLATIONS`, `HAVE_DISCORD_RPC`, `HAVE_QTSPARKLE`, `HAVE_CHROMAPRINT`, `HAVE_EBUR128`, `HAVE_GSTFASTSPECTRUM`, `HAVE_GPOD`, `HAVE_MTP`, `HAVE_AUDIOCD`, `HAVE_X11_GLOBALSHORTCUTS`, `HAVE_KGLOBALACCEL_GLOBALSHORTCUTS`, `HAVE_GIO`, `HAVE_UDISKS2` | Feature flags from `config.h.in` |
| `Q_OS_LINUX`, `Q_OS_MACOS`, `Q_OS_WIN32`, `Q_OS_UNIX` | Qt platform macros                                |

## File Type Cheat Sheet

| Suffix    | Meaning                                                                |
| --------- | ---------------------------------------------------------------------- |
| `.h`      | Header (most have `Q_OBJECT` if they define a `QObject` subclass)      |
| `.cpp`    | C++ implementation                                                     |
| `.mm`     | Objective-C++ (macOS only)                                             |
| `.ui`     | Qt Designer form (XML; processed by `uic` into `ui_<name>.h`)          |
| `.qrc`    | Qt resource manifest                                                   |
| `.qss`    | Qt stylesheet (CSS-ish)                                                |
| `.ts`     | Qt translation source                                                  |
| `.qm`     | Compiled translation                                                   |
| `.xml`    | D-Bus interface descriptions (in `src/device/`)                        |
| `.sql`    | SQLite schema / migration                                              |

## CMake Targets

```bash
# Configure
cmake -S . -B build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_WERROR=OFF

# Build the app
cmake --build build --parallel $(nproc)

# Build a single test binary
cmake --build build --target utilities_test

# Build & run *all* tests
cmake --build build --target run_strawberry_tests

# Build the (non-default) live lyrics test
cmake --build build --target lyrics_live_tests

# Install
sudo cmake --install build
```

## Running the App from Build

```bash
./build/strawberry --help
./build/strawberry --log-levels '*:3'   # max verbosity
./build/strawberry --play
./build/strawberry --previous
./build/strawberry --next
./build/strawberry --volume 50
./build/strawberry path/to/song.flac    # plays/appends
```

(See [`src/core/commandlineoptions.cpp`](../src/core/commandlineoptions.cpp) for the full list.)

## Key Singleton-like Services (accessor on `Application*`)

```cpp
app->player()                       // playback control
app->collection_backend()           // local library DB
app->collection_model()             // library tree model
app->playlist_manager()             // playlists + current
app->task_manager()                 // background-task tracker
app->network()                      // QNAM wrapper
app->cover_providers()              // cover provider registry
app->lyrics_providers()             // lyrics provider registry
app->scrobbler()                    // scrobbling
app->streaming_services()           // streaming registries
app->radio_services()               // radio registries
app->tagreader_client()             // tag I/O
app->database()                     // raw DB access
app->moodbar_loader()               // (HAVE_MOODBAR)
app->waveform_loader()              // (HAVE_WAVEFORM)
```

## Qt Patterns Reminders

```cpp
// Connect (function-pointer syntax preferred):
QObject::connect(src, &SourceClass::Signal, dst, &DestClass::Slot);

// Queued cross-thread:
QObject::connect(src, &SourceClass::Signal, dst, &DestClass::Slot, Qt::QueuedConnection);

// One-shot lambda:
QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
  reply->deleteLater();
  …
});

// Iterating:
for (const Song &song : std::as_const(songs)) { … }

// Settings:
Settings s;
s.beginGroup(BehaviourSettings::kSettingsGroup);
QString val = s.value(BehaviourSettings::kLanguage).toString();
s.endGroup();
```

## Where Builds Go

- `build/` is the conventional CMake output directory at the repo root (not in-tree).
- `strawberry` binary appears at `build/strawberry` (Linux/Windows console) or `build/strawberry.app` (macOS bundle).
- Tests build into `build/tests/`.

## Settings & Data Locations at Runtime

| Platform   | Settings                                                                | DB / cache                                                  |
| ---------- | ----------------------------------------------------------------------- | ----------------------------------------------------------- |
| Linux      | `~/.config/strawberry/Strawberry.conf`                                  | `~/.local/share/strawberry/strawberry/strawberry.db`        |
| macOS      | `~/Library/Preferences/org.strawberrymusicplayer.strawberry.plist`      | `~/Library/Application Support/strawberry/strawberry/…`     |
| Windows    | `HKCU\Software\Strawberry\Strawberry`                                   | `%APPDATA%\strawberry\strawberry\strawberry.db`             |

## Sponsor / Support Reminders

- **User-facing issues:** the README directs users to the [forum](https://forum.strawberrymusicplayer.org/) or [Wiki FAQ](https://wiki.strawberrymusicplayer.org/wiki/FAQ).
- **Feature requests are *not* accepted via GitHub issues** — they belong on the forum.
- **Flatpak issues are not supported by the project** — direct users to Flatpak.

## Git Commit Message Quick Form

```
ClassName: Short imperative summary

Optional longer body if needed.

Fixes #1234
```

See [`04-coding-conventions.md`](./04-coding-conventions.md) ("Commit Messages") and [`CONTRIBUTING.md`](../CONTRIBUTING.md).