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
#include <QIcon>
#include <QList>
#include <QString>
#include <QVariant>

#include "core/song.h"

// A simple list-model that tracks the per-song progress of a "Copy to device"
// (i.e. Organize) sync. It exists so the device-sync-progress pane in the
// sidebar can show a green check next to each song that has been durably
// committed to the device, and the user can tell at a glance how far through
// the queue the sync got before any failure. Lives on the main thread and is
// owned by DeviceManager.
class DeviceSyncProgressModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum class Status {
    Pending,    // queued but not yet attempted
    InProgress, // currently being transcoded/copied
    Done,       // CopyToStorage + CommitCopy succeeded
    Failed,     // CopyToStorage or CommitCopy reported an error
  };

  enum Role {
    Role_Status = Qt::UserRole + 1,
  };

  explicit DeviceSyncProgressModel(QObject *parent = nullptr);

  // QAbstractListModel
  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

  // Public API used by the OrganizeDialog wiring.

  // Called once when a sync starts. Replaces any existing queue. label_prefix
  // is shown as a fake first row separator if non-empty (we currently use
  // device friendly name like "iPod Classic"). Each Song's title (falling
  // back to basefilename) becomes one row marked Pending.
  void StartSync(const QString &device_label, const SongList &queue);

  // Called after each song finishes (success or failure) in the same order
  // as the queue passed to StartSync. song is used purely as a sanity check
  // against the row title; if it does not match we still advance.
  void SongFinished(const Song &song, const bool success);

  // Called when the entire sync ends. Does not clear the rows -- the user
  // typically wants to scroll back and see which songs failed -- but does
  // emit syncFinished() so the view can de-emphasise the header.
  void EndSync();

  // True iff there is a sync in progress (between StartSync and EndSync).
  bool is_syncing() const { return syncing_; }

  // Friendly name of the device being synced (empty if no sync in flight).
  QString device_label() const { return device_label_; }

  // Process-wide singleton accessor. DeviceManager registers itself in its
  // constructor; OrganizeDialog reads it on accept() so all four OrganizeDialog
  // call sites (mainwindow / collectionview / fileview / playlistlistcontainer
  // / deviceview) get device-sync-pane progress for free, without having to
  // be plumbed with DeviceManager. Returns nullptr if no DeviceManager has
  // been constructed yet, which is correct for unit tests and headless tools.
  static DeviceSyncProgressModel *instance();
  static void SetInstance(DeviceSyncProgressModel *model);

 Q_SIGNALS:
  void SyncStarted(const QString &device_label, const int total);
  void SongStatusChanged(const int row, const Status status);
  void SyncFinished(const int done, const int failed, const int total);

 private:
  struct Row {
    QString title;
    Status status;
  };

  QList<Row> rows_;
  QIcon icon_pending_;
  QIcon icon_done_;
  QIcon icon_failed_;
  QString device_label_;
  bool syncing_;
  int next_song_index_;
  int done_count_;
  int failed_count_;
};

#endif  // DEVICESYNCPROGRESSMODEL_H