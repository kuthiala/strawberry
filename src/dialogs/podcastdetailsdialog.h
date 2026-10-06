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

#ifndef PODCASTDETAILSDIALOG_H
#define PODCASTDETAILSDIALOG_H

#include <QDialog>
#include <QString>

#include "core/musicstorage.h"

class QShowEvent;
class Ui_PodcastDetailsDialog;

// Modal dialog that prompts the user for show-level podcast metadata before a
// "Copy as podcast" sync to an iPod. The gathered values are applied to every
// track in the selection (see Organize / GPodDevice::CopyToStorage) so that the
// episodes group under one browsable show rather than appearing as one podcast
// per track. Use exec(); on accept() read info().
class PodcastDetailsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PodcastDetailsDialog(QWidget *parent = nullptr);
  ~PodcastDetailsDialog() override;

  // Pre-fill the fields with a best guess derived from the selection (e.g. the
  // common album as the show title and the common albumartist as the author).
  void SetInitialValues(const QString &show_title, const QString &author);

  MusicStorage::PodcastInfo info() const;

 protected:
  void showEvent(QShowEvent *e) override;

 private Q_SLOTS:
  void ValidateFields();

 private:
  Ui_PodcastDetailsDialog *ui_;
};

#endif  // PODCASTDETAILSDIALOG_H
