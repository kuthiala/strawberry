/*
 * Strawberry Music Player
 * Copyright 2026, Strawberry contributors
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef AUDIOBOOKMERGER_H
#define AUDIOBOOKMERGER_H

#include "config.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <functional>

#include "core/song.h"
#include "core/musicstorage.h"
#include "organize/organize.h"
#include "includes/shared_ptr.h"

class TagReaderClient;

// AudiobookMerger pre-processes a selection of tracks destined for an iPod
// "Audiobook" sync. The iPod firmware groups audiobooks by their byte-identical
// (album, album-artist) pair and only ever shows ONE representative title per
// group, so a book ripped as N separate chapter files tends to fragment into N
// single-chapter "audiobooks" whose names are the chapter titles rather than
// the book title.
//
// Rather than fight the firmware's grouping, this helper combines every chapter
// file that shares the same book (keyed on effective_album) into a SINGLE `.m4b`
// file, with one chapter marker per original track so per-chapter navigation and
// resume-position are preserved. The merged file is then synced as one track, so
// one book == one audiobook, guaranteed.
//
// Audio quality is preserved: when every input is already AAC/ALAC inside an MP4
// container the merge is a lossless stream copy (`ffmpeg -c copy`). For mixed or
// non-MP4 sources (MP3/FLAC/OGG/...) a true stream copy is impossible, so the
// book is re-encoded once to high-bitrate AAC (the user-selected policy).
//
// This runs synchronously on the calling (GUI) thread via QProcess; it is
// intended to be invoked from OrganizeDialog::accept() BEFORE the Organize
// pipeline is constructed, so the rest of the sync path only ever sees the
// already-merged `.m4b` tracks.
class AudiobookMerger : public QObject {
  Q_OBJECT

 public:
  explicit AudiobookMerger(QObject *parent = nullptr);

  // Optional progress hooks so the caller can drive a modal progress/cancel
  // dialog. `on_progress(done, total, book_title)` is invoked before each book
  // starts; `is_cancelled()` is polled frequently (including while ffmpeg runs)
  // and, when it returns true, the current ffmpeg is killed and the merge stops.
  using ProgressCallback = std::function<void(int done, int total, const QString &book_title)>;
  using CancelCallback = std::function<bool()>;
  void SetProgressCallback(ProgressCallback callback) { progress_callback_ = std::move(callback); }
  void SetCancelCallback(CancelCallback callback) { cancel_callback_ = std::move(callback); }

  // Optional tag reader used to extract embedded cover art (art_embedded) from
  // the first chapter when no sidecar/manual cover file is available on disk.
  void SetTagReaderClient(SharedPtr<TagReaderClient> tagreader_client) { tagreader_client_ = tagreader_client; }

  struct Result {
    // One synthetic NewSongInfo per book, each pointing at a merged `.m4b` on
    // disk. Ready to hand straight to Organize.
    Organize::NewSongInfoList songs_info;
    // Per-book chapter markers, keyed by the merged `.m4b` local file path
    // (song.url().toLocalFile()). Carried through to GPodDevice, which writes
    // them into the iTunesDB track so the iPod shows real chapter navigation.
    QMap<QString, MusicStorage::AudiobookChapterList> chapters_by_file;
    // Human-readable per-book log lines (both successes and failures).
    QStringList log;
    // Books that could not be merged (e.g. ffmpeg missing/failed). The caller
    // should surface these; the corresponding tracks are omitted from
    // songs_info so a half-merged book is never synced.
    QStringList errors;
    bool ok = true;  // false if ANY book failed to merge.
    bool cancelled = false;  // true if the user aborted mid-way.
  };

  // Group `songs_info` by book (effective_album) and merge each group into one
  // `.m4b` under the cache dir. `format` is reused to compute the on-device
  // filename for each merged book via OrganizeDialog::ComputeNewSongsFilenames.
  Result MergeByBook(const Organize::NewSongInfoList &songs_info, const OrganizeFormat &format);

  // Returns the resolved ffmpeg executable path, or an empty string if none was
  // found. Exposed so callers can fail fast with a clear message.
  static QString FindFfmpeg();

 private:
  // Merge one book's ordered chapter files into out_m4b. Fills chapter markers.
  // Returns true on success; on failure error is populated. `stream_copy` is
  // set to whether a lossless copy was used (for logging).
  bool MergeOneBook(const SongList &chapters, const QString &out_m4b, bool *stream_copy, QString &error) const;

  // True when every chapter is already AAC/ALAC inside an MP4/M4A/M4B container,
  // i.e. a lossless `-c copy` concat is safe.
  static bool CanStreamCopy(const SongList &chapters);

  // Build the ffmetadata file (title/artist/album + [CHAPTER] blocks) for a book
  // and write it to metadata_path. Returns false on write failure.
  static bool WriteMetadataFile(const SongList &chapters, const QString &metadata_path, QString &error);

 private:
  QString ffmpeg_;
  ProgressCallback progress_callback_;
  CancelCallback cancel_callback_;
  SharedPtr<TagReaderClient> tagreader_client_;

  // User-configurable merge policy, read from AudiobookSettings at construction.
  bool force_reencode_;   // Always re-encode even when a lossless copy is possible.
  QString aac_bitrate_;   // ffmpeg bitrate string used when re-encoding (e.g. "256k").
};

#endif  // AUDIOBOOKMERGER_H
