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

#ifndef DEVICESYNCPROGRESSVIEW_H
#define DEVICESYNCPROGRESSVIEW_H

#include "config.h"

#include <QObject>
#include <QString>
#include <QWidget>

#include "devicesyncprogressmodel.h"

class QLabel;
class QListView;

// A small widget that lives at the bottom of the Devices sidebar pane and
// shows the per-song progress of a "Copy to device" sync. The pane is empty
// (just a "No active sync" placeholder) when nothing is syncing. When a sync
// starts the list fills with the song titles in the order they will be
// processed, and each row picks up a green check (or red cross) as the
// per-song unit of work commits to the device.
class DeviceSyncProgressView : public QWidget {
  Q_OBJECT

 public:
  explicit DeviceSyncProgressView(QWidget *parent = nullptr);

  // Plug a model in. Safe to call repeatedly; the previous model is
  // disconnected. The model is owned by the caller (DeviceManager).
  void SetModel(DeviceSyncProgressModel *model);

 private Q_SLOTS:
  void HandleSyncStarted(const QString &device_label, const int total);
  void HandleSongStatusChanged(const int row, const DeviceSyncProgressModel::Status status);
  void HandleSyncFinished(const int done, const int failed, const int total);

 private:
  void UpdateHeader(const QString &device_label, const int done, const int failed, const int total, const bool active);

 private:
  DeviceSyncProgressModel *model_;
  QLabel *header_;
  QListView *list_view_;

  int total_;
  int done_;
  int failed_;
  QString device_label_;
};

#endif  // DEVICESYNCPROGRESSVIEW_H