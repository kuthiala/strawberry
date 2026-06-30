# 9. Glossary

Domain-specific terms you'll meet in this codebase. Glance here when an unfamiliar word appears in a filename, comment, or variable.

## Project-specific

| Term                      | Meaning                                                                                                   |
| ------------------------- | --------------------------------------------------------------------------------------------------------- |
| **Strawberry**            | This project. A Qt 6 / C++17 desktop music player & collection manager.                                  |
| **Clementine**            | Strawberry's upstream — Strawberry was forked from Clementine in 2018. Many files keep Clementine copyright headers. |
| **Collection / Library**  | The user's *local* music library (files on disk, indexed in the `songs` table). |
| **Service mirror tables** | Parallel SQL tables (`tidal_songs`, `qobuz_*`, `spotify_*`, `subsonic_songs`) that cache streaming-service catalogues using the same column shape as `songs`. |
| **Source** (`Song::Source`) | Where a `Song` came from — `LocalFile`, `Collection`, `CDDA`, `Device`, `Stream`, `Tidal`, `Subsonic`, `Qobuz`, `Spotify`, `SomaFM`, `RadioParadise`, `RadioBrowser`. |
| **PlaylistItem**          | An entry in a playlist. Specialised subclasses: `SongPlaylistItem`, `StreamPlaylistItem`, `CollectionPlaylistItem`, `RadioStreamPlaylistItem`, `StreamServicePlaylistItem`. |
| **Smart playlist**        | A playlist defined by a query (e.g. "all 5-star rock from 1990–1999"), evaluated against the collection. |
| **Dynamic playlist**      | A smart playlist that continually appends matching tracks as the user plays through it. |
| **Moodbar**               | A colourful horizontal bar above the seek slider, generated from spectral analysis. |
| **Waveform**              | A peak-amplitude waveform under the seek slider — visual scrubbing aid. |
| **OSD**                   | On-Screen Display — the popup that shows the new track on song change. |
| **Provider**              | A pluggable source of metadata: cover art, lyrics, etc. Each provider subclasses a base (`CoverProvider`, `LyricsProvider`) and is registered with a registry (`CoverProviders`, `LyricsProviders`). |
| **Organize**              | The "Copy/Move to Collection" feature — renames files using a format string. |
| **Transcoder**            | GStreamer-based on-disk conversion of audio files (e.g. FLAC → MP3). |
| **AcoustID / fingerprint**| A perceptual hash of an audio file (via Chromaprint). Used by MusicBrainz to identify untagged files. |
| **Effective albumartist** | Derived field. If a song has no `albumartist` set, the collection uses `artist` (or "Various Artists" for compilations) so grouping works. |
| **Compilation effective** | Derived flag combining `compilation` (from the tag), `compilation_on` (user override yes), `compilation_off` (user override no), `compilation_detected` (heuristics). |
| **Cue sheet**             | A `.cue` text file describing track boundaries inside a single big audio file. Parsed by `playlistparsers/cueparser.{h,cpp}`. |
| **Stream**                | Any non-file URL — internet radio, Subsonic/Tidal/etc. URLs. |
| **Tab** (in the sidebar)  | A top-level area in the main window driven by `FancyTabWidget` — Collection / Playlists / Smart / Streaming / Radio / Files / Devices / Context. |
| **Context view**          | The "now playing" pane with cover, song info, lyrics, and an analyzer. |
| **Sparkle / qtsparkle**   | Auto-updater frameworks bundled on macOS / Windows respectively. |

## Domain (audio / music) terms

| Term            | Meaning                                                                                  |
| --------------- | ---------------------------------------------------------------------------------------- |
| **CDDA**        | Compact Disc Digital Audio — the bare audio CD format (handled via `libcdio`).          |
| **EBU R128**    | A loudness-measurement standard (LUFS / LRA). Strawberry uses `libebur128` to compute integrated loudness so different files play at similar volume. |
| **LUFS / LU**   | Loudness Units (Full Scale / relative). Output of EBU R128.                             |
| **Bit-perfect** | Playback that does not resample/convert; sends raw samples to the audio device. Strawberry supports this on Linux via ALSA. |
| **MPRIS2**      | A freedesktop.org D-Bus specification that lets media players expose state and accept commands from desktop environments. |
| **Scrobble**    | Reporting a track listen to Last.fm / ListenBrainz.                                     |
| **Gapless**     | Playing one track immediately after the previous one with no silence (matters for live albums, classical, mixes). |
| **MTP**         | Media Transfer Protocol — used by most Android devices and modern portable players.    |
| **iPod Classic**| Legacy iPods (pre-touch) — accessed via the proprietary iTunesDB format through `libgpod`. See [`10-ipod-sync.md`](./10-ipod-sync.md). |
| **iTunesDB**    | Binary database file at `iPod_Control/iTunes/iTunesDB` containing every track, playlist, and metadata blob on the iPod. Parsed/written by `libgpod`. |
| **libgpod**     | The C library that reads/writes the iTunesDB. Strawberry pins its own fork at `.idea/strawberry-libgpod/`. |
| **`Itdb_*`**    | libgpod's C structs (`Itdb_iTunesDB`, `Itdb_Track`, `Itdb_Playlist`, `Itdb_Device`). |
| **MPL**         | Master PLaylist — the synthetic "all tracks" playlist that must contain every track in iTunesDB. Tracks not in the MPL are silently hidden by the iPod firmware. |
| **SysInfo**     | Two text files under `iPod_Control/Device/` (`SysInfo` + `SysInfoExtended`) that libgpod consults for `FirewireGuid` and `ModelNumStr`. **Without these the synced DB will be rejected by the iPod.** |
| **FirewireGuid**| 64-bit per-device ID used as the secret salt for the iTunesDB HMAC. On all post-FireWire iPods it equals the USB serial number. |
| **ModelNumStr** | The iPod's model number (e.g. `MC297` = Classic 7G 160GB Black). libgpod looks it up in `ipod_info_table` to determine the generation, which in turn picks the hash scheme and artwork formats. |
| **hash58 / HASH58** | The pre-Touch checksum scheme libgpod can compute purely from `FirewireGuid`. The only scheme that works without help from iTunes/Music.app. |
| **hash72 / hashAB** | Newer checksum schemes used by Nano 5G+/Touch/iPhone/iPad. Require iTunes-extracted `HashInfo` — not supported by Strawberry. |
| **ArtworkDB**   | Side database at `iPod_Control/Artwork/ArtworkDB` that pairs with `*.ithmb` blob files to render cover art on the iPod's screen. |
| **`.ithmb`**    | "iTunes thumbnail" raw-pixel blob file. One blob per artwork size, each containing every track's thumbnail concatenated. |
| **UDisks2**     | Freedesktop D-Bus daemon Linux uses to manage removable media.                          |
| **GIO**         | "GNOME I/O" — a glib-based filesystem abstraction. Strawberry can use it to enumerate device mounts. |
| **Chromaprint** | Open-source audio fingerprinting library used by AcoustID / MusicBrainz.                |
| **AcoustID**    | Online lookup service that maps a Chromaprint hash to MusicBrainz IDs.                  |
| **MusicBrainz** | Open-source music database used for tag lookups, cover art (CAA), and IDs.              |
| **GStreamer**   | Cross-platform multimedia pipeline framework — Strawberry's audio backend. Pipelines look like `filesrc ! decodebin ! audioconvert ! equalizer ! volume ! pulsesink`. |
| **Pipeline (GStreamer)** | A directed graph of `GstElement` objects through which audio flows. Strawberry uses two of them to enable gapless / crossfade. |
| **TagLib**      | C++ library for reading/writing audio file metadata (ID3, Vorbis Comments, etc.).      |
| **ID3 / Vorbis Comments / APEv2** | Tag formats for MP3, Ogg/FLAC, APE files respectively.                       |

## Qt / C++ terms

| Term                     | Meaning                                                                                  |
| ------------------------ | ---------------------------------------------------------------------------------------- |
| **MOC**                  | Qt's Meta-Object Compiler — generates the runtime metadata used by signals/slots/`Q_OBJECT`. Triggered automatically by CMake's `automoc`. |
| **automoc / autouic / autorcc** | CMake features that run MOC, UIC (UI compiler), RCC (Resource compiler) for you. |
| **`Q_OBJECT`**           | Macro any class needs to participate in the Qt meta-object system (signals, slots, `tr()`). |
| **Signal / slot**        | Qt's observer pattern. A signal is a function whose body MOC generates; a slot is a normal method. Connect with `QObject::connect(...)`. |
| **Queued connection**    | A `connect()` mode that posts the slot invocation to the receiver's event loop — required when crossing threads. |
| **QString / QByteArray** | UTF-16 string / raw byte buffer (Qt's primary text types). |
| **QStringLiteral / u""_s**| Compile-time `QString` constructors that avoid heap allocation. |
| **QSharedData / QSharedDataPointer** | Qt's COW (copy-on-write) implementation for value classes like `Song`. |
| **`tr("…")`**            | Translation function. The string is extracted by `lupdate` and translated via `.ts`/`.qm`. |
| **Implicit sharing**     | Qt containers (`QString`, `QList`, …) share data between copies until written to. Cheap to copy. |
| **`QAbstractItemModel` / `QStandardItemModel`** | Qt's Model/View framework — used by the collection tree, playlist view, etc. |
| **Proxy model**          | A model that wraps another model to filter, sort, or rearrange. See `MergedProxyModel`, `MultiSortFilterProxy`. |
| **D-Bus**                | Inter-process bus used on Linux. Strawberry uses it for MPRIS2 and UDisks2 integration via Qt D-Bus. |
| **GLib main loop**       | GNOME's event loop. GStreamer needs one; Strawberry runs it on its own thread inside `Application`. |
| **`SharedPtr<T>` / `ScopedPtr<T>`** | Strawberry's smart-pointer aliases from `src/includes/` — semantically equivalent to `std::shared_ptr` and `std::unique_ptr`. |
| **CMake target**         | A build "thing" — an executable, a library, or a custom command. The main one is `strawberry`. |
| **Pkg-config / `pkg_check_modules`** | Tool/CMake macro to find dependencies via `.pc` files. Used heavily in this repo. |
| **`Q_DECLARE_METATYPE` / `qRegisterMetaType`** | Required to use a custom type in `QVariant` or across signal/slot queues. See `core/metatypes.cpp`. |

## File / config terms

| Term            | Meaning                                                                          |
| --------------- | -------------------------------------------------------------------------------- |
| **`.qrc`**      | Qt resource manifest — bundles assets into the binary.                          |
| **`.qss`**      | Qt stylesheet — CSS-like syntax for styling widgets.                            |
| **`.ui`**       | Qt Designer form file (XML). `uic` compiles it to `ui_<name>.h`.                |
| **`.ts` / `.qm`** | Translation source / compiled.                                                |
| **Crowdin**     | The translation platform (https://crowdin.com) Strawberry uses. See [`crowdin.yml`](../crowdin.yml). |
| **`config.h`**  | Generated from `src/config.h.in` — provides `HAVE_*` macros for optional features. |
| **`version.h`** | Generated from `src/version.h.in` — provides `STRAWBERRY_VERSION_*` macros.     |

## Acronyms You May Encounter

| Acronym  | Expanded                                                                |
| -------- | ----------------------------------------------------------------------- |
| GPL      | GNU General Public License                                              |
| LGPL     | Lesser GPL                                                              |
| FLAC     | Free Lossless Audio Codec                                               |
| ALAC     | Apple Lossless Audio Codec                                              |
| DSD/DSF/DSDIFF | Direct Stream Digital (high-res audio)                           |
| MPC      | Musepack                                                                |
| ASF      | Advanced Systems Format (Microsoft, hosts WMA)                          |
| APE      | Monkey's Audio                                                          |
| WPL      | Windows Media Playlist                                                  |
| XSPF     | XML Shareable Playlist Format                                           |
| PLS      | (Shoutcast / generic) playlist file                                     |
| M3U      | A simple playlist text-file format                                      |
| BPM      | Beats per minute                                                        |
| LRC      | Lyrics file format with timestamps (used by `lrclib`)                   |
| OAuth    | Open Authorization — the auth protocol used for Tidal/Qobuz/Spotify/etc.|
| PKCE     | Proof Key for Code Exchange — an OAuth extension                       |
| MIME     | Multipurpose Internet Mail Extensions — Strawberry uses MIME types for drag-and-drop and file handling. |