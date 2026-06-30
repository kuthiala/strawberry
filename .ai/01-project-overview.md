# 1. Project Overview

## What is Strawberry?

**Strawberry** is a free and open-source **music player and music collection organizer** for desktop platforms. It is a fork of Clementine (2018), with a focus on:

- **Audiophiles and music collectors** (high-quality formats, bit-perfect playback, EBU R128 loudness normalization).
- **Local libraries first** — Strawberry is fundamentally a library/collection manager that *also* talks to several streaming services.
- **Native desktop integration** — system tray, MPRIS2 on Linux, media keys on macOS/Windows, Discord Rich Presence, scrobbling.

It is licensed under **GPL v3 or later** ([`COPYING`](../COPYING)).

## Tech Stack

| Layer                 | Technology                                                                 |
| --------------------- | -------------------------------------------------------------------------- |
| Language              | **C++17** (with a small amount of Objective-C++ on macOS in `.mm` files)   |
| GUI Toolkit           | **Qt 6** (>= 6.4) — Widgets, Network, Concurrent, SQL, optional DBus       |
| Audio backend         | **GStreamer 1.x** (mandatory)                                              |
| Database              | **SQLite** (via Qt SQL, schema in `data/schema/`)                          |
| Tag reading/writing   | **TagLib** (>= 1.12, prefers TagLib 2.0)                                   |
| Internationalisation  | **Qt LinguistTools** + Crowdin for translations                            |
| Build system          | **CMake** (>= 3.13)                                                        |
| Single-instance       | **KDSingleApplication** (>= 1.1.0)                                         |
| Testing               | **GoogleTest / GMock**                                                    |
| Optional acoustids    | **Chromaprint** (for MusicBrainz fingerprint matching)                     |
| Optional FFT          | **FFTW3** (fast spectrum moodbar)                                          |
| Optional loudness     | **libebur128** (EBU R128 normalization)                                    |
| Optional devices      | **libmtp** (MTP), **libgpod** (iPod classic), **libcdio** (audio CD)       |
| Optional updaters     | **Sparkle** (macOS), **qtsparkle-qt6** (Windows)                           |

## High-Level Features (what the user gets)

- Playback of WAV, FLAC, WavPack, Ogg Vorbis/Opus/Speex, MPC, TrueAudio, AIFF, MP4, MP3, ASF, APE.
- **Local collection** management with file system watcher, smart/dynamic playlists, queues.
- **Audio CD** playback.
- **EBU R128** loudness analysis & normalization.
- **Cover art** fetching from Last.fm, MusicBrainz, Discogs, Musixmatch, Deezer, Tidal, Qobuz, Spotify.
- **Lyrics** fetching from Genius, Musixmatch, lyrics.ovh, songlyrics, azlyrics, elyrics, letras, lrclib.
- **Streaming integrations:** Subsonic-compatible (official), and unofficial Tidal/Spotify/Qobuz.
- **Internet radio:** SomaFM, Radio Paradise, RadioBrowser.
- **Scrobbling:** Last.fm and ListenBrainz.
- **Audio analyzers** (block, boom, turbine, sonogram, waverubber, rainbow).
- **Equalizer**, **moodbar**, **waveform** views.
- **Transcoder** (via GStreamer pipelines) to FLAC, WavPack, Vorbis, Opus, Speex, AAC, ASF, MP3.
- **Device sync** to USB/MTP/iPod.

## Platforms

| Platform   | Status                                                       |
| ---------- | ------------------------------------------------------------ |
| Linux      | Primary target, fully supported. WSL is *explicitly rejected* at runtime ([`src/main.cpp`](../src/main.cpp)). |
| FreeBSD/OpenBSD | Supported (community).                                 |
| macOS      | Supported (Sparkle for updates, native NSStatusItem, native OSD). |
| Windows    | Supported (MSVC and MinGW; uses qtsparkle).                  |

Some macOS/Windows-specific code lives in `.mm` (Objective-C++) and `winutils.*`/`windows7thumbbar.*` files. Conditional compilation uses Qt's `Q_OS_*` macros and CMake `optional_component` flags (see [`cmake/OptionalComponent.cmake`](../cmake/OptionalComponent.cmake)).

## Repository Layout (top level)

```
strawberry/
├── CMakeLists.txt          ← top-level build, lists every source file
├── README.md               ← user-facing intro & dependencies
├── CONTRIBUTING.md         ← commit message style, PR workflow
├── COPYING                 ← GPL-3+ license
├── Changelog               ← user-visible changes per release
├── .clang-format           ← code style (largely disabled — `DisableFormat: true`!)
├── crowdin.yml             ← translation config
├── cmake/                  ← CMake helpers (Version, Deb, Dmg, Rpm, OptionalComponent…)
├── data/                   ← resources packed into the binary
│   ├── data.qrc            ← Qt resource manifest
│   ├── icons.qrc           ← icon resource manifest
│   ├── html/, icons/, pictures/, style/, text/, mood/
│   └── schema/             ← SQL schema + migrations (schema.sql + schema-NN.sql)
├── debian/                 ← Debian packaging
├── src/                    ← all application source (see 05-module-guide.md)
└── tests/                  ← GoogleTest unit & integration tests
```

See [`02-architecture.md`](./02-architecture.md) for how `src/` is organised.

## Project Philosophy / Cultural Notes

- **Strict warnings:** the build sets `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Woverloaded-virtual` etc. New code should compile cleanly.
- **No Qt foreach / signals / slots keywords:** these are disabled at compile time (`QT_NO_FOREACH`, `QT_NO_KEYWORDS`).
- **No implicit string conversions:** `QT_NO_CAST_FROM_ASCII`, `QT_NO_CAST_TO_ASCII`, `QT_NO_URL_CAST_FROM_STRING`, `QT_NO_CAST_FROM_BYTEARRAY` are set.
- **Forked-from-Clementine heritage:** Many files still carry Clementine copyright headers (David Sansome, John Maguire). Keep these intact when modifying; **add a new copyright line** for substantial changes.
- **Sponsor-only macOS/Windows binaries:** the project is open source, but pre-built macOS/Windows installers are gated by sponsorship. This does not affect contribution.