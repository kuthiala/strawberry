/*
 * Strawberry Music Player
 * Copyright 2025, Strawberry contributors
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

#include "podcastdetailsdialog.h"
#include "ui_podcastdetailsdialog.h"

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QShowEvent>

PodcastDetailsDialog::PodcastDetailsDialog(QWidget *parent)
    : QDialog(parent), ui_(new Ui_PodcastDetailsDialog) {

  ui_->setupUi(this);

  QObject::connect(ui_->show_title, &QLineEdit::textChanged, this, &PodcastDetailsDialog::ValidateFields);
  QObject::connect(ui_->feed_url, &QLineEdit::textChanged, this, &PodcastDetailsDialog::ValidateFields);

  ValidateFields();

}

PodcastDetailsDialog::~PodcastDetailsDialog() { delete ui_; }

void PodcastDetailsDialog::SetInitialValues(const QString &show_title, const QString &author) {

  ui_->show_title->setText(show_title);
  ui_->author->setText(author);

}

void PodcastDetailsDialog::showEvent(QShowEvent *e) {

  if (!e->spontaneous()) {
    ui_->show_title->setFocus();
    ui_->show_title->selectAll();
  }

  QDialog::showEvent(e);

}

void PodcastDetailsDialog::ValidateFields() {

  // Require at least a show title OR a feed URL so we always have a stable
  // grouping identity for the episodes.
  const bool valid = !ui_->show_title->text().trimmed().isEmpty() || !ui_->feed_url->text().trimmed().isEmpty();
  ui_->button_box->button(QDialogButtonBox::Ok)->setEnabled(valid);

}

MusicStorage::PodcastInfo PodcastDetailsDialog::info() const {

  MusicStorage::PodcastInfo info;
  info.show_title_ = ui_->show_title->text().trimmed();
  info.author_ = ui_->author->text().trimmed();
  info.feed_url_ = ui_->feed_url->text().trimmed();
  info.description_ = ui_->description->toPlainText().trimmed();
  return info;

}
