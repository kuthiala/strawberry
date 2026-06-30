/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2012, David Sansome <me@davidsansome.com>
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

#include "config.h"

#include <QLayout>
#include <QList>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

#include "deviceview.h"
#include "deviceviewcontainer.h"
#include "devicesyncprogressmodel.h"
#include "devicesyncprogressview.h"
#include "ui_deviceviewcontainer.h"

DeviceViewContainer::DeviceViewContainer(QWidget *parent)
    : QWidget(parent),
      ui_(new Ui_DeviceViewContainer),
      splitter_(nullptr),
      progress_view_(nullptr) {

  ui_->setupUi(this);

  // The .ui file gave us a QVBoxLayout with `view` as its only child. To
  // split the device pane top/bottom we replace that single-child layout with
  // a QSplitter whose top is the original DeviceView and whose bottom is the
  // new DeviceSyncProgressView. Doing this here (rather than rewriting the
  // .ui file) keeps the .ui generator output stable and makes the change
  // reviewable as pure C++ -- no Qt Designer round-trip required.
  QLayout *old_layout = layout();
  if (old_layout) {
    old_layout->removeWidget(ui_->view);
  }

  splitter_ = new QSplitter(Qt::Vertical, this);
  splitter_->setChildrenCollapsible(false);
  splitter_->addWidget(ui_->view);

  progress_view_ = new DeviceSyncProgressView(this);
  splitter_->addWidget(progress_view_);

  // Default stretch: device tree gets most of the room, progress pane gets a
  // visible-but-compact slice. The user can drag the splitter to taste; Qt
  // persists the split via QSplitter::saveState if the embedding code wants
  // to preserve it (MainWindow doesn't currently, which is fine -- 70/30 is
  // a sensible default).
  splitter_->setStretchFactor(0, 7);
  splitter_->setStretchFactor(1, 3);

  // Replace the now-empty layout with one that hosts the splitter. Re-using
  // the same QVBoxLayout we got from the .ui keeps any margin/spacing
  // settings the designer applied, while letting us add the splitter as the
  // sole child.
  if (old_layout) {
    old_layout->addWidget(splitter_);
  }
  else {
    auto *new_layout = new QVBoxLayout(this);
    new_layout->setContentsMargins(0, 0, 0, 0);
    new_layout->setSpacing(0);
    new_layout->addWidget(splitter_);
  }

}

DeviceViewContainer::~DeviceViewContainer() { delete ui_; }

DeviceView *DeviceViewContainer::view() const { return ui_->view; }

void DeviceViewContainer::SetSyncProgressModel(DeviceSyncProgressModel *model) {
  if (progress_view_) progress_view_->SetModel(model);
}