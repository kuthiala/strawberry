/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef MUSICSTORAGE_H
#define MUSICSTORAGE_H

#include "config.h"

#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>

#include <QMetaType>
#include <QString>
#include <QList>
#include <QImage>

#include "includes/shared_ptr.h"
#include "song.h"

class MusicStorage {
 public:
  explicit MusicStorage();
  virtual ~MusicStorage() = default;

  enum Role {
    Role_Storage = Qt::UserRole + 100,
    Role_StorageForceConnect,
    Role_Capacity,
    Role_FreeSpace,
  };

  // Values are saved in the database - don't change
  enum class TranscodeMode {
    Transcode_Always = 1,
    Transcode_Never = 2,
    Transcode_Unsupported = 3,
  };

  using ProgressFunction = std::function<void (float progress)>;

  // Target media type when copying to an iPod (GPodDevice). Music is the
  // default and preserves the historical behaviour; Podcast and Audiobook
  // change how the track is flagged and grouped in the iTunesDB so the iPod
  // firmware surfaces it under the correct menu. Ignored by non-iPod storages.
  enum class DeviceMediaType {
    Music = 0,
    Podcast = 1,
    Audiobook = 2,
  };

  // Show-level podcast metadata gathered from the user (via PodcastDetailsDialog)
  // and applied to every track in a "Copy as podcast" batch. Unlike music, a
  // podcast needs a stable feed identity so the iPod firmware groups the
  // episodes under one browsable show instead of one entry per track.
  struct PodcastInfo {
    QString show_title_;   // The podcast show name (becomes album + grouping key).
    QString author_;       // Author / publisher (becomes artist / albumartist).
    QString feed_url_;     // Optional RSS/feed URL; used as the grouping identity.
    QString description_;  // Optional show description.
  };

  // One chapter marker for an audiobook. start_ms_ is the chapter's start
  // offset from the beginning of the merged file (milliseconds); title_ is the
  // chapter label shown on the device. These are written into the iTunesDB
  // track record via libgpod (itdb_chapterdata_*) so the iPod firmware shows
  // real chapter navigation -- the in-file QuickTime chapter track that ffmpeg
  // writes is ignored by the iPod, so this is the only reliable path.
  struct AudiobookChapter {
    quint64 start_ms_ = 0;
    QString title_;
  };
  using AudiobookChapterList = QList<AudiobookChapter>;


  struct CopyJob {
    CopyJob() : overwrite_(false), remove_original_(false), albumcover_(false), media_type_(DeviceMediaType::Music) {}
    QString source_;
    QString destination_;
    Song metadata_;
    bool overwrite_;
    bool remove_original_;
    bool albumcover_;
    // iPod media-type routing. For Music this is a no-op (default behaviour).
    // For Podcast/Audiobook, GPodDevice flags the track accordingly and places
    // it in the right playlist / menu. Ignored by non-iPod storages.
    DeviceMediaType media_type_;
    // Only meaningful when media_type_ == Podcast. Carries the show-level
    // metadata the user supplied so all episodes group under one show.
    PodcastInfo podcast_info_;
    // Only meaningful when media_type_ == Audiobook and the track is a merged
    // book. When non-empty, GPodDevice attaches these as real iPod chapters.
    AudiobookChapterList audiobook_chapters_;
    QString cover_source_;
    QString cover_dest_;
    QImage cover_image_;
    ProgressFunction progress_;
    QString playlist_;
  };

  struct DeleteJob {
    DeleteJob() : use_trash_(false) {}
    Song metadata_;
    bool use_trash_;
  };

  virtual Song::Source source() const = 0;
  virtual QString LocalPath() const { return QString(); }
  virtual std::optional<int> collection_directory_id() const { return std::optional<int>(); }

  virtual TranscodeMode GetTranscodeMode() const { return TranscodeMode::Transcode_Never; }
  virtual Song::FileType GetTranscodeFormat() const { return Song::FileType::Unknown; }
  virtual bool GetSupportedFiletypes(QList<Song::FileType> *ret) { Q_UNUSED(ret); return true; }

  virtual bool StartCopy(QList<Song::FileType> *supported_types) { Q_UNUSED(supported_types); return true; }
  virtual bool CopyToStorage(const CopyJob &job, QString &error_text) = 0;
  // Commit the storage after a single CopyToStorage so that the unit of work
  // is one song -- if the next song fails (or the app crashes), the work done
  // for the song just copied is preserved on disk. Default no-op for storages
  // that don't have an explicit commit step (e.g. plain filesystem); GPodDevice
  // overrides this to flush the iTunesDB via WriteDatabase. Returns true on
  // success; on failure, error_text describes the problem and Organize will
  // treat the just-copied song as failed.
  virtual bool CommitCopy(QString &error_text) { Q_UNUSED(error_text); return true; }
  virtual bool FinishCopy(bool success, QString &error_text) { Q_UNUSED(error_text); return success; }

  virtual void StartDelete() {}
  virtual bool DeleteFromStorage(const DeleteJob &job) = 0;
  virtual bool FinishDelete(bool success, QString &error_text) { Q_UNUSED(error_text); return success; }

  virtual void Eject() {}

 private:
  Q_DISABLE_COPY(MusicStorage)
};

Q_DECLARE_METATYPE(MusicStorage*)
Q_DECLARE_METATYPE(SharedPtr<MusicStorage>)

#endif  // MUSICSTORAGE_H
