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

#include "config.h"

#include <QFrame>
#include <QLabel>
#include <QListView>
#include <QMargins>
#include <QObject>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "devicesyncprogressmodel.h"
#include "devicesyncprogressview.h"

DeviceSyncProgressView::DeviceSyncProgressView(QWidget *parent)
    : QWidget(parent),
      model_(nullptr),
      header_(new QLabel(this)),
      list_view_(new QListView(this)),
      total_(0),
      done_(0),
      failed_(0) {

  setObjectName(QStringLiteral("DeviceSyncProgressView"));

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 6, 8, 6);
  layout->setSpacing(4);

  // Subtle separator line at the top so the pane visually splits from the
  // device tree above when the user has the splitter handle dragged tight.
  auto *separator = new QFrame(this);
  separator->setFrameShape(QFrame::HLine);
  separator->setFrameShadow(QFrame::Sunken);
  layout->addWidget(separator);

  header_->setObjectName(QStringLiteral("DeviceSyncProgressHeader"));
  header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  header_->setWordWrap(true);
  layout->addWidget(header_);

  list_view_->setObjectName(QStringLiteral("DeviceSyncProgressList"));
  // Read-only: the user is observing progress, not editing.
  list_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_view_->setSelectionMode(QAbstractItemView::NoSelection);
  list_view_->setFocusPolicy(Qt::NoFocus);
  list_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_view_->setUniformItemSizes(true);
  list_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  layout->addWidget(list_view_, /*stretch=*/1);

  UpdateHeader(QString(), 0, 0, 0, /*active=*/false);

}

void DeviceSyncProgressView::SetModel(DeviceSyncProgressModel *model) {

  if (model_) {
    QObject::disconnect(model_, nullptr, this, nullptr);
  }

  model_ = model;
  list_view_->setModel(model_);

  if (model_) {
    QObject::connect(model_, &DeviceSyncProgressModel::SyncStarted, this, &DeviceSyncProgressView::HandleSyncStarted);
    QObject::connect(model_, &DeviceSyncProgressModel::SongStatusChanged, this, &DeviceSyncProgressView::HandleSongStatusChanged);
    QObject::connect(model_, &DeviceSyncProgressModel::SyncFinished, this, &DeviceSyncProgressView::HandleSyncFinished);
    // Re-seed header in case the model already has rows from a previous sync.
    UpdateHeader(model_->device_label(), /*done=*/0, /*failed=*/0, model_->rowCount(), model_->is_syncing());
  }
  else {
    UpdateHeader(QString(), 0, 0, 0, false);
  }

}

void DeviceSyncProgressView::HandleSyncStarted(const QString &device_label, const int total) {

  device_label_ = device_label;
  total_ = total;
  done_ = 0;
  failed_ = 0;
  UpdateHeader(device_label_, done_, failed_, total_, /*active=*/true);
  // Scroll to top so the first song is visible when sync kicks off.
  if (model_ && model_->rowCount() > 0) {
    list_view_->scrollToTop();
  }

}

void DeviceSyncProgressView::HandleSongStatusChanged(const int row, const DeviceSyncProgressModel::Status status) {

  switch (status) {
    case DeviceSyncProgressModel::Status::Done:   ++done_; break;
    case DeviceSyncProgressModel::Status::Failed: ++failed_; break;
    default: break;
  }
  UpdateHeader(device_label_, done_, failed_, total_, /*active=*/true);
  // Keep the most recently finished song visible without ripping the user's
  // scroll position around if they've manually scrolled away.
  if (model_) {
    const QModelIndex idx = model_->index(row);
    if (idx.isValid()) list_view_->scrollTo(idx, QAbstractItemView::EnsureVisible);
  }

}

void DeviceSyncProgressView::HandleSyncFinished(const int done, const int failed, const int total) {

  done_ = done;
  failed_ = failed;
  total_ = total;
  UpdateHeader(device_label_, done_, failed_, total_, /*active=*/false);

}

void DeviceSyncProgressView::UpdateHeader(const QString &device_label, const int done, const int failed, const int total, const bool active) {

  QString text;
  if (total == 0 && !active) {
    text = tr("No active device sync.");
  }
  else {
    const QString label = device_label.isEmpty() ? tr("Device") : device_label;
    if (active) {
      text = tr("Syncing to %1 — %2 / %3 done").arg(label).arg(done + failed).arg(total);
    }
    else {
      text = tr("Sync to %1 finished — %2 ok, %3 failed").arg(label).arg(done).arg(failed);
    }
  }
  header_->setText(text);

}