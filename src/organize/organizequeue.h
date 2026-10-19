/*
 * Strawberry Music Player
 * This file is part of Strawberry.
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

#ifndef ORGANIZEQUEUE_H
#define ORGANIZEQUEUE_H

#include "config.h"

#include <atomic>

#include <QObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QPointer>

#include "includes/shared_ptr.h"
#include "core/song.h"
#include "core/musicstorage.h"
#include "organize.h"
#include "organizeformat.h"

class TaskManager;
class TagReaderClient;
class CollectionBackend;

// Queue of "Copy to device" / "Organize files" jobs.
//
// Before this existed, every OrganizeDialog::accept() started its own Organize
// immediately. A second sync to the same iPod then blocked its worker thread on
// GPodDevice::db_busy_ until the first finished, the sidebar progress pane was
// wiped by the second job, the first job's eject_after could unmount the iPod
// under the second, and audiobook merging blocked the whole UI behind a modal
// progress dialog.
//
// Now OrganizeDialog hands every job to this queue and returns immediately:
//
//  - Jobs to the SAME destination run one at a time, in the order they were
//    enqueued. Jobs to different destinations run in parallel.
//  - Audiobook jobs first go through a Preparing phase (chapter merging with
//    ffmpeg) on a worker thread. Merging runs while another job is copying, and
//    a job that is still preparing never blocks ready jobs queued behind it.
//  - "Eject after" is deferred until the last queued job for that device ends.
//  - Queued, preparing and running jobs can be cancelled. A cancelled running
//    job stops after the song in flight and still writes the device database,
//    so everything copied so far is kept.
//  - Jobs still waiting for a device that is disconnected are cancelled.
//
// Lives on the main thread, owned by DeviceManager. All public methods must be
// called from the main thread.
class OrganizeQueue : public QObject {
  Q_OBJECT

 public:
  explicit OrganizeQueue(QObject *parent = nullptr);
  ~OrganizeQueue() override;

  enum class JobState {
    Preparing,  // Audiobook chapters are being merged.
    Queued,     // Waiting for the destination to be free.
    Running,    // Organize is copying.
    Finished,   // Every song was copied.
    Failed,     // Some or all songs failed, or preparing failed.
    Cancelled,  // Cancelled by the user or because the device went away.
  };

  struct JobRequest {
    JobRequest() : copy(true), overwrite(false), albumcover(false), eject_after(false), media_type(MusicStorage::DeviceMediaType::Music), merge_audiobooks(false) {}
    SharedPtr<TaskManager> task_manager;
    SharedPtr<TagReaderClient> tagreader_client;
    SharedPtr<CollectionBackend> collection_backend;
    SharedPtr<MusicStorage> destination;
    QString destination_label;
    OrganizeFormat format;
    bool copy;
    bool overwrite;
    bool albumcover;
    bool eject_after;
    Organize::NewSongInfoList songs_info;
    QStringList original_roots;
    MusicStorage::DeviceMediaType media_type;
    MusicStorage::PodcastInfo podcast_info;
    QString playlist;
    // Merge each audiobook's chapter files into one .m4b in the Preparing phase.
    bool merge_audiobooks;
  };

  struct JobInfo {
    quint64 id = 0;
    QString title;
    QString destination_label;
    MusicStorage::DeviceMediaType media_type = MusicStorage::DeviceMediaType::Music;
    JobState state = JobState::Queued;
    int total = 0;
    int done = 0;
    int failed = 0;
    // Free-form progress text for the Preparing phase, e.g. "Combining book 2 of 3".
    QString detail;
  };

  // Process-wide instance registered by DeviceManager, so every OrganizeDialog can reach it without being plumbed.
  static OrganizeQueue *instance();
  static void SetInstance(OrganizeQueue *queue);

  // Adds a job and starts it as soon as its destination is free. Returns the job id.
  quint64 Enqueue(const JobRequest &request);

  void Cancel(const quint64 id);
  void CancelAll();
  // Forgets finished, failed and cancelled jobs.
  void ClearFinished();

  QList<JobInfo> jobs() const;
  JobInfo job(const quint64 id) const;
  bool HasActiveJobs() const;

  static bool IsFinalState(const JobState state) { return state == JobState::Finished || state == JobState::Failed || state == JobState::Cancelled; }
  static QString MediaTypeName(const MusicStorage::DeviceMediaType media_type);
  static QString StateName(const JobState state);

 Q_SIGNALS:
  // A job was added, songs are the songs it will copy, in order.
  void JobAdded(const quint64 id, const SongList &songs);
  // The songs of a job changed, this happens when audiobook chapters were merged into books.
  void JobSongsChanged(const quint64 id, const SongList &songs);
  // State, counters or detail text of a job changed.
  void JobChanged(const quint64 id);
  // A job was removed by ClearFinished().
  void JobRemoved(const quint64 id);
  void SongStarted(const quint64 id, const Song &song);
  void SongFinished(const quint64 id, const Song &song, const bool success);
  void SongRetry(const quint64 id, const Song &song, const int attempt, const int max_attempts, const qint64 delay_ms);
  // The job ended, in any of the final states.
  void JobFinished(const quint64 id, const QStringList &files_with_errors, const QStringList &log, const bool success);

 private:
  struct Job {
    JobInfo info;
    JobRequest request;
    QMap<QString, MusicStorage::AudiobookChapterList> audiobook_chapters;
    QStringList prepare_errors;
    QPointer<Organize> organize;
    bool cancel_requested = false;
  };

  Job *FindJob(const quint64 id);
  const Job *FindJob(const quint64 id) const;
  static QString JobTitle(const JobRequest &request);
  static SongList SongsOf(const Organize::NewSongInfoList &songs_info);

  void WatchDestination(const SharedPtr<MusicStorage> &destination);
  void DestinationClosed(const MusicStorage *destination);

  void StartPreparing(Job *job);
  void PrepareFinished(const quint64 id);
  void StartJob(Job *job);
  void JobDone(const quint64 id, const QStringList &files_with_errors, const QStringList &log, const bool success);
  void FinishJob(Job *job, const JobState state, const QStringList &files_with_errors, const QStringList &log, const bool success);
  void MaybeEject(const SharedPtr<MusicStorage> &destination);

  // Starts every job that can start now: one preparing job at a time, and the oldest queued job of each idle destination.
  void Pump();

 private:
  QList<Job*> jobs_;
  quint64 next_id_;
  // The job currently preparing, preparing jobs run one at a time since the audiobook merger writes to a shared cache directory.
  quint64 preparing_id_;
  SharedPtr<std::atomic<bool>> prepare_cancel_;
  // Destinations which had a job requesting eject, ejected when their last job ended.
  QList<const MusicStorage*> eject_requested_;
  QList<const MusicStorage*> watched_destinations_;
};

#endif  // ORGANIZEQUEUE_H
