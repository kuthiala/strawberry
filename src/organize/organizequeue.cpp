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

#include "config.h"

#include <memory>
#include <utility>

#include <QObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/song.h"
#include "core/musicstorage.h"
#include "collection/collectionbackend.h"
#include "device/connecteddevice.h"
#include "device/audiobookmerger.h"
#include "organize.h"
#include "organizequeue.h"

using std::make_shared;

namespace {
OrganizeQueue *g_instance = nullptr;
}  // namespace

OrganizeQueue *OrganizeQueue::instance() { return g_instance; }
void OrganizeQueue::SetInstance(OrganizeQueue *queue) { g_instance = queue; }

OrganizeQueue::OrganizeQueue(QObject *parent)
    : QObject(parent),
      next_id_(1),
      preparing_id_(0) {}

OrganizeQueue::~OrganizeQueue() {

  if (g_instance == this) g_instance = nullptr;

  // Running Organize objects delete themselves when their thread finished, cancel them so they stop soon.
  for (Job *job : std::as_const(jobs_)) {
    if (job->organize) job->organize->Cancel();
  }
  if (prepare_cancel_) prepare_cancel_->store(true);

  qDeleteAll(jobs_);
  jobs_.clear();

}

QString OrganizeQueue::MediaTypeName(const MusicStorage::DeviceMediaType media_type) {

  switch (media_type) {
    case MusicStorage::DeviceMediaType::Podcast:
      return tr("Podcast");
    case MusicStorage::DeviceMediaType::Audiobook:
      return tr("Audiobook");
    case MusicStorage::DeviceMediaType::Music:
      break;
  }

  return tr("Music");

}

QString OrganizeQueue::StateName(const JobState state) {

  switch (state) {
    case JobState::Preparing:
      return tr("Preparing");
    case JobState::Queued:
      return tr("Queued");
    case JobState::Running:
      return tr("Copying");
    case JobState::Finished:
      return tr("Done");
    case JobState::Failed:
      return tr("Failed");
    case JobState::Cancelled:
      return tr("Cancelled");
  }

  return QString();

}

QString OrganizeQueue::JobTitle(const JobRequest &request) {

  const qint64 count = request.songs_info.count();

  switch (request.media_type) {
    case MusicStorage::DeviceMediaType::Podcast:
      if (!request.podcast_info.show_title_.isEmpty()) {
        return tr("%1 (%n episode(s))", nullptr, static_cast<int>(count)).arg(request.podcast_info.show_title_);
      }
      return tr("%n podcast episode(s)", nullptr, static_cast<int>(count));
    case MusicStorage::DeviceMediaType::Audiobook:
      return tr("%n audiobook file(s)", nullptr, static_cast<int>(count));
    case MusicStorage::DeviceMediaType::Music:
      break;
  }

  if (!request.playlist.isEmpty()) {
    return tr("Playlist \"%1\" (%n song(s))", nullptr, static_cast<int>(count)).arg(request.playlist);
  }

  // Name the job after the album when every song is from the same one, which is the common case of copying an album.
  QString album;
  for (const Organize::NewSongInfo &song_info : request.songs_info) {
    const QString song_album = song_info.song_.effective_album();
    if (song_album.isEmpty() || (!album.isEmpty() && album != song_album)) {
      album.clear();
      break;
    }
    album = song_album;
  }
  if (!album.isEmpty()) {
    return tr("%1 (%n song(s))", nullptr, static_cast<int>(count)).arg(album);
  }

  return tr("%n song(s)", nullptr, static_cast<int>(count));

}

SongList OrganizeQueue::SongsOf(const Organize::NewSongInfoList &songs_info) {

  SongList songs;
  songs.reserve(songs_info.count());
  for (const Organize::NewSongInfo &song_info : songs_info) {
    songs << song_info.song_;
  }
  return songs;

}

OrganizeQueue::Job *OrganizeQueue::FindJob(const quint64 id) {

  for (Job *job : std::as_const(jobs_)) {
    if (job->info.id == id) return job;
  }
  return nullptr;

}

const OrganizeQueue::Job *OrganizeQueue::FindJob(const quint64 id) const {

  for (const Job *job : jobs_) {
    if (job->info.id == id) return job;
  }
  return nullptr;

}

QList<OrganizeQueue::JobInfo> OrganizeQueue::jobs() const {

  QList<JobInfo> infos;
  infos.reserve(jobs_.count());
  for (const Job *job : jobs_) {
    infos << job->info;
  }
  return infos;

}

OrganizeQueue::JobInfo OrganizeQueue::job(const quint64 id) const {

  const Job *job = FindJob(id);
  return job ? job->info : JobInfo();

}

bool OrganizeQueue::HasActiveJobs() const {

  for (const Job *job : jobs_) {
    if (!IsFinalState(job->info.state)) return true;
  }
  return false;

}

quint64 OrganizeQueue::Enqueue(const JobRequest &request) {

  Job *job = new Job;
  job->request = request;
  job->info.id = next_id_++;
  job->info.title = JobTitle(request);
  job->info.destination_label = request.destination_label;
  job->info.media_type = request.media_type;
  job->info.total = static_cast<int>(request.songs_info.count());
  const bool prepare = request.media_type == MusicStorage::DeviceMediaType::Audiobook && request.merge_audiobooks;
  job->info.state = prepare ? JobState::Preparing : JobState::Queued;
  if (prepare) job->info.detail = tr("Waiting to combine chapters");

  // Ejecting is done by the queue after the last job for the device, so a queued job is never left without its device.
  if (request.eject_after && request.destination && !eject_requested_.contains(request.destination.get())) {
    eject_requested_ << request.destination.get();
  }

  jobs_ << job;
  WatchDestination(request.destination);

  qLog(Info) << "Queued organize job" << job->info.id << job->info.title << "to" << job->info.destination_label << "-" << StateName(job->info.state);

  Q_EMIT JobAdded(job->info.id, SongsOf(request.songs_info));

  Pump();

  return job->info.id;

}

void OrganizeQueue::WatchDestination(const SharedPtr<MusicStorage> &destination) {

  if (!destination || watched_destinations_.contains(destination.get())) return;

  // Jobs waiting for a device which is disconnected can never run, cancel them.
  SharedPtr<ConnectedDevice> device = std::dynamic_pointer_cast<ConnectedDevice>(destination);
  if (!device) return;

  const MusicStorage *storage = destination.get();
  watched_destinations_ << storage;
  QObject::connect(&*device, &ConnectedDevice::DeviceCloseFinished, this, [this, storage]() { DestinationClosed(storage); });
  QObject::connect(&*device, &QObject::destroyed, this, [this, storage]() { DestinationClosed(storage); watched_destinations_.removeAll(storage); });

}

void OrganizeQueue::DestinationClosed(const MusicStorage *destination) {

  eject_requested_.removeAll(destination);

  const QList<Job*> jobs = jobs_;
  for (Job *job : jobs) {
    if (job->request.destination.get() != destination) continue;
    if (job->info.state == JobState::Queued || job->info.state == JobState::Preparing) {
      qLog(Warning) << "Cancelling organize job" << job->info.id << "since" << job->info.destination_label << "was disconnected";
      if (job->info.id == preparing_id_ && prepare_cancel_) prepare_cancel_->store(true);
      FinishJob(job, JobState::Cancelled, QStringList(), QStringList() << tr("%1 was disconnected before the job could start.").arg(job->info.destination_label), false);
    }
  }

}

void OrganizeQueue::Cancel(const quint64 id) {

  Job *job = FindJob(id);
  if (!job || IsFinalState(job->info.state)) return;

  qLog(Info) << "Cancelling organize job" << id << "in state" << StateName(job->info.state);
  job->cancel_requested = true;

  switch (job->info.state) {
    case JobState::Preparing:
      if (id == preparing_id_ && prepare_cancel_) {
        // PrepareFinished() ends the job when the merger stopped.
        prepare_cancel_->store(true);
        job->info.detail = tr("Cancelling");
        Q_EMIT JobChanged(id);
      }
      else {
        FinishJob(job, JobState::Cancelled, QStringList(), QStringList(), false);
      }
      break;
    case JobState::Queued:
      FinishJob(job, JobState::Cancelled, QStringList(), QStringList(), false);
      break;
    case JobState::Running:
      // Organize stops after the song in flight, writes the device database and then emits Finished, which ends the job.
      if (job->organize) job->organize->Cancel();
      job->info.detail = tr("Cancelling after the current song");
      Q_EMIT JobChanged(id);
      break;
    default:
      break;
  }

  Pump();

}

void OrganizeQueue::CancelAll() {

  const QList<Job*> jobs = jobs_;
  for (const Job *job : jobs) {
    Cancel(job->info.id);
  }

}

void OrganizeQueue::ClearFinished() {

  const QList<Job*> jobs = jobs_;
  for (Job *job : jobs) {
    if (!IsFinalState(job->info.state)) continue;
    const quint64 id = job->info.id;
    jobs_.removeAll(job);
    delete job;
    Q_EMIT JobRemoved(id);
  }

}

void OrganizeQueue::Pump() {

  // One preparing job at a time, oldest first.
  if (preparing_id_ == 0) {
    for (Job *job : std::as_const(jobs_)) {
      if (job->info.state == JobState::Preparing) {
        StartPreparing(job);
        break;
      }
    }
  }

  // The oldest queued job of each destination which isn't busy.
  // Preparing jobs don't block the jobs queued after them, their place in the queue is kept by the order of jobs_ though, so a job which finished preparing goes before jobs queued later.
  QSet<const MusicStorage*> busy_destinations;
  for (const Job *job : std::as_const(jobs_)) {
    if (job->info.state == JobState::Running) busy_destinations << job->request.destination.get();
  }

  const QList<Job*> jobs = jobs_;
  for (Job *job : jobs) {
    if (job->info.state != JobState::Queued) continue;
    const MusicStorage *destination = job->request.destination.get();
    if (busy_destinations.contains(destination)) continue;
    busy_destinations << destination;
    StartJob(job);
  }

}

void OrganizeQueue::StartPreparing(Job *job) {

  preparing_id_ = job->info.id;
  prepare_cancel_ = make_shared<std::atomic<bool>>(false);
  job->info.detail = tr("Combining audiobook chapters");
  Q_EMIT JobChanged(job->info.id);

  const quint64 id = job->info.id;
  const Organize::NewSongInfoList songs_info = job->request.songs_info;
  const OrganizeFormat format = job->request.format;
  const SharedPtr<TagReaderClient> tagreader_client = job->request.tagreader_client;
  const SharedPtr<std::atomic<bool>> cancel = prepare_cancel_;
  QPointer<OrganizeQueue> self(this);

  // The merger runs ffmpeg synchronously, so it runs on a worker thread, this way copying other jobs and the UI continue while books are combined.
  QFuture<AudiobookMerger::Result> future = QtConcurrent::run([self, id, songs_info, format, tagreader_client, cancel]() {
    AudiobookMerger merger;
    merger.SetTagReaderClient(tagreader_client);
    merger.SetCancelCallback([cancel]() { return cancel->load(); });
    merger.SetProgressCallback([self, id](const int done, const int total, const QString &book_title) {
      QMetaObject::invokeMethod(self, [self, id, done, total, book_title]() {
        if (!self) return;
        Job *job = self->FindJob(id);
        if (!job || job->info.state != JobState::Preparing || job->cancel_requested) return;
        job->info.detail = tr("Combining \"%1\" (%2 of %3)").arg(book_title, QString::number(qMin(done + 1, total)), QString::number(total));
        Q_EMIT self->JobChanged(id);
      }, Qt::QueuedConnection);
    });
    return merger.MergeByBook(songs_info, format);
  });

  QFutureWatcher<AudiobookMerger::Result> *watcher = new QFutureWatcher<AudiobookMerger::Result>(this);
  QObject::connect(watcher, &QFutureWatcher<AudiobookMerger::Result>::finished, this, [this, watcher, id]() {
    const AudiobookMerger::Result result = watcher->result();
    watcher->deleteLater();
    Job *job = FindJob(id);
    if (job && job->info.state == JobState::Preparing) {
      if (!result.cancelled && !job->cancel_requested) {
        job->prepare_errors = result.errors;
        if (!result.songs_info.isEmpty()) {
          // Replace the per-chapter selection with one merged book per entry.
          job->request.songs_info = result.songs_info;
          job->audiobook_chapters = result.chapters_by_file;
          job->info.total = static_cast<int>(result.songs_info.count());
          job->info.title = tr("%n audiobook(s)", nullptr, job->info.total);
          Q_EMIT JobSongsChanged(id, SongsOf(result.songs_info));
        }
        else {
          job->request.songs_info.clear();
        }
      }
    }
    PrepareFinished(id);
  });
  watcher->setFuture(future);

}

void OrganizeQueue::PrepareFinished(const quint64 id) {

  if (preparing_id_ == id) {
    preparing_id_ = 0;
    prepare_cancel_.reset();
  }

  Job *job = FindJob(id);
  if (job && job->info.state == JobState::Preparing) {
    if (job->cancel_requested) {
      FinishJob(job, JobState::Cancelled, QStringList(), QStringList(), false);
    }
    else if (job->request.songs_info.isEmpty()) {
      // Never sync a half-merged book, when no book could be combined there's nothing left to copy.
      FinishJob(job, JobState::Failed, QStringList(), QStringList() << tr("None of the selected audiobooks could be combined.") << job->prepare_errors, false);
    }
    else {
      job->info.state = JobState::Queued;
      job->info.detail.clear();
      Q_EMIT JobChanged(id);
    }
  }

  Pump();

}

void OrganizeQueue::StartJob(Job *job) {

  const JobRequest &request = job->request;
  const quint64 id = job->info.id;

  // eject_after is handled by the queue, see MaybeEject().
  Organize *organize = new Organize(request.task_manager, request.tagreader_client, request.destination, request.format, request.copy, request.overwrite, request.albumcover, request.songs_info, request.original_roots, false, request.media_type, request.podcast_info, request.playlist, job->audiobook_chapters);
  job->organize = organize;
  job->info.state = JobState::Running;
  job->info.detail.clear();

  // Organize emits from its worker thread, these are queued to this thread.
  // The lambdas capture the id, not the job or the Organize, since Organize deletes itself when its thread finished, and the job can be gone too.
  QObject::connect(organize, &Organize::SongSyncStarted, this, [this, id](const Song &song) { Q_EMIT SongStarted(id, song); });
  QObject::connect(organize, &Organize::SongSyncProgress, this, [this, id](const Song &song, const bool success) {
    if (Job *j = FindJob(id)) {
      if (success) ++j->info.done; else ++j->info.failed;
      Q_EMIT JobChanged(id);
    }
    Q_EMIT SongFinished(id, song, success);
  });
  QObject::connect(organize, &Organize::SongSyncRetry, this, [this, id](const Song &song, const int attempt, const int max_attempts, const qint64 delay_ms) { Q_EMIT SongRetry(id, song, attempt, max_attempts, delay_ms); });
  QObject::connect(organize, &Organize::Finished, this, [this, id](const QStringList &files_with_errors, const QStringList &log, const bool success) { JobDone(id, files_with_errors, log, success); });
  if (request.collection_backend) {
    QObject::connect(organize, &Organize::SongPathChanged, &*request.collection_backend, &CollectionBackend::SongPathChanged);
  }

  qLog(Info) << "Starting organize job" << id << job->info.title << "to" << job->info.destination_label;

  Q_EMIT JobChanged(id);

  organize->Start();

}

void OrganizeQueue::JobDone(const quint64 id, const QStringList &files_with_errors, const QStringList &log, const bool success) {

  Job *job = FindJob(id);
  if (!job) return;

  job->organize.clear();

  JobState state = JobState::Finished;
  if (job->cancel_requested) {
    state = JobState::Cancelled;
  }
  else if (!success || !files_with_errors.isEmpty() || !job->prepare_errors.isEmpty()) {
    state = JobState::Failed;
  }

  QStringList all_log = log;
  all_log << job->prepare_errors;
  FinishJob(job, state, files_with_errors, all_log, success && job->prepare_errors.isEmpty());

}

void OrganizeQueue::FinishJob(Job *job, const JobState state, const QStringList &files_with_errors, const QStringList &log, const bool success) {

  const quint64 id = job->info.id;
  job->info.state = state;
  job->info.detail.clear();

  qLog(Info) << "Organize job" << id << job->info.title << "ended:" << StateName(state) << "-" << job->info.done << "done," << job->info.failed << "failed of" << job->info.total;

  const SharedPtr<MusicStorage> destination = job->request.destination;

  // Don't keep the device (and its database) or the songs alive for a job which only stays in the list for showing its result.
  job->request.destination.reset();
  job->request.songs_info.clear();
  job->audiobook_chapters.clear();

  Q_EMIT JobChanged(id);
  Q_EMIT JobFinished(id, files_with_errors, log, success);

  MaybeEject(destination);

  Pump();

}

void OrganizeQueue::MaybeEject(const SharedPtr<MusicStorage> &destination) {

  if (!destination || !eject_requested_.contains(destination.get())) return;

  for (const Job *job : std::as_const(jobs_)) {
    if (job->request.destination.get() == destination.get() && !IsFinalState(job->info.state)) return;
  }

  eject_requested_.removeAll(destination.get());
  qLog(Info) << "Ejecting device after the last organize job";
  destination->Eject();

}
