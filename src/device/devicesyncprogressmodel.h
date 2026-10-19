/*
 * Strawberry Music Player
 * Copyright 2026, the Strawberry contributors
 *
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
 */

#ifndef DEVICESYNCPROGRESSMODEL_H
#define DEVICESYNCPROGRESSMODEL_H

#include "config.h"

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QPointer>
#include <QString>
#include <QVariant>

#include "core/song.h"
#include "organize/organizequeue.h"

// List model of the sync queue shown in the pane at the bottom of the Devices sidebar.
//
// Every OrganizeQueue job is one header row (title, destination, state, done / total) followed by one row per song.
// Each song row picks up a green check once the song is durably committed to the device, a red cross when it failed after all retries, and an amber "retry N/M in Xs" while it waits between attempts.
// Lives on the main thread, owned by DeviceManager, and only reads from the queue.
class DeviceSyncProgressModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum class Status {
    Pending,     // queued but not yet attempted
    InProgress,  // currently being transcoded/copied
    Retrying,    // last attempt failed; waiting for the next attempt
    Done,        // CopyToStorage + CommitCopy succeeded
    Failed,      // CopyToStorage or CommitCopy reported an error after all retries
    Skipped,     // the job was cancelled before this song was copied
  };

  enum Role {
    Role_Status = Qt::UserRole + 1,
    Role_IsJob,
    Role_JobId,
    Role_JobState,
  };

  explicit DeviceSyncProgressModel(OrganizeQueue *queue, QObject *parent = nullptr);

  OrganizeQueue *queue() const { return queue_; }

  // QAbstractListModel
  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

  // One line summarising the whole queue, for the pane header.
  QString Summary() const;

 Q_SIGNALS:
  void SummaryChanged();
  // A song row changed state, the view keeps it visible.
  void SongRowChanged(const QModelIndex &idx);

 private:
  struct SongRow {
    QString title;
    QString url;
    Status status = Status::Pending;
    // Retry annotation, attempt is 0 when no retry is in flight.
    int retry_attempt = 0;
    int retry_max = 0;
    qint64 retry_delay_ms = 0;
  };

  struct JobBlock {
    OrganizeQueue::JobInfo info;
    QList<SongRow> songs;
  };

  static QList<SongRow> SongRows(const SongList &songs);
  static QString SongTitle(const Song &song);

  int JobPosition(const quint64 id) const;
  int JobFirstRow(const int position) const;
  // Finds the job and song for a flat row, song is -1 for the job header row.
  bool Locate(const int row, int *position, int *song) const;
  // The first song row of the job for this song which isn't final yet.
  int FindSong(const JobBlock &block, const Song &song) const;
  void SongChanged(const int position, const int song);

  void JobAdded(const quint64 id, const SongList &songs);
  void JobSongsChanged(const quint64 id, const SongList &songs);
  void JobChanged(const quint64 id);
  void JobRemoved(const quint64 id);
  void SongStarted(const quint64 id, const Song &song);
  void SongFinished(const quint64 id, const Song &song, const bool success);
  void SongRetry(const quint64 id, const Song &song, const int attempt, const int max_attempts, const qint64 delay_ms);

  QIcon JobIcon(const OrganizeQueue::JobState state) const;
  QIcon SongIcon(const Status status) const;

 private:
  QPointer<OrganizeQueue> queue_;
  QList<JobBlock> jobs_;

  QIcon icon_pending_;
  QIcon icon_active_;
  QIcon icon_retrying_;
  QIcon icon_done_;
  QIcon icon_failed_;
  QIcon icon_skipped_;
};

#endif  // DEVICESYNCPROGRESSMODEL_H
