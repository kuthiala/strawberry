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
class QModelIndex;
class QPoint;
class QToolButton;

// The sync queue pane at the bottom of the Devices sidebar.
// Shows every queued, running and finished "Copy to device" job with its songs, and lets the user cancel a job or clear the finished ones.
class DeviceSyncProgressView : public QWidget {
  Q_OBJECT

 public:
  explicit DeviceSyncProgressView(QWidget *parent = nullptr);

  // Plug a model in. Safe to call repeatedly; the previous model is disconnected. The model is owned by the caller (DeviceManager).
  void SetModel(DeviceSyncProgressModel *model);

 private Q_SLOTS:
  void UpdateHeader();
  void UpdateButtons();
  void SongRowChanged(const QModelIndex &idx);
  void ShowContextMenu(const QPoint &pos);
  void CancelSelectedJob();
  void ClearFinished();

 private:
  quint64 SelectedJobId(bool *active) const;

 private:
  DeviceSyncProgressModel *model_;
  QLabel *header_;
  QToolButton *button_cancel_;
  QToolButton *button_clear_;
  QListView *list_view_;
};

#endif  // DEVICESYNCPROGRESSVIEW_H
