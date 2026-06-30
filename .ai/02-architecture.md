# 2. Architecture

This document describes the *runtime* shape of the application — how the pieces fit together. For a flat catalogue of directories, see [`05-module-guide.md`](./05-module-guide.md).

## Startup Sequence

The entry point is [`src/main.cpp`](../src/main.cpp). At a high level:

```
main()
 ├── (macOS only) mac::MacMain()          // media keys, early Cocoa init
 ├── set QCoreApplication name/org/version
 ├── RegisterMetaTypes()                  // src/core/metatypes.cpp
 ├── logging::Init()                      // src/core/logging.cpp
 ├── CommandlineOptions::Parse()          // src/core/commandlineoptions.cpp
 ├── KDSingleApplication (early)          // bail out if another instance is up
 ├── QApplication a(argc, argv)
 ├── KDSingleApplication (real one)       // again, after QApplication exists
 ├── GstStartup::Initialize()             // src/engine/gststartup.cpp
 ├── Apply Qt style from settings
 ├── Set permissions on the settings file (Unix)
 ├── Q_INIT_RESOURCE(data); Q_INIT_RESOURCE(icons);
 ├── IconLoader::Init()                   // src/core/iconloader.cpp
 ├── Load translations                    // src/core/translations.cpp
 ├── Application app                      // *** the big one ***
 ├── NetworkProxyFactory::setApplicationProxyFactory(...)
 ├── SystemTrayIcon tray_icon
 ├── OSD osd                              // OSDMac / OSDDBus / OSDBase
 ├── (optional) mpris::Mpris2
 ├── (optional) DiscordRichPresence
 ├── MainWindow w(&app, tray_icon, &osd, [&discord], options)
 ├── (Unix) UnixSignalWatcher for SIGTERM
 └── QCoreApplication::exec()
```

After `exec()` returns, on MinGW (non-winpthreads) GCC the process is force-terminated to work around a known crash.

## The `Application` Object — Dependency Container

[`src/core/application.h`](../src/core/application.h) is **the dependency-injection container** for the rest of the app. It owns (via `ScopedPtr<ApplicationImpl>`) singletons-of-sorts for every cross-cutting service, and exposes them as `SharedPtr<T>` accessors:

| Accessor                       | Service                                                         |
| ------------------------------ | --------------------------------------------------------------- |
| `tagreader_client()`           | TagLib-backed tag I/O ([`tagreader/`](../src/tagreader/))      |
| `database()`                   | The SQLite `Database` ([`core/database.h`](../src/core/database.h)) |
| `task_manager()`               | Background task tracker for the UI                              |
| `player()`                     | Playback controller ([`core/player.h`](../src/core/player.h))   |
| `network()`                    | Shared `QNetworkAccessManager` wrapper                          |
| `device_finders()`             | Audio output device enumeration                                 |
| `url_handlers()`               | Custom URL scheme handlers (tidal://, qobuz://, …)              |
| `device_manager()`             | Removable music devices (MTP, iPod, USB)                        |
| `collection()`                 | Local library — backend + model + watcher                       |
| `collection_backend()`         | DB access for the local library                                 |
| `collection_model()`           | Tree model for the library view                                 |
| `playlist_backend()`           | DB persistence for playlists                                    |
| `playlist_manager()`           | All open playlists + active selection                           |
| `cover_providers()` etc.       | Cover art providers / loader / current-track cover              |
| `lyrics_providers()`           | Lyrics fetcher registry                                         |
| `scrobbler()`                  | Last.fm / ListenBrainz / Subsonic scrobblers                    |
| `streaming_services()`         | Tidal / Spotify / Qobuz / Subsonic                              |
| `radio_services()`             | SomaFM / Radio Paradise / RadioBrowser                          |
| `moodbar_*()` / `waveform_*()` | Optional visualisers                                            |
| `lastfm_import()`              | Last.fm playcount importer                                      |

The `Application` also runs a **GLib main loop on its own GThread** (required by GStreamer/glib), and helps move `QObject`s onto worker threads via `MoveToNewThread()` and `MoveToThread()`. Many subsystems live on dedicated worker threads (database, tag reader, collection watcher, scrobbler, etc.).

> **Rule of thumb for AI:** if you need a cross-cutting service, look up its accessor on `Application*`. Do not create a second instance.

## Threading Model

- **Main (GUI) thread:** the `QApplication` event loop; owns `MainWindow`, all widgets, and most controllers.
- **Database thread:** `Database` is `MoveToNewThread()`-ed. All SQL must run on it (use `QMetaObject::invokeMethod` / queued signals; or `ScopedTransaction` from [`core/scopedtransaction.h`](../src/core/scopedtransaction.h)).
- **Tag reader thread:** [`TagReaderClient`](../src/tagreader/tagreaderclient.h) marshals tag I/O off the main thread.
- **Collection watcher thread:** `CollectionWatcher` performs filesystem scans and tag reads in batches.
- **GLib thread:** drives GStreamer's main loop.
- **Network operations:** Qt's `QNetworkAccessManager` is event-loop-driven; requests dispatched from the main thread.

Helper utilities live in [`core/thread.h`](../src/core/thread.h) and [`utilities/threadutils.h`](../src/utilities/threadutils.h).

## Smart Pointer Conventions

The project does **not** use raw `std::shared_ptr` everywhere — it uses thin wrappers from [`src/includes/`](../src/includes/):

- `ScopedPtr<T>` (≈ `std::unique_ptr`) — see [`includes/scoped_ptr.h`](../src/includes/scoped_ptr.h)
- `SharedPtr<T>` (≈ `std::shared_ptr`) — see [`includes/shared_ptr.h`](../src/includes/shared_ptr.h)
- `Lazy<T>` for lazily-initialised members
- Mac-specific: `scoped_nsobject<T>`, `scoped_cftyperef<T>`
- GObject: `ScopedGObject`

Use `make_shared<T>(...)` (the `using std::make_shared;` form is common in `main.cpp` and elsewhere) to construct shared objects.

## UI Layout — `MainWindow`

[`src/core/mainwindow.{h,cpp,ui}`](../src/core/mainwindow.h) builds a fancy-tabbed sidebar UI (using [`widgets/fancytabwidget.h`](../src/widgets/fancytabwidget.h)) with the following major tabs/panes:

- **Collection** ([`collection/`](../src/collection/)) — local library tree.
- **Playlists** ([`playlist/`](../src/playlist/)) — tabbed playlists + queue.
- **Smart Playlists** ([`smartplaylists/`](../src/smartplaylists/)).
- **Streaming services** ([`streaming/`](../src/streaming/), [`subsonic/`](../src/subsonic/), [`tidal/`](../src/tidal/), [`spotify/`](../src/spotify/), [`qobuz/`](../src/qobuz/)).
- **Radio** ([`radios/`](../src/radios/)).
- **Files** ([`fileview/`](../src/fileview/)) — file system browser.
- **Devices** ([`device/`](../src/device/)).
- **Context** ([`context/`](../src/context/)) — currently-playing info, cover, lyrics, analyzer.

Settings dialog lives in [`settings/`](../src/settings/) with one `SettingsPage` subclass per section. Other modal dialogs live in [`dialogs/`](../src/dialogs/).

## Playback Pipeline

```
User picks a song
     │
     ▼
PlaylistManager.current() → Playlist → PlaylistItem (Song / Stream / Collection / Radio)
     │
     ▼
Player (core/player.cpp) decides URL, asks engine to play
     │
     ▼
EngineBase  ── GstEngine (only backend) ── GstEnginePipeline
     │
     ├─► gst-launch-style pipeline:  src → decodebin → audioconvert → equalizer → volume → sink
     ├─► EBU R128 measurement (optional)
     ├─► Fast-spectrum tap → FastSpectrum / Moodbar
     └─► Waveform tap → Waveform pipeline (optional)
```

- [`engine/enginebase.{h,cpp}`](../src/engine/enginebase.h) — abstract interface; `GstEngine` is the only concrete implementation.
- [`engine/gstenginepipeline.cpp`](../src/engine/gstenginepipeline.cpp) — single-track GStreamer pipeline, including crossfade / gapless logic with **two pipelines** swapped between tracks.
- [`engine/devicefinder*.{h,cpp}`](../src/engine/) — enumerate ALSA, PulseAudio, DirectSound, WASAPI, Core Audio, ASIO devices.
- [`engine/ebur128analysis.{h,cpp}`](../src/engine/ebur128analysis.cpp) — EBU R128 loudness measurement.
- [`engine/gstfastspectrum.{h,cpp}`](../src/engine/gstfastspectrum.cpp) — custom GStreamer element for moodbar.

## Collection / Library Pipeline

```
CollectionWatcher (worker thread) ─ scans directories
   │
   ├── reads tags via TagReaderClient → TagReaderTagLib
   ├── compares with DB (CollectionBackend)
   └── emits SongsDiscovered / SongsDeleted

CollectionBackend (DB thread) ─ SQL CRUD against `songs`, `directories`, etc.
   │
   ▼
CollectionModel (main thread) ─ QAbstractItemModel grouped by Album/Artist/Genre/…
   │
   ▼
CollectionView ─ QTreeView in the sidebar
```

- Database connection per thread (Qt SQL requirement) — `Database::Connect()`.
- Schema migrations are applied at startup by reading `schema-NN.sql` files in order.

## Settings & Constants

- All user settings keys live in [`src/constants/*.h`](../src/constants/) as `inline constexpr` strings (e.g. `BehaviourSettings::kLanguage`). **Never hard-code a settings group/key name** — add it to or use one of these headers.
- `Settings` ([`core/settings.h`](../src/core/settings.h)) is a thin wrapper around `QSettings`.

## Inter-process / Desktop Integration

| Feature                   | Code                                                                    |
| ------------------------- | ----------------------------------------------------------------------- |
| MPRIS2 (Linux)            | [`mpris2/mpris2.{h,cpp}`](../src/mpris2/) over Qt D-Bus                |
| Global shortcuts          | [`globalshortcuts/`](../src/globalshortcuts/) — X11, KGlobalAccel, mac, win |
| Discord Rich Presence     | [`discord/discordrichpresence.{h,cpp}`](../src/discord/)                |
| System tray icon          | [`systemtrayicon/`](../src/systemtrayicon/) — Qt vs. native macOS       |
| OSD (on-screen display)   | [`osd/`](../src/osd/) — `OSDPretty`, `OSDDBus` (Linux), `OSDMac`        |
| Single-instance           | `KDSingleApplication` (see `main.cpp`)                                  |

## Provider Pattern (Covers, Lyrics, Streaming, Radio)

Strawberry uses a recurring "**registry of providers**" pattern. For each pluggable surface:

| Surface       | Base class                                         | Registry                                                    |
| ------------- | -------------------------------------------------- | ----------------------------------------------------------- |
| Album covers  | [`covermanager/coverprovider.h`](../src/covermanager/coverprovider.h) | [`covermanager/coverproviders.h`](../src/covermanager/coverproviders.h) |
| Lyrics        | [`lyrics/lyricsprovider.h`](../src/lyrics/lyricsprovider.h)         | [`lyrics/lyricsproviders.h`](../src/lyrics/lyricsproviders.h)           |
| Streaming     | [`streaming/streamingservice.h`](../src/streaming/streamingservice.h) | [`streaming/streamingservices.h`](../src/streaming/streamingservices.h) |
| Radio         | [`radios/radioservice.h`](../src/radios/radioservice.h)             | [`radios/radioservices.h`](../src/radios/radioservices.h)               |
| Scrobblers    | [`scrobbler/scrobblerservice.h`](../src/scrobbler/scrobblerservice.h) | [`scrobbler/audioscrobbler.h`](../src/scrobbler/audioscrobbler.h)       |
| Playlist parsers | [`playlistparsers/parserbase.h`](../src/playlistparsers/parserbase.h) | [`playlistparsers/playlistparser.h`](../src/playlistparsers/playlistparser.h) |

Adding a new provider is described in [`07-common-workflows.md`](./07-common-workflows.md).

## Files Not Built Into the Single `strawberry` Target

Almost everything in `src/` is compiled into the single `strawberry` executable target (see top-level [`CMakeLists.txt`](../CMakeLists.txt)). There is **no library split** into front-end vs back-end — `tests/` link a `strawberry_lib` produced by the same target (look for `add_library(strawberry_lib …)` in the build).