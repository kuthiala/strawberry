# 6. Data Model

This document covers Strawberry's persistent data: the SQLite schema, the in-memory `Song` value class, and how migrations are organised.

## The Database File

- Format: **SQLite 3** (≥ 3.9 required).
- Location: under `QStandardPaths::AppLocalDataLocation` (Linux: `~/.local/share/strawberry/strawberry/strawberry.db`).
- Per-thread connections (Qt SQL requirement) — see [`core/database.h`](../src/core/database.h) (`Database::Connect()`).
- A separate **device** database is created per attached music device (see `data/schema/device-schema.sql` and [`src/device/devicedatabasebackend.{h,cpp}`](../src/device/)).

## Schema Files

In [`data/schema/`](../data/schema/):

| File              | Purpose                                                                |
| ----------------- | ---------------------------------------------------------------------- |
| `schema.sql`      | The **current** baseline schema. Bootstraps a fresh DB at version `23`.|
| `schema-NN.sql`   | One per migration step. Applied in order from the current DB version. |
| `device-schema.sql` | Baseline schema for a per-device DB (`devices/<uuid>/strawberry.db`). |

When `Database` starts up:

1. Reads `schema_version` from the DB.
2. If empty, runs `schema.sql`.
3. Otherwise, iterates `schema-NN.sql` for each version after the current up to the embedded latest, running each in a transaction.
4. Bumps `schema_version` accordingly.

> **When you change the schema:**
>
> 1. Add a new `schema-NN.sql` migration (where `NN` is the next integer) under [`data/schema/`](../data/schema/).
> 2. Update **`schema.sql`** so a fresh install starts at version `NN` directly (don't ship users through every old migration).
> 3. Update the `INSERT INTO schema_version` value in `schema.sql` to `NN`.
> 4. Add the new file to [`data/data.qrc`](../data/data.qrc).
> 5. If your migration touches `songs` columns, also update **all** the parallel "service mirror" tables (see below) and the matching code in `Song::kColumns` ([`src/core/song.h`](../src/core/song.h) / `song.cpp`).
> 6. Add regression coverage in [`tests/src/collectionbackend_test.cpp`](../tests/src/collectionbackend_test.cpp).

## Top-level Tables

Inferred from [`data/schema/schema.sql`](../data/schema/schema.sql) (version 23):

### Library

- **`schema_version (version INTEGER)`** — single-row meta table.
- **`directories (path TEXT, subdirs INTEGER)`** — the user-configured root folders.
- **`subdirectories (directory_id INTEGER, path TEXT, mtime INTEGER)`** — discovered subdirectories with mtime for change detection.
- **`songs (…)`** — the *primary* library table. Schema below.

### Service Mirror Tables (parallel song tables)

Each of the following has **the same schema as `songs`** (every column, every default). They cache results from the corresponding online service so the user can browse offline-ish:

- `subsonic_songs`
- `tidal_artists_songs`, `tidal_albums_songs`, `tidal_songs`
- `spotify_artists_songs`, `spotify_albums_songs`, `spotify_songs`
- `qobuz_artists_songs`, `qobuz_albums_songs`, `qobuz_songs`

> Because they share a schema, when you add a column to `songs` you almost always have to add it to **all** these tables. Grep for the column name in `schema.sql` to be sure.

### Other tables

The remainder of `schema.sql` (which we have not quoted in full here) contains tables for:

- `playlists`, `playlist_items` — saved playlists and their entries.
- `playlist_view_state` — per-playlist column visibility/order.
- Radio: `radio_channels`, plus per-service caches.
- Scrobbler queue.
- Smart playlists.

Always consult the live schema (`data/schema/schema.sql`) before assuming column names.

## The `songs` Table (annotated)

```sql
CREATE TABLE IF NOT EXISTS songs (
  -- Display + sort variants of all "name" fields:
  title TEXT,                            titlesort TEXT,
  album TEXT,                            albumsort TEXT,
  artist TEXT,                           artistsort TEXT,
  albumartist TEXT,                      albumartistsort TEXT,
  -- Track ordering & era:
  track INTEGER NOT NULL DEFAULT -1,     disc INTEGER NOT NULL DEFAULT -1,
  year INTEGER NOT NULL DEFAULT -1,      originalyear INTEGER NOT NULL DEFAULT -1,
  -- Classification:
  genre TEXT,
  compilation INTEGER NOT NULL DEFAULT 0,
  composer TEXT,                         composersort TEXT,
  performer TEXT,                        performersort TEXT,
  grouping TEXT,
  comment TEXT,                          lyrics TEXT,

  -- Service IDs (used for streaming-service mirror tables; null for local files):
  artist_id TEXT,                        album_id TEXT,                song_id TEXT,

  -- Position / length within a (possibly multi-track) source:
  beginning INTEGER NOT NULL DEFAULT 0,  length INTEGER NOT NULL DEFAULT 0,

  -- Stream properties:
  bitrate INTEGER NOT NULL DEFAULT -1,
  samplerate INTEGER NOT NULL DEFAULT -1,
  bitdepth INTEGER NOT NULL DEFAULT -1,

  -- Where this song came from (`Song::Source` enum) and how to reach it:
  source INTEGER NOT NULL DEFAULT 0,     -- 0=Unknown, 1=LocalFile, 2=Collection, 3=CDDA…
  directory_id INTEGER NOT NULL DEFAULT -1,
  url TEXT NOT NULL,                     -- canonical URL (file://, tidal://, …)
  filetype INTEGER NOT NULL DEFAULT 0,   -- `Song::FileType`
  filesize INTEGER NOT NULL DEFAULT -1,
  mtime INTEGER NOT NULL DEFAULT -1,     ctime INTEGER NOT NULL DEFAULT -1,
  unavailable INTEGER DEFAULT 0,         -- file gone but row kept

  fingerprint TEXT,                      -- Chromaprint

  -- Statistics:
  playcount INTEGER NOT NULL DEFAULT 0,
  skipcount INTEGER NOT NULL DEFAULT 0,
  lastplayed INTEGER NOT NULL DEFAULT -1,
  lastseen INTEGER NOT NULL DEFAULT -1,  -- last time a scan saw this file

  -- "Is this a compilation?" — Strawberry tracks both the tag and user overrides:
  compilation_detected INTEGER DEFAULT 0,
  compilation_on INTEGER NOT NULL DEFAULT 0,
  compilation_off INTEGER NOT NULL DEFAULT 0,
  compilation_effective INTEGER NOT NULL DEFAULT 0,

  -- Cover art:
  art_embedded INTEGER DEFAULT 0,        -- ID3 picture frame found
  art_automatic TEXT,                    -- discovered alongside file (folder.jpg…)
  art_manual TEXT,                       -- user-picked
  art_unset INTEGER DEFAULT 0,           -- user explicitly cleared art

  -- Derived / cached aggregates (kept in sync by triggers / code):
  effective_albumartist TEXT,
  effective_originalyear INTEGER NOT NULL DEFAULT 0,

  cue_path TEXT,                         -- path to .cue if this song was loaded from one

  rating INTEGER DEFAULT -1,             -- 0..5 stars, -1 = unrated

  -- AcoustID / MusicBrainz IDs:
  acoustid_id TEXT,                      acoustid_fingerprint TEXT,
  musicbrainz_album_artist_id TEXT,      musicbrainz_artist_id TEXT,
  musicbrainz_original_artist_id TEXT,   musicbrainz_album_id TEXT,
  musicbrainz_original_album_id TEXT,    musicbrainz_recording_id TEXT,
  musicbrainz_track_id TEXT,             musicbrainz_disc_id TEXT,
  musicbrainz_release_group_id TEXT,     musicbrainz_work_id TEXT,

  -- EBU R128 loudness analysis results:
  ebur128_integrated_loudness_lufs REAL,
  ebur128_loudness_range_lu REAL,

  -- Extra audio analysis:
  bpm REAL,
  mood TEXT,                             -- moodbar data (binary as base64ish? see code)
  initial_key TEXT
);
```

**No primary key column is declared**, but SQLite's implicit `ROWID` is used as the song's `id`. Look for `ROWID AS id` in queries in `collectionbackend.cpp`.

## The `Song` Value Class

[`src/core/song.h`](../src/core/song.h) / [`src/core/song.cpp`](../src/core/song.cpp) is the **in-memory** mirror of a row in `songs`. It is the most-passed-around type in the codebase and you will encounter it everywhere.

Key members and enums:

```cpp
class Song {
 public:
  enum class Source {
    Unknown = 0, LocalFile = 1, Collection = 2, CDDA = 3, Device = 4,
    Stream = 5, Tidal = 6, Subsonic = 7, Qobuz = 8, SomaFM = 9,
    RadioParadise = 10, Spotify = 11, RadioBrowser = 12
  };

  enum class FileType {
    Unknown=0, WAV=1, FLAC=2, WavPack=3, OggFlac=4, OggVorbis=5, OggOpus=6,
    OggSpeex=7, MPEG=8, MP4=9, ASF=10, AIFF=11, MPC=12, TrueAudio=13,
    DSF=14, DSDIFF=15, PCM=16, APE=17, MOD=18, S3M=19, XM=20, IT=21,
    SPC=22, VGM=23, ALAC=24, CDDA=90, Stream=91
  };

  // Column metadata used to build SELECT/INSERT/UPDATE statements:
  static const QStringList kColumns;
  static const QString     kColumnSpec, kRowIdColumnSpec, kBindSpec, kUpdateSpec;

  // Subsets used by the filter parser:
  static const QStringList kTextSearchColumns, kIntSearchColumns,
                           kUIntSearchColumns, kInt64SearchColumns,
                           kFloatSearchColumns, kNumericalSearchColumns,
                           kSearchColumns;

  // Regex tables used to clean up sloppy album/title tags:
  static const RegularExpressionList kAlbumDisc, kRemastered, kExplicit,
                                     kAlbumMisc, kTitleMisc;
  static const QStringList kArticles;            // "the", "a", "an", …
  static const QStringList kAcceptedExtensions, kRejectedExtensions;

  // … many getters / setters returning QString / int / qint64 …
};
```

`Song` is **value-typed with implicit sharing** — it uses `QSharedData` / `QSharedDataPointer` internally (see the `#include <QSharedData>` at the top of the header). Copying is cheap; modifying triggers a detach.

### Conversion methods

`Song` knows how to read/write itself to/from several formats — look in `song.cpp` for these (names vary slightly):

| From / To              | Method                                  |
| ---------------------- | --------------------------------------- |
| `QSqlRecord` / `SqlRow`| `InitFromQuery()`, `BindToQuery()`      |
| Tag-reader payload     | `InitFromProtobuf()` / similar          |
| `EngineMetadata`       | `MergeFromEngineMetadata()`             |
| GPod (`Itdb_Track`)    | `InitFromItdb()`, `ToItdb()`            |
| MTP (`LIBMTP_track_struct`) | `InitFromMTP()`, `ToMTP()`         |
| File on disk           | `InitFromFile()`                        |

### When you add a new field to `songs`

1. Migration SQL — add the column to **every** parallel table (`songs`, `subsonic_songs`, all `tidal_*`, all `spotify_*`, all `qobuz_*`).
2. `Song::kColumns` and `kColumnSpec`/`kBindSpec`/`kUpdateSpec` in `song.cpp`.
3. Add a getter / setter and a member (`QSharedData` subclass).
4. `Song::InitFromQuery()` and `Song::BindToQuery()`.
5. If the field is searchable: include it in the appropriate `k*SearchColumns` list.
6. Tag reader: read/write it in [`tagreader/tagreadertaglib.cpp`](../src/tagreader/tagreadertaglib.cpp) (if it lives in file tags).
7. UI: update [`dialogs/edittagdialog.cpp`](../src/dialogs/edittagdialog.cpp) if the user should be able to edit it.
8. Tests: extend `collectionbackend_test.cpp` and `tagreader_test.cpp`.

## How a Song Is Loaded

```
User points to a URL (file://path/to/song.flac, or tidal://…)
        │
        ▼
SongLoader (core/songloader.cpp)
        │
        ├── If file URL with .cue → CueParser → multiple Songs
        ├── If file URL → TagReaderClient → fills tags
        ├── If playlist URL (.m3u/.pls/…) → PlaylistParser
        └── If stream URL → minimal Song with Source::Stream
        │
        ▼
List<Song> handed back to caller (usually PlaylistManager or Player)
```

## Cover Art Storage

Cover art is **not** stored in the DB itself — only paths/flags. Three states:

| Field            | Meaning                                                  |
| ---------------- | -------------------------------------------------------- |
| `art_embedded`   | The file's own tags include a picture frame.             |
| `art_automatic`  | A `folder.jpg` / `cover.png` / etc. discovered next to the file. |
| `art_manual`     | User picked / downloaded a specific image (cached path). |
| `art_unset`      | User explicitly removed art; don't keep looking.        |

The actual image bytes (when needed) come from `AlbumCoverLoader` / `CurrentAlbumCoverLoader` (see [`05-module-guide.md`](./05-module-guide.md) → `covermanager/`).

## Settings (not the DB)

User preferences are **not** in the SQLite database — they live in:

- **Linux:** `~/.config/strawberry/Strawberry.conf` (INI).
- **macOS:** `~/Library/Preferences/org.strawberrymusicplayer.strawberry.plist`.
- **Windows:** the registry under `HKEY_CURRENT_USER\Software\Strawberry\Strawberry`.

Access via [`Settings`](../src/core/settings.h) + the constants in [`src/constants/`](../src/constants/). See [`04-coding-conventions.md`](./04-coding-conventions.md).