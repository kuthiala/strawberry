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

#include <QtGlobal>

#include <functional>
#include <utility>
#include <chrono>

#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QUrl>
#include <QImage>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/taskmanager.h"
#include "core/musicstorage.h"
#include "core/song.h"
#include "utilities/strutils.h"
#include "tagreader/tagreaderclient.h"
#include "organize.h"
#include "transcoder/transcoder.h"

using namespace std::chrono_literals;

class OrganizeFormat;

namespace {
constexpr int kBatchSize = 10;
constexpr int kTranscodeProgressInterval = 500;

// Compute the wall-clock delay between the just-failed attempt and the next
// retry. attempt is 1-based and indicates how many *failed* attempts have
// happened so far. The first failure (attempt == 1) waits kInitialBackoffMs,
// the second waits 2 * kInitialBackoffMs, ... capped at kMaxBackoffMs.
// We use a left-shift instead of std::pow to stay integer-only, and clamp
// at 30 shifts to avoid undefined behaviour from an overflow if someone
// raises kMaxAttempts in the future.
qint64 ComputeBackoffMs(const int attempt) {
  if (attempt <= 0) return 0;
  const int shift = qMin(attempt - 1, 30);
  qint64 delay = static_cast<qint64>(Organize::kInitialBackoffMs) << shift;
  if (delay > Organize::kMaxBackoffMs || delay < 0) {
    delay = Organize::kMaxBackoffMs;
  }
  return delay;
}
}  // namespace

Organize::Organize(const SharedPtr<TaskManager> task_manager,
                   const SharedPtr<TagReaderClient> tagreader_client,
                   const SharedPtr<MusicStorage> destination,
                   const OrganizeFormat &format,
                   const bool copy,
                   const bool overwrite,
                   const bool albumcover,
                   const NewSongInfoList &songs_info,
                   const bool eject_after,
                   const QString &playlist,
                   QObject *parent)

    : QObject(parent),
      thread_(nullptr),
      task_manager_(task_manager),
      tagreader_client_(tagreader_client),
      transcoder_(new Transcoder(this)),
      process_files_timer_(new QTimer(this)),
      destination_(destination),
      format_(format),
      copy_(copy),
      overwrite_(overwrite),
      albumcover_(albumcover),
      eject_after_(eject_after),
      task_count_(static_cast<quint64>(songs_info.count())),
      playlist_(playlist),
      tasks_complete_(0),
      started_(false),
      task_id_(0),
      current_copy_progress_(0),
      finished_(false) {

  original_thread_ = thread();

  process_files_timer_->setSingleShot(true);
  process_files_timer_->setInterval(100ms);
  QObject::connect(process_files_timer_, &QTimer::timeout, this, &Organize::ProcessSomeFiles);

  tasks_pending_.reserve(songs_info.count());
  for (const NewSongInfo &song_info : songs_info) {
    tasks_pending_ << Task(song_info);
  }

}

Organize::~Organize() {

  if (thread_) {
    thread_->quit();
    thread_->deleteLater();
  }

}

void Organize::Start() {

  if (thread_) return;

  task_id_ = task_manager_->StartTask(tr("Organizing files"));
  task_manager_->SetTaskBlocksCollectionScans(task_id_);

  // Publish the ordered queue of songs to anyone listening (the sidebar
  // device-sync-progress pane in particular). Emitted before we move to the
  // worker thread so receivers connected with the default AutoConnection
  // still get the queue synchronously on the main thread.
  SongList queue;
  queue.reserve(tasks_pending_.count());
  for (const Task &task : std::as_const(tasks_pending_)) {
    queue << task.song_info_.song_;
  }
  Q_EMIT SyncQueueReady(queue);

  thread_ = new QThread;
  QObject::connect(thread_, &QThread::started, this, &Organize::ProcessSomeFiles);

  QObject::connect(transcoder_, &Transcoder::JobComplete, this, &Organize::FileTranscoded);
  QObject::connect(transcoder_, &Transcoder::LogLine, this, &Organize::LogLine);

  moveToThread(thread_);
  thread_->start();

}

void Organize::ProcessSomeFiles() {

  if (finished_) return;

  if (!started_) {
    if (!destination_->StartCopy(&supported_filetypes_)) {
      // Failed to start - mark everything as failed :(
      for (const Task &task : std::as_const(tasks_pending_)) {
        files_with_errors_ << task.song_info_.song_.url().toLocalFile();
      }
      tasks_pending_.clear();
    }
    started_ = true;
  }

  // None left?
  if (tasks_pending_.isEmpty()) {
    if (!tasks_transcoding_.isEmpty()) {
      // Just wait - FileTranscoded will start us off again in a little while
      qLog(Debug) << "Waiting for transcoding jobs";
      transcode_progress_timer_.start(kTranscodeProgressInterval, this);
      return;
    }

    UpdateProgress();

    QString error_text;
    if (!destination_->FinishCopy(files_with_errors_.isEmpty(), error_text) && !error_text.isEmpty()) {
      log_ << error_text;
    }
    if (eject_after_) destination_->Eject();

    task_manager_->SetTaskFinished(task_id_);

    Q_EMIT Finished(files_with_errors_, log_);

    // Move back to the original thread so deleteLater() can get called in the main thread's event loop
    moveToThread(original_thread_);
    deleteLater();

    // Stop this thread
    thread_->quit();
    finished_ = true;
    return;
  }

  // We process files in batches so we can be cancelled part-way through.
  //
  // Retry-aware scheduling: previously we only inspected the head of the
  // queue and, if it wasn't due yet, we broke out and waited. That was
  // fine when retries were rare, but it caused a HARD stall when even one
  // song entered the extended-backoff regime — e.g. Bug #13's transcoder
  // filename collision put "05 Life of a Salesman.flac" into a 300 s/
  // attempt backoff loop with 9 attempts, blocking every other queued
  // song for ~13 minutes before the user gave up and killed the app.
  //
  // Fix: scan through the queue and pick the FIRST task whose
  // next_attempt_at_ms_ has elapsed. Skip past not-yet-due retries — they
  // remain in place with their backoff timer intact, and the earliest one
  // controls the timer re-arm below. Ordering guarantee is now
  // "top-eligible-first" rather than strictly "top-first", which lets the
  // sidebar keep progressing while a stuck song is cooling down.
  // See `.ai/10-ipod-sync.md §10.16` for full context.
  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  qint64 earliest_due_ms = 0;
  for (int i = 0; i < kBatchSize; ++i) {
    SetSongProgress(0);

    if (tasks_pending_.isEmpty()) break;

    // Scan for the first eligible task. Not-yet-due tasks contribute to
    // earliest_due_ms so the timer re-arm below wakes us exactly when
    // the earliest retry becomes ready. If ALL tasks are cooling down
    // this scan sets earliest_due_ms and leaves due_idx as -1, which
    // breaks the batch loop cleanly.
    int due_idx = -1;
    for (int j = 0; j < tasks_pending_.size(); ++j) {
      const qint64 next = tasks_pending_[j].next_attempt_at_ms_;
      if (next <= now_ms) {
        due_idx = j;
        break;
      }
      if (earliest_due_ms == 0 || next < earliest_due_ms) {
        earliest_due_ms = next;
      }
    }
    if (due_idx < 0) break;

    Task task = tasks_pending_.takeAt(due_idx);
    if (task.attempts_ > 0) {
      qLog(Info) << "Retrying" << task.song_info_.song_.url().toLocalFile()
                 << "(attempt" << (task.attempts_ + 1) << "of" << kMaxAttempts << ")";
    }
    else {
      qLog(Info) << "Processing" << task.song_info_.song_.url().toLocalFile();
    }

    // Use a Song instead of a tag reader
    Song song = task.song_info_.song_;
    if (!song.is_valid()) {
      tasks_complete_++;  // Otherwise progress never reaches 100%.
      continue;
    }

    // Maybe this file is one that's been transcoded already?
    if (!task.transcoded_filename_.isEmpty()) {
      qLog(Debug) << "This file has already been transcoded";

      // Set the new filetype on the song so the formatter gets it right
      song.set_filetype(task.new_filetype_);

      // Fiddle the filename extension as well to match the new type
      song.set_url(QUrl::fromLocalFile(Utilities::FiddleFileExtension(song.basefilename(), task.new_extension_)));
      song.set_basefilename(Utilities::FiddleFileExtension(song.basefilename(), task.new_extension_));
      task.song_info_.new_filename_ = Utilities::FiddleFileExtension(task.song_info_.new_filename_, task.new_extension_);

      // Have to set this to the size of the new file or else funny stuff happens
      song.set_filesize(QFileInfo(task.transcoded_filename_).size());
    }
    else {
      // Figure out if we need to transcode it
      Song::FileType dest_type = CheckTranscode(song.filetype());
      if (dest_type != Song::FileType::Unknown) {
        // Get the preset
        TranscoderPreset preset = Transcoder::PresetForFileType(dest_type);

        // Check if the destination file already exists and we're not allowed to overwrite
        const QString dest_filename_with_new_ext = Utilities::FiddleFileExtension(task.song_info_.new_filename_, preset.extension_);
        if (ShouldSkipFile(dest_filename_with_new_ext)) {
          qLog(Debug) << "Skipping" << task.song_info_.song_.url().toLocalFile() << ", destination file already exists";
          tasks_complete_++;
          continue;
        }

        qLog(Debug) << "Transcoding with" << preset.name_;

        task.transcoded_filename_ = transcoder_->GetFile(task.song_info_.song_.url().toLocalFile(), preset);
        task.new_extension_ = preset.extension_;
        task.new_filetype_ = dest_type;
        tasks_transcoding_[task.song_info_.song_.url().toLocalFile()] = task;
        qLog(Debug) << "Transcoding to" << task.transcoded_filename_;

        // Start the transcoding - this will happen in the background and FileTranscoded() will get called when it's done.
        // At that point the task will get re-added to the pending queue with the new filename.
        transcoder_->AddJob(task.song_info_.song_.url().toLocalFile(), preset, task.transcoded_filename_);
        transcoder_->Start();
        continue;
      }
    }

    // Check if the destination file already exists and we're not allowed to overwrite
    if (ShouldSkipFile(task.song_info_.new_filename_)) {
      qLog(Debug) << "Skipping" << task.song_info_.song_.url().toLocalFile() << ", destination file already exists";
      tasks_complete_++;
      continue;
    }

    MusicStorage::CopyJob job;
    job.source_ = task.transcoded_filename_.isEmpty() ? task.song_info_.song_.url().toLocalFile() : task.transcoded_filename_;
    job.destination_ = task.song_info_.new_filename_;
    job.metadata_ = song;
    job.overwrite_ = overwrite_;
    job.albumcover_ = albumcover_;
    job.remove_original_ = !copy_;
    job.playlist_ = playlist_;

    // [cover-art trace] Log the inputs to the cover-resolution cascade so we
    // can see in production exactly which branch fires for a transcoded
    // FLAC → ALAC iPod copy. Bug #5: covers vanish on reconnect after a
    // Strawberry "Copy to Device" of a FLAC (my probe against the same iPod
    // worked end-to-end; the difference is this Organize→Transcoder path).
    qLog(Info) << "[cover-trace] Organize: original-url=" << task.song_info_.song_.url().toString()
               << " transcoded=" << !task.transcoded_filename_.isEmpty()
               << " dest=" << job.destination_
               << " albumcover_=" << job.albumcover_
               << " device-source=" << (destination_->source() == Song::Source::Device)
               << " art_manual_valid=" << task.song_info_.song_.art_manual_is_valid()
               << " art_manual_unset=" << task.song_info_.song_.art_unset()
               << " art_manual=" << task.song_info_.song_.art_manual().toString()
               << " art_automatic_valid=" << task.song_info_.song_.art_automatic_is_valid()
               << " art_automatic=" << task.song_info_.song_.art_automatic().toString();

    if (task.song_info_.song_.art_manual_is_valid() && !task.song_info_.song_.art_unset()) {
      if (task.song_info_.song_.art_manual().isLocalFile() && QFile::exists(task.song_info_.song_.art_manual().toLocalFile())) {
        job.cover_source_ = task.song_info_.song_.art_manual().toLocalFile();
        qLog(Info) << "[cover-trace] Organize: using art_manual local file" << job.cover_source_;
      }
      else if (task.song_info_.song_.art_manual().scheme().isEmpty() && QFile::exists(task.song_info_.song_.art_manual().path())) {
        job.cover_source_ = task.song_info_.song_.art_manual().path();
        qLog(Info) << "[cover-trace] Organize: using art_manual schemeless path" << job.cover_source_;
      }
      else {
        qLog(Info) << "[cover-trace] Organize: art_manual valid but path not on disk:" << task.song_info_.song_.art_manual().toString();
      }
    }
    else if (task.song_info_.song_.art_automatic_is_valid()) {
      if (task.song_info_.song_.art_automatic().isLocalFile() && QFile::exists(task.song_info_.song_.art_automatic().toLocalFile())) {
        job.cover_source_ = task.song_info_.song_.art_automatic().toLocalFile();
        qLog(Info) << "[cover-trace] Organize: using art_automatic local file" << job.cover_source_;
      }
      else if (task.song_info_.song_.art_automatic().scheme().isEmpty() && QFile::exists(task.song_info_.song_.art_automatic().path())) {
        job.cover_source_ = task.song_info_.song_.art_automatic().path();
        qLog(Info) << "[cover-trace] Organize: using art_automatic schemeless path" << job.cover_source_;
      }
      else {
        qLog(Info) << "[cover-trace] Organize: art_automatic valid but path not on disk:" << task.song_info_.song_.art_automatic().toString();
      }
    }
    else if (destination_->source() == Song::Source::Device) {
      const QString embed_src = task.song_info_.song_.url().toLocalFile();
      qLog(Info) << "[cover-trace] Organize: falling through to embedded-art load from" << embed_src
                 << " exists=" << QFile::exists(embed_src);
      const TagReaderResult result = tagreader_client_->LoadCoverImageBlocking(embed_src, job.cover_image_);
      qLog(Info) << "[cover-trace] Organize: LoadCoverImageBlocking result=" << result.success()
                 << " image.isNull=" << job.cover_image_.isNull()
                 << " image.size=" << job.cover_image_.size();
      if (!result.success()) {
        qLog(Error) << "Could not load embedded art from" << task.song_info_.song_.url() << result.error_string();
      }
    }
    else {
      qLog(Info) << "[cover-trace] Organize: no cover-source branch matched (destination is not Device or song has no art metadata)";
    }

    if (!job.cover_source_.isEmpty()) {
      job.cover_dest_ = QFileInfo(job.destination_).path() + QLatin1Char('/') + QFileInfo(job.cover_source_).fileName();
    }

    job.progress_ = std::bind(&Organize::SetSongProgress, this, std::placeholders::_1, !task.transcoded_filename_.isEmpty());

    qLog(Info) << "[cover-trace] Organize: handing job to CopyToStorage"
               << " cover_source_=" << job.cover_source_
               << " cover_image_.isNull=" << job.cover_image_.isNull()
               << " cover_image_.size=" << job.cover_image_.size();

    QString error_text;
    bool song_succeeded = destination_->CopyToStorage(job, error_text);
    if (song_succeeded) {
      // Per-song unit of work: ask the destination to commit the just-copied
      // song durably (e.g. flush iTunesDB on iPods) before we move on to the
      // next one. This is a no-op for plain filesystem storages, and an
      // iTunesDB write on GPodDevice. If the commit fails we treat the
      // just-copied song as a failure so the user knows to retry it.
      QString commit_error;
      if (!destination_->CommitCopy(commit_error)) {
        qLog(Error) << "Per-song commit failed for" << task.song_info_.song_.url().toLocalFile() << ":" << commit_error;
        if (!commit_error.isEmpty()) {
          if (error_text.isEmpty()) error_text = commit_error;
          else error_text += QStringLiteral("; ") + commit_error;
        }
        song_succeeded = false;
      }
    }

    if (song_succeeded) {
      if (job.remove_original_ && song.is_local_collection_song() && destination_->source() == Song::Source::Collection) {
        // Notify other aspects of system that song has been invalidated
        QString root = destination_->LocalPath();
        QFileInfo new_file = QFileInfo(root + QLatin1Char('/') + task.song_info_.new_filename_);
        Q_EMIT SongPathChanged(song, new_file, destination_->collection_directory_id());
      }
      Q_EMIT SongSyncProgress(task.song_info_.song_, true);

      // Clean up the temporary transcoded file and free its Transcoder
      // reservation (Bug #13, see `.ai/10-ipod-sync.md §10.16`) so a
      // subsequent song with the same basename can safely reuse the
      // name. Order matters: remove the file FIRST so ReleaseOutput's
      // successor GetFile call sees the free name via the disk check
      // and doesn't have to fall back to a "-N" suffix.
      if (!task.transcoded_filename_.isEmpty()) {
        QFile::remove(task.transcoded_filename_);
        transcoder_->ReleaseOutput(task.transcoded_filename_);
      }

      tasks_complete_++;
    }
    else {
      // The attempt failed. Either re-queue with exponential backoff (if we
      // have retries left) or report a final failure (if we just exhausted
      // them). Logging the error text on *every* attempt would spam the log,
      // so we only record it in `log_` on the final failure -- but always
      // qLog(Warning) so the developer console shows each retry.
      ++task.attempts_;
      const QString full_path = task.song_info_.song_.url().toLocalFile();
      const QString reported_path = full_path.isEmpty() ? task.song_info_.song_.basefilename() : full_path;

      if (task.attempts_ < kMaxAttempts) {
        const qint64 delay_ms = ComputeBackoffMs(task.attempts_);
        task.next_attempt_at_ms_ = QDateTime::currentMSecsSinceEpoch() + delay_ms;
        qLog(Warning) << "Sync attempt" << task.attempts_ << "of" << kMaxAttempts
                      << "failed for" << reported_path
                      << "-- retrying in" << delay_ms << "ms"
                      << (error_text.isEmpty() ? QString() : (QStringLiteral(": ") + error_text));
        // Re-queue at the *front* so retries don't get bumped to the end of
        // very long queues and the user keeps seeing roughly-sequential
        // checkmarks; the next_attempt_at_ms_ gate prevents busy-spin and
        // makes the queue effectively a min-heap on the retry timestamp.
        tasks_pending_.prepend(task);
        // Re-emit the queue position to listeners (the pane shows e.g.
        // "retry 3/10 in 4s" next to the row). Do NOT update files_with_errors_
        // and do NOT bump tasks_complete_ -- the song is still in flight.
        Q_EMIT SongSyncRetry(task.song_info_.song_, task.attempts_, kMaxAttempts, delay_ms);

        // Track earliest due so we can rearm the timer with an accurate wait.
        if (earliest_due_ms == 0 || task.next_attempt_at_ms_ < earliest_due_ms) {
          earliest_due_ms = task.next_attempt_at_ms_;
        }
        // The transcoded file (if any) is preserved so we don't have to
        // re-encode the same source on every retry. It is cleaned up below
        // on either success or final failure.
        break;  // Stop the batch -- the head of the queue is now in the
                // future and we want the rearm logic below to set the timer
                // to fire when the retry becomes due rather than burning
                // through more no-op iterations.
      }

      // All retries exhausted -- this is a permanent failure for this song.
      qLog(Error) << "Sync giving up on" << reported_path
                  << "after" << kMaxAttempts << "attempts"
                  << (error_text.isEmpty() ? QString() : (QStringLiteral(": ") + error_text));
      files_with_errors_ << reported_path;
      if (!error_text.isEmpty()) {
        log_ << error_text;
      }
      Q_EMIT SongSyncProgress(task.song_info_.song_, false);

      // Clean up the temporary transcoded file and free its Transcoder
      // reservation (Bug #13). Same rationale as the success branch
      // above: the reservation would otherwise leak until the sync ends,
      // blocking other same-basename songs from ever getting a clean
      // path.
      if (!task.transcoded_filename_.isEmpty()) {
        QFile::remove(task.transcoded_filename_);
        transcoder_->ReleaseOutput(task.transcoded_filename_);
      }

      tasks_complete_++;
    }
  }
  SetSongProgress(0);

  if (!process_files_timer_->isActive()) {
    // After the batch, decide when to fire next. Three cases:
    //
    // 1. There are still eligible (non-cooldown) tasks in the queue that
    //    we didn't get to (batch cap hit OR a failure broke us out
    //    early). Fire on the default fast tick — those tasks are ready
    //    right now and should not wait behind a cooling-down retry.
    //
    // 2. All remaining tasks are in retry-cooldown. Fire at the earliest
    //    cooldown expiry (bounded below by 100 ms to avoid a busy loop
    //    and above by kMaxBackoffMs because Qt's int32 timer clamps
    //    beyond that).
    //
    // 3. The queue is empty. Fall through to the default tick — the
    //    next event will land on the "None left?" branch at the top of
    //    ProcessSomeFiles and finalize the batch.
    //
    // The scan is O(N) worst-case per batch, but N is bounded by the
    // caller's queue size (max ~thousands) and the per-tick fixed cost
    // is dominated by the actual disk / device I/O in the batch itself.
    // See `.ai/10-ipod-sync.md §10.16` — the failure mode this closes
    // is a 300 s stall of the entire sync behind one bad song.
    const qint64 now_after_batch = QDateTime::currentMSecsSinceEpoch();
    bool have_more_eligible = false;
    for (const Task &t : std::as_const(tasks_pending_)) {
      if (t.next_attempt_at_ms_ <= now_after_batch) {
        have_more_eligible = true;
        break;
      }
    }

    if (have_more_eligible) {
      process_files_timer_->start();
    }
    else if (earliest_due_ms > 0) {
      const qint64 wait_ms = qMax<qint64>(100, earliest_due_ms - now_after_batch);
      process_files_timer_->start(static_cast<int>(qMin<qint64>(wait_ms, kMaxBackoffMs)));
    }
    else {
      process_files_timer_->start();
    }
  }


}

bool Organize::ShouldSkipFile(const QString &filename) const {

  if (overwrite_) {
    return false;
  }

  return QFile::exists(destination_->LocalPath() + QLatin1Char('/') + filename);

}

Song::FileType Organize::CheckTranscode(const Song::FileType original_type) const {

  if (original_type == Song::FileType::Stream) return Song::FileType::Unknown;

  const MusicStorage::TranscodeMode mode = destination_->GetTranscodeMode();
  const Song::FileType format = destination_->GetTranscodeFormat();

  switch (mode) {
    case MusicStorage::TranscodeMode::Transcode_Never:
      return Song::FileType::Unknown;

    case MusicStorage::TranscodeMode::Transcode_Always:
      if (original_type == format) return Song::FileType::Unknown;
      return format;

    case MusicStorage::TranscodeMode::Transcode_Unsupported:
      if (supported_filetypes_.isEmpty() || supported_filetypes_.contains(original_type)) return Song::FileType::Unknown;

      if (format != Song::FileType::Unknown) return format;

      // The user hasn't visited the device properties page yet to set a preferred format for the device, so we have to pick the best available one.
      return Transcoder::PickBestFormat(supported_filetypes_);
  }
  return Song::FileType::Unknown;

}

void Organize::SetSongProgress(const float progress, const bool transcoded) {

  const int max = transcoded ? 50 : 100;
  current_copy_progress_ = (transcoded ? 50 : 0) + qBound(0, static_cast<int>(progress * static_cast<float>(max)), max - 1);
  UpdateProgress();

}

void Organize::UpdateProgress() {

  const quint64 total = task_count_ * 100;

  // Update transcoding progress
  QMap<QString, float> transcode_progress = transcoder_->GetProgress();
  const QStringList filenames = transcode_progress.keys();
  for (const QString &filename : filenames) {
    if (!tasks_transcoding_.contains(filename)) continue;
    tasks_transcoding_[filename].transcode_progress_ = transcode_progress[filename];
  }

  // Count the progress of all tasks that are in the queue.
  // Files that need transcoding total 50 for the transcode and 50 for the copy, files that only need to be copied total 100.
  int progress = tasks_complete_ * 100;

  for (const Task &task : std::as_const(tasks_pending_)) {
    progress += qBound(0, static_cast<int>(task.transcode_progress_ * 50), 50);
  }

  const QList<Task> tasks_transcoding = tasks_transcoding_.values();
  for (const Task &task : tasks_transcoding) {
    progress += qBound(0, static_cast<int>(task.transcode_progress_ * 50), 50);
  }

  // Add the progress of the track that's currently copying
  progress += current_copy_progress_;

  task_manager_->SetTaskProgress(task_id_, static_cast<quint64>(progress), total);

}

void Organize::FileTranscoded(const QString &input, const QString &output, const bool success) {

  Q_UNUSED(output);

  qLog(Info) << "File finished" << input << success;
  transcode_progress_timer_.stop();

  Task task = tasks_transcoding_.take(input);
  if (!success) {
    files_with_errors_ << input;
  }
  else {
    tasks_pending_ << task;
  }

  if (!process_files_timer_->isActive()) {
    process_files_timer_->start();
  }

}

void Organize::timerEvent(QTimerEvent *e) {

  QObject::timerEvent(e);

  if (e->timerId() == transcode_progress_timer_.timerId()) {
    UpdateProgress();
  }

}

void Organize::LogLine(const QString &message) {

  QString date(QDateTime::currentDateTime().toString(Qt::TextDate));
  log_.append(QStringLiteral("%1: %2").arg(date, message));

}
