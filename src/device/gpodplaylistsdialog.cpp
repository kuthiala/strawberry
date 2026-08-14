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

#include "gpodplaylistsdialog.h"

#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include "core/logging.h"
#include "gpoddevice.h"

namespace {

constexpr int kPlaylistIdRole = Qt::UserRole + 1;
constexpr int kTrackDbidRole = Qt::UserRole + 1;

QString FormatDuration(int length_ms) {
  if (length_ms <= 0) return QStringLiteral("-");
  const int seconds = length_ms / 1000;
  const int m = seconds / 60;
  const int s = seconds % 60;
  return QString::asprintf("%d:%02d", m, s);
}

}  // namespace

GPodPlaylistsDialog::GPodPlaylistsDialog(GPodDevice *device, GPodPlaylistManager *manager, QWidget *parent)
    : QDialog(parent),
      device_(device),
      manager_(manager),
      playlists_list_(nullptr),
      btn_new_(nullptr),
      btn_rename_(nullptr),
      btn_delete_(nullptr),
      btn_pl_up_(nullptr),
      btn_pl_down_(nullptr),
      tracks_tree_(nullptr),
      btn_add_tracks_(nullptr),
      btn_remove_tracks_(nullptr),
      btn_tr_up_(nullptr),
      btn_tr_down_(nullptr),
      status_label_(nullptr),
      btn_save_(nullptr),
      btn_close_(nullptr) {
  setWindowTitle(tr("iPod Playlists"));
  resize(900, 600);
  BuildUi();
  RefreshPlaylists();
  UpdateButtonStates();

  connect(manager_, &GPodPlaylistManager::PlaylistsChanged, this, [this]() {
    // Refresh both panes and preserve selection where possible.
    const quint64 pid = SelectedPlaylistId();
    RefreshPlaylists();
    if (pid) {
      for (int i = 0; i < playlists_list_->count(); ++i) {
        if (playlists_list_->item(i)->data(kPlaylistIdRole).toULongLong() == pid) {
          playlists_list_->setCurrentRow(i);
          break;
        }
      }
    }
    RefreshTracks();
    UpdateButtonStates();
  });
}

void GPodPlaylistsDialog::BuildUi() {
  auto *outer = new QVBoxLayout(this);

  auto *panes = new QHBoxLayout;
  outer->addLayout(panes, /*stretch=*/1);

  // ------- Left pane: playlists list + buttons -------
  auto *left = new QVBoxLayout;
  panes->addLayout(left, /*stretch=*/1);

  auto *left_label = new QLabel(tr("Playlists"), this);
  left->addWidget(left_label);

  playlists_list_ = new QListWidget(this);
  playlists_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  left->addWidget(playlists_list_, /*stretch=*/1);

  auto *left_buttons = new QHBoxLayout;
  btn_new_ = new QPushButton(tr("New"), this);
  btn_rename_ = new QPushButton(tr("Rename"), this);
  btn_delete_ = new QPushButton(tr("Delete"), this);
  btn_pl_up_ = new QPushButton(tr("▲"), this);
  btn_pl_down_ = new QPushButton(tr("▼"), this);
  btn_pl_up_->setToolTip(tr("Move playlist up"));
  btn_pl_down_->setToolTip(tr("Move playlist down"));
  left_buttons->addWidget(btn_new_);
  left_buttons->addWidget(btn_rename_);
  left_buttons->addWidget(btn_delete_);
  left_buttons->addStretch();
  left_buttons->addWidget(btn_pl_up_);
  left_buttons->addWidget(btn_pl_down_);
  left->addLayout(left_buttons);

  // ------- Right pane: track table + buttons -------
  auto *right = new QVBoxLayout;
  panes->addLayout(right, /*stretch=*/2);

  auto *right_label = new QLabel(tr("Tracks"), this);
  right->addWidget(right_label);

  tracks_tree_ = new QTreeWidget(this);
  tracks_tree_->setColumnCount(5);
  tracks_tree_->setHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("Album"), tr("Length")});
  tracks_tree_->setRootIsDecorated(false);
  tracks_tree_->setUniformRowHeights(true);
  tracks_tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tracks_tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tracks_tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  tracks_tree_->header()->setSectionResizeMode(2, QHeaderView::Interactive);
  tracks_tree_->header()->setSectionResizeMode(3, QHeaderView::Interactive);
  tracks_tree_->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  right->addWidget(tracks_tree_, /*stretch=*/1);

  auto *right_buttons = new QHBoxLayout;
  btn_add_tracks_ = new QPushButton(tr("Add tracks…"), this);
  btn_remove_tracks_ = new QPushButton(tr("Remove selected"), this);
  btn_tr_up_ = new QPushButton(tr("▲"), this);
  btn_tr_down_ = new QPushButton(tr("▼"), this);
  btn_tr_up_->setToolTip(tr("Move track up"));
  btn_tr_down_->setToolTip(tr("Move track down"));
  right_buttons->addWidget(btn_add_tracks_);
  right_buttons->addWidget(btn_remove_tracks_);
  right_buttons->addStretch();
  right_buttons->addWidget(btn_tr_up_);
  right_buttons->addWidget(btn_tr_down_);
  right->addLayout(right_buttons);

  // ------- Bottom row: status + Save/Close -------
  auto *bottom = new QHBoxLayout;
  status_label_ = new QLabel(tr("No unsaved changes."), this);
  btn_save_ = new QPushButton(tr("Save to iPod"), this);
  btn_close_ = new QPushButton(tr("Close"), this);
  bottom->addWidget(status_label_, /*stretch=*/1);
  bottom->addWidget(btn_save_);
  bottom->addWidget(btn_close_);
  outer->addLayout(bottom);

  // ------- Signals -------
  connect(playlists_list_, &QListWidget::itemSelectionChanged, this, &GPodPlaylistsDialog::OnPlaylistSelectionChanged);
  connect(tracks_tree_, &QTreeWidget::itemSelectionChanged, this, &GPodPlaylistsDialog::UpdateButtonStates);

  connect(btn_new_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnNewPlaylist);
  connect(btn_rename_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnRenamePlaylist);
  connect(btn_delete_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnDeletePlaylist);
  connect(btn_pl_up_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnMovePlaylistUp);
  connect(btn_pl_down_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnMovePlaylistDown);

  connect(btn_add_tracks_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnAddTracks);
  connect(btn_remove_tracks_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnRemoveTracks);
  connect(btn_tr_up_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnMoveTrackUp);
  connect(btn_tr_down_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnMoveTrackDown);

  connect(btn_save_, &QPushButton::clicked, this, &GPodPlaylistsDialog::OnSave);
  connect(btn_close_, &QPushButton::clicked, this, &QDialog::close);
}

void GPodPlaylistsDialog::closeEvent(QCloseEvent *event) {
  if (manager_ && manager_->IsDirty()) {
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved playlist changes"),
        tr("You have unsaved playlist changes. Save them to the iPod before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
      OnSave();
    }
    else if (answer == QMessageBox::Cancel) {
      event->ignore();
      return;
    }
  }
  event->accept();
}

// -----------------------------------------------------------------------------
// Selection / state helpers
// -----------------------------------------------------------------------------

quint64 GPodPlaylistsDialog::SelectedPlaylistId() const {
  if (!playlists_list_) return 0;
  QListWidgetItem *item = playlists_list_->currentItem();
  return item ? item->data(kPlaylistIdRole).toULongLong() : 0;
}

QList<quint64> GPodPlaylistsDialog::SelectedTrackDbids() const {
  QList<quint64> out;
  if (!tracks_tree_) return out;
  const auto selected = tracks_tree_->selectedItems();
  out.reserve(selected.size());
  for (QTreeWidgetItem *it : selected) {
    out.append(it->data(0, kTrackDbidRole).toULongLong());
  }
  return out;
}

void GPodPlaylistsDialog::UpdateButtonStates() {
  const quint64 pid = SelectedPlaylistId();
  bool have_pl = pid != 0;
  bool is_master = false;
  if (have_pl) {
    for (const auto &info : manager_->ListPlaylists()) {
      if (info.id == pid) {
        is_master = info.is_master;
        break;
      }
    }
  }
  const bool editable = have_pl && !is_master;
  btn_rename_->setEnabled(editable);
  btn_delete_->setEnabled(editable);
  btn_pl_up_->setEnabled(editable);
  btn_pl_down_->setEnabled(editable);
  btn_add_tracks_->setEnabled(editable);

  const bool have_tracks = !tracks_tree_->selectedItems().isEmpty();
  btn_remove_tracks_->setEnabled(editable && have_tracks);
  btn_tr_up_->setEnabled(editable && have_tracks);
  btn_tr_down_->setEnabled(editable && have_tracks);

  const bool dirty = manager_ && manager_->IsDirty();
  btn_save_->setEnabled(dirty);
  status_label_->setText(dirty ? tr("Unsaved playlist changes.") : tr("No unsaved changes."));
}

// -----------------------------------------------------------------------------
// Refresh
// -----------------------------------------------------------------------------

void GPodPlaylistsDialog::RefreshPlaylists() {
  if (!manager_) return;
  playlists_list_->clear();
  for (const auto &info : manager_->ListPlaylists()) {
    QString label;
    if (info.is_master) {
      label = tr("%1 (all tracks) [%2]").arg(info.name.isEmpty() ? tr("iPod") : info.name).arg(info.track_count);
    }
    else {
      label = QStringLiteral("%1 [%2]%3").arg(info.name).arg(info.track_count).arg(info.is_smart ? tr(" [smart]") : QString());
    }
    auto *item = new QListWidgetItem(label);
    item->setData(kPlaylistIdRole, static_cast<qulonglong>(info.id));
    if (info.is_master || info.is_smart) {
      QFont f = item->font();
      f.setItalic(true);
      item->setFont(f);
    }
    playlists_list_->addItem(item);
  }
  if (playlists_list_->count() > 0 && !playlists_list_->currentItem()) {
    playlists_list_->setCurrentRow(0);
  }
}

void GPodPlaylistsDialog::RefreshTracks() {
  if (!manager_) return;
  tracks_tree_->clear();
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  const auto tracks = manager_->GetTracks(pid);
  int n = 0;
  for (const auto &t : tracks) {
    ++n;
    auto *item = new QTreeWidgetItem(tracks_tree_);
    item->setText(0, QString::number(n));
    item->setText(1, t.title);
    item->setText(2, t.artist);
    item->setText(3, t.album);
    item->setText(4, FormatDuration(t.length_ms));
    item->setData(0, kTrackDbidRole, static_cast<qulonglong>(t.dbid));
  }
}

void GPodPlaylistsDialog::OnPlaylistSelectionChanged() {
  RefreshTracks();
  UpdateButtonStates();
}

// -----------------------------------------------------------------------------
// Playlist actions
// -----------------------------------------------------------------------------

void GPodPlaylistsDialog::OnNewPlaylist() {
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("New playlist"), tr("Playlist name:"), QLineEdit::Normal, tr("New Playlist"), &ok);
  if (!ok || name.trimmed().isEmpty()) return;
  const quint64 pid = manager_->CreatePlaylist(name.trimmed());
  if (pid == 0) {
    QMessageBox::warning(this, tr("New playlist"), tr("Failed to create playlist."));
  }
}

void GPodPlaylistsDialog::OnRenamePlaylist() {
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  QString current;
  for (const auto &info : manager_->ListPlaylists()) {
    if (info.id == pid) { current = info.name; break; }
  }
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("Rename playlist"), tr("New name:"), QLineEdit::Normal, current, &ok);
  if (!ok || name.trimmed().isEmpty()) return;
  if (!manager_->RenamePlaylist(pid, name.trimmed())) {
    QMessageBox::warning(this, tr("Rename playlist"), tr("Failed to rename playlist."));
  }
}

void GPodPlaylistsDialog::OnDeletePlaylist() {
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  QString name;
  for (const auto &info : manager_->ListPlaylists()) {
    if (info.id == pid) { name = info.name; break; }
  }
  const auto answer = QMessageBox::question(this, tr("Delete playlist"),
      tr("Delete the playlist \"%1\"? The tracks themselves remain on the iPod.").arg(name),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer != QMessageBox::Yes) return;
  if (!manager_->DeletePlaylist(pid)) {
    QMessageBox::warning(this, tr("Delete playlist"), tr("Failed to delete playlist."));
  }
}

void GPodPlaylistsDialog::OnMovePlaylistUp() {
  const int row = playlists_list_->currentRow();
  if (row <= 0) return;
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  manager_->ReorderPlaylist(pid, row - 1);
}

void GPodPlaylistsDialog::OnMovePlaylistDown() {
  const int row = playlists_list_->currentRow();
  if (row < 0 || row >= playlists_list_->count() - 1) return;
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  manager_->ReorderPlaylist(pid, row + 1);
}

// -----------------------------------------------------------------------------
// Track picker (inline sub-dialog for "Add tracks…")
// -----------------------------------------------------------------------------

void GPodPlaylistsDialog::OnAddTracks() {
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;

  const quint64 mpl_id = manager_->MasterPlaylistId();
  if (!mpl_id) {
    QMessageBox::warning(this, tr("Add tracks"), tr("Could not read master playlist."));
    return;
  }
  const auto all_tracks = manager_->GetTracks(mpl_id);
  if (all_tracks.isEmpty()) {
    QMessageBox::information(this, tr("Add tracks"), tr("No tracks on the iPod."));
    return;
  }

  // Existing membership of the target playlist — used to grey out items already present.
  QSet<quint64> already_in;
  for (const auto &t : manager_->GetTracks(pid)) already_in.insert(t.dbid);

  QDialog picker(this);
  picker.setWindowTitle(tr("Add tracks to playlist"));
  picker.resize(700, 500);
  auto *pv = new QVBoxLayout(&picker);
  auto *search = new QLineEdit(&picker);
  search->setPlaceholderText(tr("Filter by title / artist / album…"));
  pv->addWidget(search);

  auto *tree = new QTreeWidget(&picker);
  tree->setColumnCount(4);
  tree->setHeaderLabels({tr("Title"), tr("Artist"), tr("Album"), tr("Length")});
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree->setUniformRowHeights(true);
  tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
  tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
  tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  pv->addWidget(tree, 1);

  for (const auto &t : all_tracks) {
    auto *item = new QTreeWidgetItem(tree);
    item->setText(0, t.title);
    item->setText(1, t.artist);
    item->setText(2, t.album);
    item->setText(3, FormatDuration(t.length_ms));
    item->setData(0, kTrackDbidRole, static_cast<qulonglong>(t.dbid));
    if (already_in.contains(t.dbid)) {
      item->setDisabled(true);
      item->setToolTip(0, tr("Already in the playlist."));
    }
  }

  connect(search, &QLineEdit::textChanged, tree, [tree](const QString &q) {
    const QString needle = q.trimmed().toLower();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
      QTreeWidgetItem *it = tree->topLevelItem(i);
      const bool match = needle.isEmpty()
          || it->text(0).contains(needle, Qt::CaseInsensitive)
          || it->text(1).contains(needle, Qt::CaseInsensitive)
          || it->text(2).contains(needle, Qt::CaseInsensitive);
      it->setHidden(!match);
    }
  });

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Add selected"));
  pv->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);

  if (picker.exec() != QDialog::Accepted) return;

  QList<quint64> selected;
  for (QTreeWidgetItem *it : tree->selectedItems()) {
    if (it->isDisabled()) continue;
    selected.append(it->data(0, kTrackDbidRole).toULongLong());
  }
  if (selected.isEmpty()) return;
  if (!manager_->AddTracks(pid, selected)) {
    QMessageBox::warning(this, tr("Add tracks"), tr("Failed to add tracks (or all were already present)."));
  }
}

void GPodPlaylistsDialog::OnRemoveTracks() {
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  const auto dbids = SelectedTrackDbids();
  if (dbids.isEmpty()) return;
  manager_->RemoveTracks(pid, dbids);
}

void GPodPlaylistsDialog::OnMoveTrackUp() {
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  const auto items = tracks_tree_->selectedItems();
  if (items.size() != 1) return;
  QTreeWidgetItem *it = items.first();
  const int row = tracks_tree_->indexOfTopLevelItem(it);
  if (row <= 0) return;
  const quint64 dbid = it->data(0, kTrackDbidRole).toULongLong();
  manager_->MoveTrack(pid, dbid, row - 1);
}

void GPodPlaylistsDialog::OnMoveTrackDown() {
  const quint64 pid = SelectedPlaylistId();
  if (!pid) return;
  const auto items = tracks_tree_->selectedItems();
  if (items.size() != 1) return;
  QTreeWidgetItem *it = items.first();
  const int row = tracks_tree_->indexOfTopLevelItem(it);
  if (row < 0 || row >= tracks_tree_->topLevelItemCount() - 1) return;
  const quint64 dbid = it->data(0, kTrackDbidRole).toULongLong();
  manager_->MoveTrack(pid, dbid, row + 1);
}

// -----------------------------------------------------------------------------
// Save
// -----------------------------------------------------------------------------

void GPodPlaylistsDialog::OnSave() {
  if (!device_ || !manager_) return;
  if (!manager_->IsDirty()) return;
  QString error;
  if (!device_->WritePlaylistChanges(error)) {
    QMessageBox::critical(this, tr("Save to iPod"),
        tr("Failed to write the iTunes database:\n%1").arg(error));
    return;
  }
  manager_->ResetDirty();
  UpdateButtonStates();
  QMessageBox::information(this, tr("Save to iPod"),
      tr("Playlist changes saved. Eject the iPod normally to finalise."));
}