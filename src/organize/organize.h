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

#ifndef ORGANISE_H
#define ORGANISE_H

#include "config.h"

#include <optional>

#include <QObject>
#include <QBasicTimer>
#include <QFileInfo>
#include <QSet>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "includes/shared_ptr.h"
#include "core/song.h"
#include "organizeformat.h"

class QThread;
class QTimer;
class QTimerEvent;

class TaskManager;
class TagReaderClient;
class MusicStorage;
class Transcoder;

class Organize : public QObject {
  Q_OBJECT

 public:
  struct NewSongInfo {
    explicit NewSongInfo() : unique_filename_(false) {}
    explicit NewSongInfo(const Song &song, const QString &new_filename, const bool unique_filename) : song_(song), new_filename_(new_filename), unique_filename_(unique_filename) {}
    Song song_;
    QString new_filename_;
    bool unique_filename_;
  };
  using NewSongInfoList = QList<NewSongInfo>;

  explicit Organize(const SharedPtr<TaskManager> task_manager,
                    const SharedPtr<TagReaderClient> tagreader_client,
                    const SharedPtr<MusicStorage> destination,
                    const OrganizeFormat &format,
                    const bool copy,
                    const bool overwrite,
                    const bool albumcover,
                    const NewSongInfoList &songs,
                    const bool eject_after,
                    const QString &playlist = QString(),
                    QObject *parent = nullptr);

  ~Organize() override;

  void Start();

 public:
  // Retry policy for songs that fail to copy or commit to the destination.
  // Exponential backoff: 2s, 4s, 8s, 16s, 32s, 64s, 128s, 256s, 300s (capped),
  // then final-fail on the 10th failed attempt. Total worst-case wait before
  // giving up on a single song is ~17 minutes. These values were chosen so a
  // transient device hiccup (USB unplug-replug, iTunesDB locked by Finder,
  // libgpod sync race) gets several quick retries while a hard failure
  // (corrupted source file, no free space) still surfaces within minutes.
  static constexpr int kMaxAttempts = 10;
  static constexpr int kInitialBackoffMs = 2000;
  static constexpr int kMaxBackoffMs = 5 * 60 * 1000;

 Q_SIGNALS:
  void Finished(const QStringList &files_with_errors, const QStringList &log);
  void FileCopied(const int database_id);
  void SongPathChanged(const Song &song, const QFileInfo &new_file, const std::optional<int> new_collection_directory_id);

  // Emitted once when Start() is called, with the ordered list of songs that
  // are about to be processed. Used by the device-sync-progress pane in the
  // sidebar to populate its checklist.
  void SyncQueueReady(const SongList &queue);
  // Emitted after each song finishes (success or *final* failure -- i.e. all
  // retries exhausted) so the sidebar pane can mark it with a green check /
  // red cross. Always emitted in the order the songs were processed.
  void SongSyncProgress(const Song &song, const bool success);
  // Emitted whenever a song fails an individual attempt but is being
  // re-queued for another try. The sidebar pane uses this to annotate the
  // row with "retry N/10 in Xs" without flipping the row to Failed (which
  // would imply giving up on the song). attempt is 1-based and indicates
  // the attempt that just failed; the next attempt is attempt + 1. delay_ms
  // is the wall-clock delay before the next attempt will start.
  void SongSyncRetry(const Song &song, const int attempt, const int max_attempts, const qint64 delay_ms);

 protected:
  void timerEvent(QTimerEvent *e) override;

 private Q_SLOTS:
  void ProcessSomeFiles();
  void FileTranscoded(const QString &input, const QString &output, const bool success);
  void LogLine(const QString &message);

 private:
  void SetSongProgress(const float progress, const bool transcoded = false);
  void UpdateProgress();
  Song::FileType CheckTranscode(const Song::FileType original_type) const;
  bool ShouldSkipFile(const QString &filename) const;

 private:
  struct Task {
    explicit Task(const NewSongInfo &song_info = NewSongInfo())
        : song_info_(song_info),
          transcode_progress_(0.0),
          attempts_(0),
          next_attempt_at_ms_(0) {}

    NewSongInfo song_info_;
    float transcode_progress_;
    QString transcoded_filename_;
    QString new_extension_;
    Song::FileType new_filetype_;

    // Retry bookkeeping. attempts_ counts how many *failed* attempts have
    // already happened for this task; on first entry it is 0. After the Nth
    // failure (N = 1..9) the task is re-queued with next_attempt_at_ms_ set
    // to the wall-clock time it becomes eligible to run again. On the 10th
    // failure (N == kMaxAttempts) the task is marked permanently failed
    // and added to files_with_errors_.
    int attempts_;
    qint64 next_attempt_at_ms_;  // Qt epoch ms; 0 = run immediately
  };

  QThread *thread_;
  QThread *original_thread_;
  const SharedPtr<TaskManager> task_manager_;
  const SharedPtr<TagReaderClient> tagreader_client_;
  Transcoder *transcoder_;
  QTimer *process_files_timer_;
  const SharedPtr<MusicStorage> destination_;
  QList<Song::FileType> supported_filetypes_;

  const OrganizeFormat format_;
  const bool copy_;
  const bool overwrite_;
  const bool albumcover_;
  const bool eject_after_;
  quint64 task_count_;
  const QString playlist_;

  QBasicTimer transcode_progress_timer_;
  QList<Task> tasks_pending_;
  QMap<QString, Task> tasks_transcoding_;
  int tasks_complete_;

  bool started_;

  int task_id_;
  int current_copy_progress_;
  bool finished_;

  QStringList files_with_errors_;
  QStringList log_;
};

#endif  // ORGANISE_H
