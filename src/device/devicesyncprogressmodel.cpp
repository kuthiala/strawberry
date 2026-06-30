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

#include <QAbstractListModel>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QList>
#include <QModelIndex>
#include <QObject>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QVariant>

#include "core/logging.h"
#include "core/song.h"
#include "devicesyncprogressmodel.h"

namespace {

// Build small status icons by hand so we don't have to ship new resources.
// 16x16 keeps them inline with the default list-item row height.
QIcon BuildSolidCircleIcon(const QColor &color, const QChar glyph = QChar()) {
  QPixmap pixmap(16, 16);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(color);
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(QRect(1, 1, 14, 14));
  if (!glyph.isNull()) {
    painter.setPen(Qt::white);
    QFont f = painter.font();
    f.setPointSize(9);
    f.setBold(true);
    painter.setFont(f);
    painter.drawText(QRect(0, 0, 16, 16), Qt::AlignCenter, QString(glyph));
  }
  return QIcon(pixmap);
}

QIcon BuildPendingIcon() {
  QPixmap pixmap(16, 16);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  // Hollow circle in a subdued grey to indicate "waiting".
  QPen pen(QColor(140, 140, 140));
  pen.setWidth(2);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRect(2, 2, 12, 12));
  return QIcon(pixmap);
}

QIcon BuildRetryIcon() {
  // Amber "clock" circle with a small dot in the middle to visually
  // separate a retrying song from a fresh pending one. We deliberately
  // pick a high-contrast warning color (not red) so the user reads it as
  // "transient hiccup, still being worked on" rather than "this failed".
  QPixmap pixmap(16, 16);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(QColor(217, 145, 38));  // amber
  pen.setWidth(2);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRect(2, 2, 12, 12));
  painter.setBrush(QColor(217, 145, 38));
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(QRect(7, 7, 2, 2));
  return QIcon(pixmap);
}

// Human-readable rendering of a millisecond delay, kept compact for the
// trailing "[retry N/M in Xs]" annotation. Anything under a minute is shown
// in whole seconds; longer waits round to the nearest 10s and switch to
// minutes/seconds so the annotation never exceeds ~10 characters.
QString FormatBackoff(qint64 ms) {
  if (ms < 0) ms = 0;
  const qint64 seconds = (ms + 500) / 1000;  // round to nearest second
  if (seconds < 60) {
    return QObject::tr("%1s").arg(seconds);
  }
  const qint64 minutes = seconds / 60;
  const qint64 rem = seconds % 60;
  if (rem == 0) return QObject::tr("%1m").arg(minutes);
  return QObject::tr("%1m %2s").arg(minutes).arg(rem);
}

}  // namespace

namespace {
// File-local pointer set by DeviceManager. Not owned. nullptr until the first
// DeviceManager is constructed, which in the live application is during
// MainWindow setup. Using a raw global is safe because DeviceManager outlives
// every Organize that would dereference it, and OrganizeDialog (the sole
// caller of instance()) only ever runs on the main thread.
DeviceSyncProgressModel *g_instance = nullptr;
}  // namespace

DeviceSyncProgressModel *DeviceSyncProgressModel::instance() { return g_instance; }
void DeviceSyncProgressModel::SetInstance(DeviceSyncProgressModel *model) { g_instance = model; }

DeviceSyncProgressModel::DeviceSyncProgressModel(QObject *parent)
    : QAbstractListModel(parent),
      syncing_(false),
      next_song_index_(0),
      done_count_(0),
      failed_count_(0) {

  icon_pending_ = BuildPendingIcon();
  icon_retrying_ = BuildRetryIcon();
  // Green check, red cross. Using simple unicode glyphs centered on a colored
  // disc avoids any dependency on system icon themes and keeps the look
  // consistent across the macOS / Linux / Windows builds. Explicit codepoints
  // (rather than literal glyphs in the source) so the encoding can't get
  // mangled by editors that re-save the file in something other than UTF-8.
  icon_done_ = BuildSolidCircleIcon(QColor(34, 139, 34), QChar(0x2713));    // CHECK MARK
  icon_failed_ = BuildSolidCircleIcon(QColor(178, 34, 34), QChar(0x00D7));  // MULTIPLICATION SIGN

}

int DeviceSyncProgressModel::rowCount(const QModelIndex &parent) const {
  if (parent.isValid()) return 0;
  return rows_.count();
}

QVariant DeviceSyncProgressModel::data(const QModelIndex &index, int role) const {

  if (!index.isValid() || index.row() < 0 || index.row() >= rows_.count()) {
    return QVariant();
  }

  const Row &row = rows_.at(index.row());

  switch (role) {
    case Qt::DisplayRole: {
      // While a row is between retries we append a compact "[retry N/M in Xs]"
      // suffix to the title so the user can see at a glance which song is
      // being retried and how long the next attempt is gated for. We don't
      // touch the underlying `title` so the suffix can be cleared on success
      // without losing the song name.
      if (row.status == Status::Retrying && row.retry_attempt > 0) {
        return tr("%1  [retry %2/%3 in %4]").arg(
            row.title,
            QString::number(row.retry_attempt),
            QString::number(row.retry_max),
            FormatBackoff(row.retry_delay_ms));
      }
      return row.title;
    }
    case Qt::DecorationRole:
      switch (row.status) {
        case Status::Pending:    return icon_pending_;
        case Status::InProgress: return icon_pending_;
        case Status::Retrying:   return icon_retrying_;
        case Status::Done:       return icon_done_;
        case Status::Failed:     return icon_failed_;
      }
      return icon_pending_;
    case Qt::ToolTipRole:
      switch (row.status) {
        case Status::Pending:    return tr("Pending");
        case Status::InProgress: return tr("In progress");
        case Status::Retrying:
          return tr("Last attempt failed -- retry %1 of %2 in %3").arg(
              QString::number(row.retry_attempt),
              QString::number(row.retry_max),
              FormatBackoff(row.retry_delay_ms));
        case Status::Done:       return tr("Copied");
        case Status::Failed:     return tr("Failed after all retries");
      }
      return QVariant();
    case Role_Status:
      return static_cast<int>(row.status);
    default:
      return QVariant();
  }

}

void DeviceSyncProgressModel::StartSync(const QString &device_label, const SongList &queue) {

  beginResetModel();
  rows_.clear();
  rows_.reserve(queue.count());
  for (const Song &song : queue) {
    QString title = song.PrettyTitleWithArtist();
    if (title.trimmed().isEmpty()) {
      title = song.basefilename();
    }
    if (title.trimmed().isEmpty()) {
      // Last-ditch fallback so the user always sees *something* per row.
      title = QFileInfo(song.url().toLocalFile()).fileName();
    }
    rows_.append(Row{title, Status::Pending});
  }
  device_label_ = device_label;
  syncing_ = true;
  next_song_index_ = 0;
  done_count_ = 0;
  failed_count_ = 0;
  endResetModel();

  Q_EMIT SyncStarted(device_label_, rows_.count());

}

void DeviceSyncProgressModel::SongFinished(const Song &song, const bool success) {

  Q_UNUSED(song);

  if (next_song_index_ < 0 || next_song_index_ >= rows_.count()) {
    qLog(Warning) << "DeviceSyncProgressModel::SongFinished called past end of queue (index"
                  << next_song_index_ << "of" << rows_.count() << ")";
    return;
  }

  const int row = next_song_index_++;
  rows_[row].status = success ? Status::Done : Status::Failed;
  // Clear any retry annotation now that the song is resolved one way or
  // the other -- the green check / red cross is the final state.
  rows_[row].retry_attempt = 0;
  rows_[row].retry_max = 0;
  rows_[row].retry_delay_ms = 0;
  if (success) ++done_count_; else ++failed_count_;

  const QModelIndex idx = index(row);
  Q_EMIT dataChanged(idx, idx, {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole, Role_Status});
  Q_EMIT SongStatusChanged(row, rows_[row].status);

}

void DeviceSyncProgressModel::SongRetryScheduled(const Song &song, const int attempt, const int max_attempts, const qint64 delay_ms) {

  Q_UNUSED(song);

  // The retry corresponds to the song *currently* at next_song_index_ --
  // Organize emits SongSyncRetry before re-queueing, so it has not advanced
  // the cursor (SongFinished does that).
  if (next_song_index_ < 0 || next_song_index_ >= rows_.count()) {
    qLog(Warning) << "DeviceSyncProgressModel::SongRetryScheduled called past end of queue (index"
                  << next_song_index_ << "of" << rows_.count() << ")";
    return;
  }

  const int row = next_song_index_;
  rows_[row].status = Status::Retrying;
  rows_[row].retry_attempt = attempt;
  rows_[row].retry_max = max_attempts;
  rows_[row].retry_delay_ms = delay_ms;

  const QModelIndex idx = index(row);
  Q_EMIT dataChanged(idx, idx, {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole, Role_Status});
  Q_EMIT SongStatusChanged(row, rows_[row].status);

}

void DeviceSyncProgressModel::EndSync() {

  if (!syncing_) return;
  syncing_ = false;
  Q_EMIT SyncFinished(done_count_, failed_count_, rows_.count());

}