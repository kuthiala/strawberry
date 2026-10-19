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

#include <QAction>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QObject>
#include <QSizePolicy>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "organize/organizequeue.h"
#include "devicesyncprogressmodel.h"
#include "devicesyncprogressview.h"

using namespace Qt::Literals::StringLiterals;

DeviceSyncProgressView::DeviceSyncProgressView(QWidget *parent)
    : QWidget(parent),
      model_(nullptr),
      header_(new QLabel(this)),
      button_cancel_(new QToolButton(this)),
      button_clear_(new QToolButton(this)),
      list_view_(new QListView(this)) {

  setObjectName(u"DeviceSyncProgressView"_s);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 8, 10, 8);
  layout->setSpacing(6);

  QLabel *title = new QLabel(tr("Sync queue"), this);
  title->setObjectName(u"DeviceSyncProgressTitle"_s);

  button_cancel_->setText(tr("Cancel job"));
  button_cancel_->setToolTip(tr("Cancel the selected job. A job which is copying stops after the current song, the songs copied so far are kept."));
  button_cancel_->setAutoRaise(true);
  button_clear_->setText(tr("Clear finished"));
  button_clear_->setAutoRaise(true);

  QHBoxLayout *title_layout = new QHBoxLayout;
  title_layout->setContentsMargins(0, 0, 0, 0);
  title_layout->addWidget(title);
  title_layout->addStretch();
  title_layout->addWidget(button_cancel_);
  title_layout->addWidget(button_clear_);
  layout->addLayout(title_layout);

  header_->setObjectName(u"DeviceSyncProgressHeader"_s);
  header_->setWordWrap(true);
  layout->addWidget(header_);

  list_view_->setObjectName(u"DeviceSyncProgressList"_s);
  list_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_view_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_view_->setTextElideMode(Qt::ElideMiddle);
  // Job rows are two lines and song rows one, so the items aren't uniform.
  list_view_->setUniformItemSizes(false);
  list_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  list_view_->setContextMenuPolicy(Qt::CustomContextMenu);
  layout->addWidget(list_view_, 1);

  QObject::connect(button_cancel_, &QToolButton::clicked, this, &DeviceSyncProgressView::CancelSelectedJob);
  QObject::connect(button_clear_, &QToolButton::clicked, this, &DeviceSyncProgressView::ClearFinished);
  QObject::connect(list_view_, &QListView::customContextMenuRequested, this, &DeviceSyncProgressView::ShowContextMenu);

  UpdateHeader();
  UpdateButtons();

}

void DeviceSyncProgressView::SetModel(DeviceSyncProgressModel *model) {

  if (model_) {
    QObject::disconnect(model_, nullptr, this, nullptr);
  }

  model_ = model;
  list_view_->setModel(model_);

  if (model_) {
    QObject::connect(model_, &DeviceSyncProgressModel::SummaryChanged, this, &DeviceSyncProgressView::UpdateHeader);
    QObject::connect(model_, &DeviceSyncProgressModel::SummaryChanged, this, &DeviceSyncProgressView::UpdateButtons);
    QObject::connect(model_, &DeviceSyncProgressModel::SongRowChanged, this, &DeviceSyncProgressView::SongRowChanged);
    QObject::connect(list_view_->selectionModel(), &QItemSelectionModel::currentChanged, this, &DeviceSyncProgressView::UpdateButtons);
  }

  UpdateHeader();
  UpdateButtons();

}

void DeviceSyncProgressView::UpdateHeader() {

  header_->setText(model_ ? model_->Summary() : tr("No device syncs."));

}

quint64 DeviceSyncProgressView::SelectedJobId(bool *active) const {

  *active = false;
  if (!model_) return 0;

  const QModelIndex idx = list_view_->currentIndex();
  if (!idx.isValid()) return 0;

  *active = !OrganizeQueue::IsFinalState(static_cast<OrganizeQueue::JobState>(idx.data(DeviceSyncProgressModel::Role_JobState).toInt()));
  return idx.data(DeviceSyncProgressModel::Role_JobId).toULongLong();

}

void DeviceSyncProgressView::UpdateButtons() {

  bool active = false;
  SelectedJobId(&active);
  button_cancel_->setEnabled(active);

  bool any_finished = false;
  if (model_ && model_->queue()) {
    const QList<OrganizeQueue::JobInfo> jobs = model_->queue()->jobs();
    for (const OrganizeQueue::JobInfo &job : jobs) {
      if (OrganizeQueue::IsFinalState(job.state)) {
        any_finished = true;
        break;
      }
    }
  }
  button_clear_->setEnabled(any_finished);

}

void DeviceSyncProgressView::SongRowChanged(const QModelIndex &idx) {

  // Keep the song being copied visible, unless the user is looking at something else.
  if (!list_view_->currentIndex().isValid() && idx.isValid()) {
    list_view_->scrollTo(idx, QAbstractItemView::EnsureVisible);
  }

}

void DeviceSyncProgressView::ShowContextMenu(const QPoint &pos) {

  const QModelIndex idx = list_view_->indexAt(pos);
  if (idx.isValid()) list_view_->setCurrentIndex(idx);

  bool active = false;
  SelectedJobId(&active);

  QMenu menu(this);
  QAction *action_cancel = menu.addAction(tr("Cancel job"), this, &DeviceSyncProgressView::CancelSelectedJob);
  action_cancel->setEnabled(active);
  QAction *action_clear = menu.addAction(tr("Clear finished jobs"), this, &DeviceSyncProgressView::ClearFinished);
  action_clear->setEnabled(button_clear_->isEnabled());
  menu.exec(list_view_->viewport()->mapToGlobal(pos));

}

void DeviceSyncProgressView::CancelSelectedJob() {

  bool active = false;
  const quint64 id = SelectedJobId(&active);
  if (id == 0 || !active || !model_ || !model_->queue()) return;

  model_->queue()->Cancel(id);

}

void DeviceSyncProgressView::ClearFinished() {

  if (!model_ || !model_->queue()) return;

  list_view_->setCurrentIndex(QModelIndex());
  model_->queue()->ClearFinished();
  UpdateButtons();

}
