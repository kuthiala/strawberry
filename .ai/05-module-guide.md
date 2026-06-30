# 5. Module Guide

A catalogue of every top-level directory under [`src/`](../src/) and [`data/`](../data/), with the role of the most important files. Use this as a map when you're hunting for the right place to make a change.

> Naming convention reminder: each directory contains a feature; class names usually start with the directory name (e.g. `collection/` → `Collection*`, `playlist/` → `Playlist*`).

## Top-level Entry Points

| File                            | Role                                                              |
| ------------------------------- | ----------------------------------------------------------------- |
| [`src/main.cpp`](../src/main.cpp) | Process entry point. See [`02-architecture.md`](./02-architecture.md). |
| [`src/main.h`](../src/main.h)   | Platform-agnostic helpers consumed by `main.cpp`.                |
| [`src/config.h.in`](../src/config.h.in)   | Template for `config.h` — `HAVE_*` macros come from here.|
| [`src/version.h.in`](../src/version.h.in) | Template for `version.h` — version strings come from here.|

## `src/core/` — Cross-cutting Foundations

The "nervous system" of the application. Most other modules `#include` from here.

| File                                      | Purpose                                                                   |
| ----------------------------------------- | ------------------------------------------------------------------------- |
| `application.h/.cpp`                      | The DI container described in [`02-architecture.md`](./02-architecture.md). |
| `mainwindow.h/.cpp/.ui`                   | The main window (sidebar, central widget, toolbars, dock widgets).        |
| `player.h/.cpp` (+ `playerinterface.*`)   | Playback controller — queues track, drives the engine, handles state.    |
| `song.h/.cpp`                             | The central `Song` value class. **Used everywhere.** See [`06-data-model.md`](./06-data-model.md). |
| `songloader.h/.cpp`                       | Given a URL, produces one or more `Song`s (parses CUE sheets, M3Us, etc.).|
| `songmimedata.{h,cpp}` / `mimedata.{h,cpp}` | MIME data subclasses for drag-and-drop.                                 |
| `database.h/.cpp`                         | SQLite owner; runs schema migrations on startup.                          |
| `memorydatabase.h/.cpp`                   | In-memory SQLite subclass for tests / temporary work.                     |
| `sqlquery.{h,cpp}` / `sqlrow.{h,cpp}`     | Thin wrappers around `QSqlQuery` with logging and proper binding.         |
| `scopedtransaction.h/.cpp`                | RAII transaction wrapper.                                                 |
| `settings.h/.cpp` (+ `settingsprovider.*`)| Wrapper around `QSettings` plus a provider interface for tests.           |
| `taskmanager.h/.cpp`                      | Background-task registry used by the status bar / busy indicator.         |
| `thread.h/.cpp`                           | `Thread` subclass of `QThread` with friendly naming and shutdown.         |
| `iconloader.h/.cpp` / `standarditemiconloader.*` | App-wide icon lookup (resource + theme).                          |
| `stylehelper.{h,cpp}` / `stylesheetloader.{h,cpp}` | Apply QSS, theme colours.                                       |
| `commandlineoptions.{h,cpp}`              | Parses argv (uses `getopt_long`); also serialises commands to the running instance. |
| `logging.h/.cpp`                          | `qLog(Level)` macro, GLib log integration, category filters.              |
| `metatypes.{h,cpp}`                       | All the `qRegisterMetaType<…>()` calls — extend here when you add a new value type used across threads / signals. |
| `networkaccessmanager.{h,cpp}` / `threadsafenetworkdiskcache.*` / `networkproxyfactory.{h,cpp}` / `networktimeouts.{h,cpp}` | Networking primitives. |
| `httpbaserequest.{h,cpp}` / `jsonbaserequest.{h,cpp}` | Base classes for HTTP / JSON-API requesters.                  |
| `oauthenticator.{h,cpp}` / `localredirectserver.{h,cpp}` | OAuth helpers (loopback redirect, PKCE).                       |
| `urlhandler.{h,cpp}` / `urlhandlers.{h,cpp}` | Registers `tidal://`, `qobuz://`, etc. handlers with `Player`.         |
| `mergedproxymodel.{h,cpp}` / `multisortfilterproxy.{h,cpp}` | Custom Qt model proxies.                                  |
| `filesystemmusicstorage.{h,cpp}` / `musicstorage.{h,cpp}` | Abstraction for file vs. device music storage.              |
| `filesystemwatcher*.{h,cpp}`              | Inotify / Qt / Windows file system watchers (per platform).               |
| `deletefiles.{h,cpp}`                     | Background deletion of song files / DB rows.                              |
| `temporaryfile.{h,cpp}`                   | RAII temp files.                                                          |
| `standardpaths.{h,cpp}`                   | `QStandardPaths`-style helpers (adjusts for portable mode).              |
| `translations.{h,cpp}`                    | `.qm` loading, language fallback.                                         |
| `enginemetadata.{h,cpp}`                  | Metadata coming back from the engine (samplerate, bitrate, embedded tags). |
| `signalchecker.{h,cpp}`                   | Sanity-check `QObject::connect` calls in debug builds.                    |
| `simpletreemodel.h` / `simpletreeitem.h`  | Generic templated tree-model utilities used by several views.             |
| `mac_startup.{h,mm}` / `unixsignalwatcher.{h,cpp}` / `windows7thumbbar.{h,cpp}` | Per-platform hooks. |

## `src/utilities/` — Stateless Helper Functions

Free functions / namespaces — never owns state, easy to unit-test.

| File                       | Helpers                                                              |
| -------------------------- | -------------------------------------------------------------------- |
| `strutils.*`               | String case fold, sanitise, escape, etc.                             |
| `fileutils.*` / `filemanagerutils.*` | File ops, "show in file manager", path normalisation.       |
| `diskutils.*`              | Free space / volume info.                                            |
| `imageutils.*` / `coverutils.*` / `coveroptions.*` | Image scaling, cover loading & resizing.       |
| `cryptutils.*`             | Hashing / signing for OAuth.                                         |
| `mimeutils.*`              | MIME type ↔ extension tables.                                        |
| `macaddrutils.*`           | MAC address (for ListenBrainz device hash, etc.).                    |
| `envutils.*`               | `getenv` / `setenv` wrappers; WSL detection.                         |
| `screenutils.*`            | Geometry / DPI utilities.                                            |
| `randutils.*`              | Seeded randoms.                                                      |
| `timeutils.*`              | Pretty-printing durations, `QDateTime` conversions.                  |
| `threadutils.*`            | Thread priorities, sleep helpers.                                    |
| `transliterate.*`          | ICU-backed transliteration (used in search).                         |
| `textencodingutils.*`      | Charset detection / conversion.                                      |
| `xmlutils.*`               | Small `QXmlStreamReader` helpers.                                    |
| `colorutils.*`             | `QColor` math used by analyzers, moodbar.                            |
| `musixmatchprovider.*`     | Shared helper for Musixmatch (used by lyrics & cover provider).      |
| `sqlhelper.h`              | Header-only SQL utility functions.                                   |
| `winutils.*` / `macosutils.*` | Per-platform misc.                                                |

## `src/includes/` — Header-only Vocabulary

Tiny support headers. Most importantly the smart pointers (`scoped_ptr.h`, `shared_ptr.h`), `lazy.h`, `iconmapper.h`, plus platform-specific scoped wrappers.

## `src/constants/` — Settings Keys

One header per settings page / domain, exposing `inline constexpr` string keys and group names. **Anywhere you read/write a setting, you should be referencing one of these.**

Examples:

- `behavioursettings.h`, `appearancesettings.h`, `playlistsettings.h`, `collectionsettings.h`
- `coverssettings.h`, `lyricssettings.h`, `scrobblersettings.h`
- `tidalsettings.h`, `qobuzsettings.h`, `spotifysettings.h`, `subsonicsettings.h`
- `radiobrowsersettings.h`, `radioparadisesettings.h`, `somafmsettings.h`
- `mainwindowsettings.h`, `moodbarsettings.h`, `waveformsettings.h`, `seekbarsettings.h`
- `filefilterconstants.h`, `filenameconstants.h`, `filesystemconstants.h`, `timeconstants.h`
- `transcodersettings.h`, `networkproxysettings.h`, `notificationssettings.h`
- `globalshortcutssettings.h`, `contextsettings.h`

## `src/tagreader/` — Tag I/O

A request/reply layer around **TagLib**. Each operation (read file, save tag, save cover, save rating, save playcount) is a `Request`/`Reply` class pair. `TagReaderClient` is the entry point; it dispatches to a worker thread.

| File                             | Role                                                       |
| -------------------------------- | ---------------------------------------------------------- |
| `tagreaderclient.{h,cpp}`        | Public API — call this from anywhere.                      |
| `tagreaderbase.{h,cpp}`          | Abstract base for backend implementations.                 |
| `tagreadertaglib.{h,cpp}`        | TagLib-backed implementation (default).                    |
| `tagreadergme.{h,cpp}`           | Game-music-emu backend (chiptune formats).                 |
| `streamtagreader.{h,cpp}`        | Reads tags off network streams (uses sparsehash if HAVE_STREAMTAGREADER). |
| `tagreaderrequest.{h,cpp}` + variants | One per operation (read, write, save cover, save playcount, save rating, …). |
| `tagreaderreply.{h,cpp}` + variants   | Async replies returned to the caller.                |
| `savetagcoverdata.{h,cpp}` / `albumcovertagdata.{h,cpp}` | Cover-art payloads.                       |
| `savetagsoptions.h` / `tagid3v2version.h` | Small option structs.                              |

## `src/engine/` — Audio Engine

GStreamer-based playback engine and audio device discovery.

| File                          | Role                                                            |
| ----------------------------- | --------------------------------------------------------------- |
| `enginebase.{h,cpp}`          | Abstract engine API; consumed by `Player`.                      |
| `gstengine.{h,cpp}`           | The only implementation. Owns playback state, two pipelines for gapless / crossfade. |
| `gstenginepipeline.{h,cpp}`   | A single GStreamer pipeline for one track.                      |
| `gststartup.{h,cpp}`          | One-time GStreamer init / plugin registration.                  |
| `enginedevice.{h,cpp}`        | Value class for an audio output device.                         |
| `devicefinder.{h,cpp}` / `devicefinders.{h,cpp}` | Base class + registry.                       |
| `alsadevicefinder.{h,cpp}` / `alsapcmdevicefinder.{h,cpp}` | ALSA backends.                       |
| `pulsedevicefinder.{h,cpp}`   | PulseAudio.                                                     |
| `macosdevicefinder.{h,cpp}`   | Core Audio.                                                     |
| `directsounddevicefinder.{h,cpp}` / `mmdevicefinder.{h,cpp}` / `asiodevicefinder.{h,cpp}` / `uwpdevicefinder.{h,cpp}` | Windows backends. |
| `chromaprinter.{h,cpp}`       | Audio fingerprint extraction (Chromaprint) — used by MusicBrainz. |
| `ebur128analysis.{h,cpp}` / `ebur128measures.h` | EBU R128 loudness analysis.                  |
| `gstfastspectrum.{h,cpp}` / `gstfastspectrumplugin.{h,cpp}` | Custom GStreamer element for the moodbar. |
| `gstbufferconsumer.h` / `gsturl.h` / `AsyncOperations.h` | Internal utilities.              |

## `src/analyzer/` — Visualisers in the Toolbar

| File                              | Type                              |
| --------------------------------- | --------------------------------- |
| `analyzerbase.{h,cpp}`            | Base widget                       |
| `analyzercontainer.{h,cpp}`       | Switches between analyzers        |
| `blockanalyzer.{h,cpp}`           | Spectrum bars                     |
| `boomanalyzer.{h,cpp}`            | "Boom" style bars                 |
| `turbineanalyzer.{h,cpp}`         | Turbine variant                   |
| `sonogramanalyzer.{h,cpp}`        | Scrolling sonogram                |
| `waverubberanalyzer.{h,cpp}`      | Waverubber                        |
| `rainbowanalyzer.{h,cpp}`         | Rainbow (the Nyan-cat one)        |
| `fht.{h,cpp}`                     | FFT helper                        |

## `src/equalizer/` — Equalizer UI + State

`equalizer.{h,cpp,ui}` (the dialog) + `equalizerslider.{h,cpp}`. Communicates with the engine via signals on `EngineBase`.

## `src/collection/` — Local Library

The biggest module. Owns the local music library: scanning, DB schema for `songs`, the tree model shown in the UI, filter widgets, the "Group by" dialog.

| File                                                  | Role                                                          |
| ----------------------------------------------------- | ------------------------------------------------------------- |
| `collectionlibrary.{h,cpp}`                           | High-level façade — what `Application::collection()` returns. |
| `collectionbackend.{h,cpp}`                           | DB access (`songs`, `directories`, `subdirectories` tables).  |
| `collectionwatcher.{h,cpp}`                           | File system watcher + scanner thread.                         |
| `collectionmodel.{h,cpp}` / `collectionmodelupdate.{h,cpp}` | `QAbstractItemModel` tree grouped by Artist/Album/Year/Genre/etc. |
| `collectionitem.{h,cpp}` / `collectionitemdelegate.{h,cpp}` | Tree-item value type & custom delegate.                  |
| `collectionview.{h,cpp}` / `collectionviewcontainer.{h,cpp,ui}` | `QTreeView` + container with filter bar.            |
| `collectionfilter.{h,cpp}` / `collectionfilteroptions.{h,cpp}` / `collectionfilterwidget.{h,cpp,ui}` | Live filter / search across the library. |
| `collectiondirectory.h` / `collectiondirectorymodel.{h,cpp}` | The list-of-roots configuration.                       |
| `collectiontask.{h,cpp}`                              | Long-running task (re-scan, etc.).                            |
| `collectionplaylistitem.{h,cpp}`                      | `PlaylistItem` subclass for collection songs.                 |
| `collectionquery.{h,cpp}`                             | Builds SQL with filter / group-by / sort options.             |
| `groupbydialog.{h,cpp,ui}` / `savedgroupingmanager.{h,cpp,ui}` | "Group by ▸…" dialog.                                |

## `src/playlist/` — Playlists

Open, manage, drag-and-drop, persist playlists. Backed by the `playlists` and `playlist_items` tables.

Key files: `playlist.{h,cpp}` (model), `playlistview.{h,cpp}` (view), `playlistmanager.{h,cpp}` (multi-playlist controller, used by everything else as `playlist_manager()`), `playlistbackend.{h,cpp}` (DB), `playlistcontainer.{h,cpp,ui}` + `playlisttabbar.{h,cpp}` (tabbed UI), `playlistheader.{h,cpp}`, `playlistdelegates.{h,cpp}` (custom cell renderers).

Item types form a class hierarchy under `PlaylistItem`:

- `playlistitem.{h,cpp}` — base
- `songplaylistitem.{h,cpp}` — local file / collection song
- `streamplaylistitem.{h,cpp}` — generic stream
- `collectionplaylistitem.{h,cpp}` — library-aware (in `collection/`)
- `radiostreamplaylistitem.{h,cpp}` — internet radio (in `radios/`)
- `streamserviceplaylistitem.{h,cpp}` — Tidal/Spotify/Qobuz (in `streaming/`)

Undo support: `playlistundocommand*.{h,cpp}` — one class per operation (insert, remove, move, reorder, sort, shuffle).

Other useful files: `playlistfilter.{h,cpp}` (live filter), `playlistsaveoptionsdialog.{h,cpp,ui}`, `playlistsequence.{h,cpp,ui}` (repeat / shuffle mode), `songloaderinserter.{h,cpp}`, `dynamicplaylistcontrols.{h,cpp,ui}`.

## `src/queue/` — Play Queue

A separate, ordered queue of upcoming songs. Just `queue.{h,cpp}` (model) and `queueview.{h,cpp}` (sidebar tab).

## `src/playlistparsers/` — Playlist File Formats

| Parser            | Format                                |
| ----------------- | ------------------------------------- |
| `m3uparser.*`     | `.m3u`, `.m3u8`                       |
| `plsparser.*`     | `.pls`                                |
| `xspfparser.*`    | `.xspf`                               |
| `asxparser.*`     | `.asx` (XML form)                     |
| `asxiniparser.*`  | `.asx` (INI form)                     |
| `wplparser.*`     | `.wpl`                                |
| `cueparser.*`     | `.cue` sheets                         |
| `xmlparser.*`     | Common base for XML-based formats.    |
| `parserbase.{h,cpp}` | Abstract `ParserBase`              |
| `playlistparser.{h,cpp}` | Registry + dispatcher          |

## `src/smartplaylists/` — Smart & Dynamic Playlists

A wizard-driven query builder over the collection.

- `smartplaylistwizard*.{h,cpp,ui}` — top-level wizard.
- `smartplaylistquerywizardplugin*.{h,cpp,ui}` — query mode pages.
- `smartplaylistsearch.{h,cpp}` / `smartplaylistsearchterm.{h,cpp}` / `smartplaylistsearchtermwidget.{h,cpp,ui}` — search term model & UI.
- `smartplaylistsearchpreview.{h,cpp,ui}` — live preview of matching songs.
- `playlistgenerator.{h,cpp}` / `playlistquerygenerator.{h,cpp}` — back-end generators.
- `smartplaylistsmodel.{h,cpp}` / `smartplaylistsview*.{h,cpp,ui}` — the sidebar tab.

## `src/covermanager/` — Album Cover Art

| File                                                | Role                                                          |
| --------------------------------------------------- | ------------------------------------------------------------- |
| `coverprovider.{h,cpp}` / `jsoncoverprovider.{h,cpp}` | Provider base classes.                                      |
| `coverproviders.{h,cpp}`                            | Registry of all providers (priorities, enabled flags).        |
| `lastfmcoverprovider.{h,cpp}`                       | Last.fm                                                       |
| `musicbrainzcoverprovider.{h,cpp}`                  | MusicBrainz                                                   |
| `discogscoverprovider.{h,cpp}`                      | Discogs                                                       |
| `deezercoverprovider.{h,cpp}`                       | Deezer                                                        |
| `musixmatchcoverprovider.{h,cpp}`                   | Musixmatch                                                    |
| `opentidalcoverprovider.{h,cpp}`                    | OpenTidal (the unauthenticated Tidal API)                     |
| `tidalcoverprovider.{h,cpp}` / `spotifycoverprovider.{h,cpp}` / `qobuzcoverprovider.{h,cpp}` | Authenticated streaming-service covers. |
| `albumcoverfetcher.{h,cpp}` / `albumcoverfetchersearch.{h,cpp}` | Fan-out search across enabled providers.           |
| `albumcoverloader.{h,cpp}` (+ `loaderoptions.{h,cpp}`, `loaderresult.h`, `imageresult.h`) | Async load + scale + cache.       |
| `currentalbumcoverloader.{h,cpp}`                   | Specialised loader for the *currently-playing* track.         |
| `albumcoverchoicecontroller.{h,cpp}`                | "Set / fetch / unset cover" UI logic.                         |
| `albumcovermanager*.{h,cpp,ui}` / `albumcoversearcher*.{h,cpp,ui}` | Standalone Cover Manager dialog.              |
| `albumcoverexport*.{h,cpp,ui}` / `coverexportrunnable.{h,cpp}` | Export covers to disk.                            |
| `coverfromurldialog.{h,cpp,ui}` / `coversearchstatisticsdialog.{h,cpp,ui}` | Helper dialogs.                          |

## `src/lyrics/` — Lyrics Providers

Mirrors `covermanager/` but for lyrics.

- Base classes: `lyricsprovider.{h,cpp}`, `jsonlyricsprovider.{h,cpp}`, `htmllyricsprovider.{h,cpp}`.
- Registry: `lyricsproviders.{h,cpp}`.
- Fetch flow: `lyricsfetcher.{h,cpp}` + `lyricsfetchersearch.{h,cpp}`.
- Search payloads: `lyricssearchrequest.h`, `lyricssearchresult.h`.
- Providers: `geniuslyricsprovider`, `musixmatchlyricsprovider`, `ovhlyricsprovider`, `songlyricscomlyricsprovider`, `azlyricscomlyricsprovider`, `elyricsnetlyricsprovider`, `letraslyricsprovider`, `lrcliblyricsprovider`.

## `src/scrobbler/` — Scrobbling

- `audioscrobbler.{h,cpp}` — multiplexes scrobbles to all enabled services.
- `scrobblerservice.{h,cpp}` — base class.
- `scrobblersettingsservice.{h,cpp}` — settings glue.
- `scrobblercache.{h,cpp}` / `scrobblercacheitem.{h,cpp}` — offline queue.
- `scrobblemetadata.{h,cpp}` — payload struct.
- Providers: `lastfmscrobbler.{h,cpp}`, `listenbrainzscrobbler.{h,cpp}`, `subsonicscrobbler.{h,cpp}`.
- `lastfmimport.{h,cpp}` — import historical scrobbles from Last.fm.

## `src/musicbrainz/`

MusicBrainz lookups (acoustid + release-group search) for the "fetch missing tags" workflow. Couples with `engine/chromaprinter.*`.

## `src/streaming/` — Generic Streaming Service UI

Reusable building blocks for "this service has artists/albums/songs/search":

- `streamingservice.{h,cpp}` — base class for Tidal/Spotify/Qobuz/Subsonic services.
- `streamingservices.{h,cpp}` — registry.
- `streamingtabsview.{h,cpp,ui}` — top-level tab.
- `streamingcollectionview.{h,cpp}` / `streamingcollectionviewcontainer.{h,cpp,ui}` — artist/album/song tree.
- `streamingsearch*.{h,cpp,ui}` — search box, results model, sort model, item delegate, view.
- `streamingsongsview.{h,cpp}` — flat "songs" tab.
- `streamserviceplaylistitem.{h,cpp}` — `PlaylistItem` for streaming songs.
- `streamsongmimedata.{h,cpp}` — drag-and-drop payload.

Service-specific code is in its own directory:

### `src/subsonic/`

Subsonic-API client (also works for Navidrome, Airsonic, Gonic). Uses HTTP + XML / JSON.

### `src/tidal/`, `src/qobuz/`, `src/spotify/`

Unofficial integrations. Each has:

- `<svc>service.{h,cpp}` — implements `StreamingService`.
- `<svc>baserequest.{h,cpp}` — common HTTP boilerplate.
- `<svc>request.{h,cpp}` — main catalogue / search requests.
- `<svc>favoriterequest.{h,cpp}` — favourites / playlists.
- `<svc>streamurlrequest.{h,cpp}` — resolve a track to a streamable URL.
- `<svc>urlhandler.{h,cpp}` — registers `<svc>://` URL scheme.

(Spotify currently exposes mainly search + cover; full integration is limited.)

## `src/radios/` — Internet Radio

- `radioservice.{h,cpp}` / `radioservices.{h,cpp}` — base + registry.
- `radiomodel.{h,cpp}` / `radioitem.h` / `radiochannel.{h,cpp}` — data model.
- `radioview.{h,cpp}` / `radioviewcontainer.{h,cpp,ui}` — sidebar tab.
- `radiomimedata.{h,cpp}` / `radiostreamplaylistitem.{h,cpp}`.
- `radiobackend.{h,cpp}` — DB persistence.
- Services: `somafmservice`, `radioparadiseservice`, `radiobrowserservice` (+ `radiobrowsersearchview/.ui`/`radiobrowsersearchmodel`).

## `src/context/` — "Now Playing" Pane

- `contextview.{h,cpp}` — central pane with cover, info, lyrics, analyzer.
- `contextalbum.{h,cpp}` — album-art widget with hover effects.

## `src/dialogs/` — Modal Dialogs

| Dialog                          | Purpose                                          |
| ------------------------------- | ------------------------------------------------ |
| `about.{h,cpp,ui}`              | About box.                                       |
| `console.{h,cpp,ui}`            | SQL console for debugging.                       |
| `edittagdialog.{h,cpp,ui}`      | Edit ID3 / tag for one or more songs.            |
| `trackselectiondialog.{h,cpp,ui}` | "We found these tags, which one?" picker.      |
| `addstreamdialog.{h,cpp,ui}`    | Add a URL stream to the playlist.                |
| `userpassdialog.{h,cpp,ui}`     | Reusable username/password prompt.               |
| `deleteconfirmationdialog.{h,cpp}` | "Are you sure?" for destructive ops.          |
| `lastfmimportdialog.{h,cpp,ui}` | Import Last.fm scrobbles.                        |
| `messagedialog.{h,cpp,ui}` / `errordialog.{h,cpp,ui}` | Generic message/error popups.      |
| `saveplaylistsdialog.{h,cpp,ui}`| Bulk-save playlists.                             |

## `src/settings/` — Settings Pages

One page per concern; all subclass `SettingsPage`. Use these as templates when adding a new setting:

`appearancesettingspage`, `backendsettingspage`, `behavioursettingspage`, `collectionsettingspage` (+ `collectionsettingsdirectorymodel`), `contextsettingspage`, `coverssettingspage`, `globalshortcutssettingspage`, `lyricssettingspage`, `moodbarsettingspage`, `networkproxysettingspage`, `notificationssettingspage`, `playlistsettingspage`, `qobuzsettingspage`, `radiosettingspage`, `scrobblersettingspage`, `spotifysettingspage`, `subsonicsettingspage`, `tidalsettingspage`, `transcodersettingspage`, `waveformsettingspage`. The host dialog: `settingsdialog.{h,cpp,ui}`. Delegate for the list: `settingsitemdelegate.{h,cpp}`.

## `src/widgets/` — Reusable UI Building Blocks

Cross-cut widgets used by many modules:

- Sidebar: `fancytabwidget.{h,cpp}`, `fancytabbar.{h,cpp}`, `fancytabdata.{h,cpp}`.
- "Now playing" widget: `playingwidget.{h,cpp}`.
- Sliders: `sliderslider.{h,cpp}`, `prettyslider.{h,cpp}`, `volumeslider.{h,cpp}`, `stickyslider.{h,cpp}`, `trackslider.{h,cpp,ui}` + `tracksliderslider.{h,cpp}` + `tracksliderpopup.{h,cpp}`.
- Misc: `autoexpandingtreeview`, `busyindicator`, `clickablelabel`, `favoritewidget`, `forcescrollperpixel`, `freespacebar`, `groupediconview`, `lineedit`, `linetextedit`, `loginstatewidget` (+ `.ui`), `multiloadingindicator`, `renametablineedit`, `resizabletextedit`, `ratingwidget`, `stretchheaderview`.
- Mac extras: `searchfield_mac.mm`, `qocoa_mac.h`.

## `src/transcoder/` — Audio Transcoding

GStreamer-based transcoder with per-codec option widgets.

- `transcoder.{h,cpp}` — engine.
- `transcodedialog.{h,cpp,ui}` / `transcodelogdialog.ui` — main dialog.
- `transcoderoptionsdialog.{h,cpp,ui}` — codec-specific options host.
- `transcoderoptionsinterface.{h,cpp}` — common interface.
- Per-codec: `transcoderoptions{aac,asf,flac,mp3,opus,speex,vorbis,wavpack}.{h,cpp,ui}`.

## `src/organize/` — File Organisation ("Copy / Move to Collection")

Renames and copies files using a user-defined format string (`%artist/%album/%track %title.%extension` etc.).

- `organize.{h,cpp}` — the runner.
- `organizedialog.{h,cpp,ui}` / `organizeerrordialog.{h,cpp,ui}` — UI.
- `organizeformat.{h,cpp}` — format string parser and applier.
- `organizeformatvalidator.{h,cpp}` — Qt validator.
- `organizesyntaxhighlighter.{h,cpp}` — QSyntaxHighlighter for the format input.

## `src/fileview/` — Filesystem Browser Tab

`fileview.{h,cpp}`, `fileviewlist.{h,cpp}`, `fileviewtree.{h,cpp}`, `fileviewtreemodel.{h,cpp}`.

## `src/device/` — External Music Devices

Removable USB drives, MTP devices (Android, etc.), and iPod Classic.

| File                                           | Role                                                  |
| ---------------------------------------------- | ----------------------------------------------------- |
| `devicemanager.{h,cpp}`                        | Top-level controller.                                 |
| `devicelister.{h,cpp}`                         | Base class for backends below.                        |
| `udisks2lister.{h,cpp}`                        | Linux UDisks2 (via D-Bus).                            |
| `giolister.{h,cpp}`                            | GIO backend.                                          |
| `macosdevicelister.{h,mm}`                     | macOS DiskArbitration.                                |
| `cddalister.{h,cpp}` / `cddadevice.{h,cpp}` / `cddasongloader.{h,cpp}` | Audio CD (libcdio).            |
| `connecteddevice.{h,cpp}` / `filesystemdevice.{h,cpp}` | Connected device state / generic filesystem.   |
| `mtpdevice.{h,cpp}` / `mtploader.{h,cpp}` / `mtpconnection.{h,cpp}` | MTP (libmtp).                     |
| `gpoddevice.{h,cpp}` / `gpodloader.{h,cpp}`    | iPod Classic (libgpod). **See [`10-ipod-sync.md`](./10-ipod-sync.md) for the *non-obvious* SysInfo / hash58 / filetype-string contracts you must satisfy or the iPod will silently reject the synced database.** |
| `devicedatabasebackend.{h,cpp}` / `deviceinfo.{h,cpp}` | Persistent device records.                   |
| `deviceproperties.{h,cpp,ui}` / `devicestatefiltermodel.{h,cpp}` / `deviceview*.{h,cpp,ui}` | UI. |
| `org.freedesktop.UDisks2.*.xml` / `org.freedesktop.DBus.ObjectManager.xml` | D-Bus interface descriptions. |

## `src/osd/` — On-Screen Display

- `osdbase.{h,cpp}` — base (Notify-like fallback).
- `osddbus.{h,cpp}` — Linux desktop notifications via D-Bus.
- `osdmac.h/.mm` — macOS Notification Center.
- `osdpretty.{h,cpp}` — Strawberry's custom translucent OSD popup window.

## `src/systemtrayicon/`

- `systemtrayicon.{h,cpp}` — base.
- `qtsystemtrayicon.{h,cpp}` — Qt `QSystemTrayIcon` backend.
- `macsystemtrayicon.{h,mm}` — native `NSStatusItem` backend (better menus on macOS).

## `src/mpris2/` — MPRIS2 D-Bus Interface (Linux)

Implements the freedesktop MPRIS2 spec so KDE/GNOME volume controls, sound applets, etc. can drive Strawberry.

## `src/discord/` — Discord Rich Presence

Embeds the Discord IPC client and publishes track info as a "Rich Presence" state.

## `src/globalshortcuts/`

Platform-specific global hotkey handlers:

- X11 raw + KGlobalAccel on Linux.
- macOS HotKey API.
- Windows `RegisterHotKey`.

The settings page is `settings/globalshortcutssettingspage.{h,cpp,ui}`.

## `src/filterparser/` — Library Filter Mini-language

Parses queries like `artist:rush year>1980 lengthrating>=3`. Used by the collection filter bar.

- `filterparser.{h,cpp}` — top-level parser.
- `filtertree.{h,cpp}` + variants (`and`, `or`, `not`, `nop`, `term`, `columnterm`) — expression tree.
- `filterparser<type><comparator>comparator.{h,cpp}` — one comparator per (data type × operator).
- `filterparsersearchtermcomparator.{h,cpp}` — runtime comparator base.

## `src/moodbar/` and `src/waveform/` — Visualisations

- **Moodbar:** colourful seek-bar overlay generated from audio analysis. Uses FFTW3 when available.
- **Waveform:** classic peak-amplitude waveform under the seek bar.

Each module has `loader`, `controller`, optional `pipeline`, optional `proxystyle`, optional `builder`, and `renderer` components — visible in the test suite (`waveform*_test.cpp`).

## `src/translations/` — Compiled translations

Holds `.ts` and `.qrc` config. Builds `.qm` files that are either embedded as resources or installed separately.

## `data/` — Resources

- `data.qrc`, `icons.qrc` — Qt resource manifests. Anything new (icon, schema file, image, HTML page) **must be added to one of these** to be available at runtime.
- `schema/` — SQL schema + per-version migrations. See [`06-data-model.md`](./06-data-model.md).
- `pictures/` — splash, currenttrack bar, OSD background, etc.
- `icons/` — 22x22 / 32x32 / … / 128x128 / full/ — multi-size theme icons.
- `style/` — QSS stylesheets injected at runtime.
- `mood/` — sample moodbar.
- `text/ghosts.txt` — Easter-egg lyrics for "Ghosts" (used in tests, occasionally as a placeholder).
- `screenshot/` — README screenshot.

## `tests/` — Unit & Integration Tests

See [`03-build-and-test.md`](./03-build-and-test.md).

## `cmake/` — Build Helpers

| File                       | Purpose                                               |
| -------------------------- | ----------------------------------------------------- |
| `Version.cmake`            | Computes version strings from git.                    |
| `OptionalComponent.cmake`  | The `optional_component(NAME default "Desc" DEPENDS ...)` macro. |
| `OptionalSource.cmake`     | Conditionally adds sources to a target.               |
| `ParseArguments.cmake`     | CMake argument parser used by the above.              |
| `Deb.cmake` / `Rpm.cmake` / `Dmg.cmake` | Packaging targets.                       |
| `Toolchain-*-w64-mingw32-shared.cmake` | Cross-compile MinGW toolchains.            |