/*
 * Strawberry Music Player
 * Copyright 2026, Strawberry contributors
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

#ifndef GPODPLAYLISTSDIALOG_H
#define GPODPLAYLISTSDIALOG_H

#include "config.h"

#include <QDialog>
#include <QString>

#include "gpodplaylistmanager.h"

class QCloseEvent;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class GPodDevice;

// GPodPlaylistsDialog — programmatically-built modeless-ish dialog that
// exposes every read + write operation on GPodPlaylistManager and, on
// Save, asks the owning GPodDevice to flush the iTunesDB.
//
// UX layout:
//
//   [+ New] [Rename] [Delete] [↑] [↓]        [Add tracks…] [Remove] [↑] [↓]
//   +---------------------+-----------------------------------------------+
//   | Playlists           | Tracks in "<selected playlist>"               |
//   |   Master Playlist * |   #  Title      Artist    Album      Length  |
//   |   Recently Added    |   1  ...        ...       ...        ...     |
//   |   Chill             |   2  ...                                     |
//   |   ...               |                                              |
//   +---------------------+-----------------------------------------------+
//   [Save]  [Close]
//
// (*) MPL is shown but its buttons stay disabled — it's the browse-all
// entry, edited via sync not by hand.
//
// "Save" calls GPodDevice::WritePlaylistChanges() which acquires
// db_busy_, runs the standard throttled WriteDatabase(), and displays a
// success/error toast. Close-without-save shows a confirmation.
class GPodPlaylistsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit GPodPlaylistsDialog(GPodDevice *device, GPodPlaylistManager *manager, QWidget *parent = nullptr);
  ~GPodPlaylistsDialog() override = default;

 protected:
  void closeEvent(QCloseEvent *event) override;

 private Q_SLOTS:
  void RefreshPlaylists();
  void RefreshTracks();
  void OnPlaylistSelectionChanged();

  void OnNewPlaylist();
  void OnRenamePlaylist();
  void OnDeletePlaylist();
  void OnMovePlaylistUp();
  void OnMovePlaylistDown();

  void OnAddTracks();
  void OnRemoveTracks();
  void OnMoveTrackUp();
  void OnMoveTrackDown();

  void OnSave();

 private:
  void BuildUi();
  void UpdateButtonStates();
  quint64 SelectedPlaylistId() const;
  QList<quint64> SelectedTrackDbids() const;

 private:
  GPodDevice *device_;               // not owned
  GPodPlaylistManager *manager_;     // not owned

  // Left pane
  QListWidget *playlists_list_;
  QPushButton *btn_new_;
  QPushButton *btn_rename_;
  QPushButton *btn_delete_;
  QPushButton *btn_pl_up_;
  QPushButton *btn_pl_down_;

  // Right pane
  QTreeWidget *tracks_tree_;
  QPushButton *btn_add_tracks_;
  QPushButton *btn_remove_tracks_;
  QPushButton *btn_tr_up_;
  QPushButton *btn_tr_down_;

  QLabel *status_label_;
  QPushButton *btn_save_;
  QPushButton *btn_close_;
};

#endif  // GPODPLAYLISTSDIALOG_H