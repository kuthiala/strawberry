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
#include "organize/organizequeue.h"
#include "devicesyncprogressmodel.h"

using namespace Qt::Literals::StringLiterals;

namespace {

// Status colours, the same hues as the Claude theme so the pane matches it, they read well on light and dark backgrounds.
constexpr QRgb kColorPending = 0x9A968C;
constexpr QRgb kColorActive = 0xD97757;
constexpr QRgb kColorRetrying = 0xC8923A;
constexpr QRgb kColorDone = 0x5E9467;
constexpr QRgb kColorFailed = 0xC0504A;

// Icons are drawn by hand so no resources need to be shipped, at 2x for HiDPI screens.
QPixmap NewIconPixmap() {
  QPixmap pixmap(32, 32);
  pixmap.setDevicePixelRatio(2.0);
  pixmap.fill(Qt::transparent);
  return pixmap;
}

QIcon BuildSolidCircleIcon(const QColor &color, const QChar glyph = QChar()) {
  QPixmap pixmap = NewIconPixmap();
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(color);
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(QRectF(1.5, 1.5, 13, 13));
  if (!glyph.isNull()) {
    painter.setPen(Qt::white);
    QFont f = painter.font();
    f.setPixelSize(10);
    f.setBold(true);
    painter.setFont(f);
    painter.drawText(QRect(0, 0, 16, 16), Qt::AlignCenter, QString(glyph));
  }
  return QIcon(pixmap);
}

QIcon BuildRingIcon(const QColor &color, const bool dot) {
  QPixmap pixmap = NewIconPixmap();
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(color);
  pen.setWidthF(1.6);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRectF(2.5, 2.5, 11, 11));
  if (dot) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(5.5, 5.5, 5, 5));
  }
  return QIcon(pixmap);
}

// Human-readable rendering of a millisecond delay, kept compact for the trailing "[retry N/M in Xs]" annotation.
QString FormatBackoff(qint64 ms) {
  if (ms < 0) ms = 0;
  const qint64 seconds = (ms + 500) / 1000;
  if (seconds < 60) {
    return QObject::tr("%1s").arg(seconds);
  }
  const qint64 minutes = seconds / 60;
  const qint64 rem = seconds % 60;
  if (rem == 0) return QObject::tr("%1m").arg(minutes);
  return QObject::tr("%1m %2s").arg(minutes).arg(rem);
}

bool IsFinalStatus(const DeviceSyncProgressModel::Status status) {
  return status == DeviceSyncProgressModel::Status::Done || status == DeviceSyncProgressModel::Status::Failed || status == DeviceSyncProgressModel::Status::Skipped;
}

}  // namespace

DeviceSyncProgressModel::DeviceSyncProgressModel(OrganizeQueue *queue, QObject *parent)
    : QAbstractListModel(parent),
      queue_(queue) {

  icon_pending_ = BuildRingIcon(QColor(kColorPending), false);
  icon_active_ = BuildRingIcon(QColor(kColorActive), true);
  icon_retrying_ = BuildRingIcon(QColor(kColorRetrying), true);
  // Explicit codepoints so the encoding can't get mangled by editors.
  icon_done_ = BuildSolidCircleIcon(QColor(kColorDone), QChar(0x2713));      // CHECK MARK
  icon_failed_ = BuildSolidCircleIcon(QColor(kColorFailed), QChar(0x00D7));  // MULTIPLICATION SIGN
  icon_skipped_ = BuildSolidCircleIcon(QColor(kColorPending), QChar(0x2013));  // EN DASH

  if (queue_) {
    QObject::connect(queue_, &OrganizeQueue::JobAdded, this, &DeviceSyncProgressModel::JobAdded);
    QObject::connect(queue_, &OrganizeQueue::JobSongsChanged, this, &DeviceSyncProgressModel::JobSongsChanged);
    QObject::connect(queue_, &OrganizeQueue::JobChanged, this, &DeviceSyncProgressModel::JobChanged);
    QObject::connect(queue_, &OrganizeQueue::JobRemoved, this, &DeviceSyncProgressModel::JobRemoved);
    QObject::connect(queue_, &OrganizeQueue::SongStarted, this, &DeviceSyncProgressModel::SongStarted);
    QObject::connect(queue_, &OrganizeQueue::SongFinished, this, &DeviceSyncProgressModel::SongFinished);
    QObject::connect(queue_, &OrganizeQueue::SongRetry, this, &DeviceSyncProgressModel::SongRetry);
  }

}

QString DeviceSyncProgressModel::SongTitle(const Song &song) {

  QString title = song.PrettyTitleWithArtist();
  if (title.trimmed().isEmpty()) title = song.basefilename();
  if (title.trimmed().isEmpty()) title = QFileInfo(song.url().toLocalFile()).fileName();
  return title;

}

QList<DeviceSyncProgressModel::SongRow> DeviceSyncProgressModel::SongRows(const SongList &songs) {

  QList<SongRow> rows;
  rows.reserve(songs.count());
  for (const Song &song : songs) {
    SongRow row;
    row.title = SongTitle(song);
    row.url = song.url().toString();
    rows << row;
  }
  return rows;

}

int DeviceSyncProgressModel::rowCount(const QModelIndex &parent) const {

  if (parent.isValid()) return 0;

  int count = 0;
  for (const JobBlock &block : jobs_) {
    count += 1 + static_cast<int>(block.songs.count());
  }
  return count;

}

int DeviceSyncProgressModel::JobPosition(const quint64 id) const {

  for (int i = 0; i < jobs_.count(); ++i) {
    if (jobs_[i].info.id == id) return i;
  }
  return -1;

}

int DeviceSyncProgressModel::JobFirstRow(const int position) const {

  int row = 0;
  for (int i = 0; i < position; ++i) {
    row += 1 + static_cast<int>(jobs_[i].songs.count());
  }
  return row;

}

bool DeviceSyncProgressModel::Locate(int row, int *position, int *song) const {

  if (row < 0) return false;
  for (int i = 0; i < jobs_.count(); ++i) {
    const int size = 1 + static_cast<int>(jobs_[i].songs.count());
    if (row < size) {
      *position = i;
      *song = row - 1;
      return true;
    }
    row -= size;
  }
  return false;

}

QIcon DeviceSyncProgressModel::JobIcon(const OrganizeQueue::JobState state) const {

  switch (state) {
    case OrganizeQueue::JobState::Preparing: return icon_retrying_;
    case OrganizeQueue::JobState::Queued:    return icon_pending_;
    case OrganizeQueue::JobState::Running:   return icon_active_;
    case OrganizeQueue::JobState::Finished:  return icon_done_;
    case OrganizeQueue::JobState::Failed:    return icon_failed_;
    case OrganizeQueue::JobState::Cancelled: return icon_skipped_;
  }
  return icon_pending_;

}

QIcon DeviceSyncProgressModel::SongIcon(const Status status) const {

  switch (status) {
    case Status::Pending:    return icon_pending_;
    case Status::InProgress: return icon_active_;
    case Status::Retrying:   return icon_retrying_;
    case Status::Done:       return icon_done_;
    case Status::Failed:     return icon_failed_;
    case Status::Skipped:    return icon_skipped_;
  }
  return icon_pending_;

}

QVariant DeviceSyncProgressModel::data(const QModelIndex &index, const int role) const {

  int position = -1;
  int song = -1;
  if (!index.isValid() || !Locate(index.row(), &position, &song)) return QVariant();

  const JobBlock &block = jobs_[position];
  const OrganizeQueue::JobInfo &info = block.info;

  if (song < 0) {
    switch (role) {
      case Qt::DisplayRole: {
        QString text = u"%1 · %2"_s.arg(OrganizeQueue::MediaTypeName(info.media_type), info.title);
        QString status = OrganizeQueue::StateName(info.state);
        if (info.state == OrganizeQueue::JobState::Running || OrganizeQueue::IsFinalState(info.state)) {
          status += u" %1/%2"_s.arg(info.done + info.failed).arg(info.total);
        }
        if (!info.detail.isEmpty()) status = info.detail;
        return u"%1\n%2 → %3"_s.arg(text, status, info.destination_label);
      }
      case Qt::DecorationRole:
        return JobIcon(info.state);
      case Qt::ToolTipRole:
        return tr("%1 to %2: %3 copied, %4 failed of %5").arg(info.title, info.destination_label).arg(info.done).arg(info.failed).arg(info.total);
      case Qt::FontRole: {
        QFont font;
        font.setBold(true);
        return font;
      }
      case Role_IsJob:
        return true;
      case Role_JobId:
        return info.id;
      case Role_JobState:
        return static_cast<int>(info.state);
      default:
        return QVariant();
    }
  }

  const SongRow &row = block.songs[song];

  switch (role) {
    case Qt::DisplayRole:
      if (row.status == Status::Retrying && row.retry_attempt > 0) {
        return tr("%1  [retry %2/%3 in %4]").arg(row.title, QString::number(row.retry_attempt), QString::number(row.retry_max), FormatBackoff(row.retry_delay_ms));
      }
      return row.title;
    case Qt::DecorationRole:
      return SongIcon(row.status);
    case Qt::ToolTipRole:
      switch (row.status) {
        case Status::Pending:    return tr("Pending");
        case Status::InProgress: return tr("Copying");
        case Status::Retrying:   return tr("Last attempt failed -- retry %1 of %2 in %3").arg(QString::number(row.retry_attempt), QString::number(row.retry_max), FormatBackoff(row.retry_delay_ms));
        case Status::Done:       return tr("Copied");
        case Status::Failed:     return tr("Failed after all retries");
        case Status::Skipped:    return tr("Not copied, the job was cancelled");
      }
      return QVariant();
    case Role_Status:
      return static_cast<int>(row.status);
    case Role_IsJob:
      return false;
    case Role_JobId:
      return info.id;
    case Role_JobState:
      return static_cast<int>(info.state);
    default:
      return QVariant();
  }

}

QString DeviceSyncProgressModel::Summary() const {

  int running = 0;
  int waiting = 0;
  int done = 0;
  int total = 0;
  for (const JobBlock &block : jobs_) {
    switch (block.info.state) {
      case OrganizeQueue::JobState::Running:
        ++running;
        done += block.info.done + block.info.failed;
        total += block.info.total;
        break;
      case OrganizeQueue::JobState::Preparing:
      case OrganizeQueue::JobState::Queued:
        ++waiting;
        break;
      default:
        break;
    }
  }

  if (jobs_.isEmpty()) return tr("No device syncs. Copy songs to a device and they queue up here.");
  if (running == 0 && waiting == 0) return tr("All syncs finished.");

  QString summary;
  if (running > 0) {
    summary = tr("Syncing %1 of %2 songs").arg(done).arg(total);
  }
  if (waiting > 0) {
    if (!summary.isEmpty()) summary += u" · "_s;
    summary += tr("%n job(s) waiting", nullptr, waiting);
  }
  return summary;

}

int DeviceSyncProgressModel::FindSong(const JobBlock &block, const Song &song) const {

  const QString url = song.url().toString();
  for (int i = 0; i < block.songs.count(); ++i) {
    if (block.songs[i].url == url && !IsFinalStatus(block.songs[i].status)) return i;
  }
  return -1;

}

void DeviceSyncProgressModel::SongChanged(const int position, const int song) {

  const QModelIndex idx = index(JobFirstRow(position) + 1 + song);
  Q_EMIT dataChanged(idx, idx, { Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole, Role_Status });
  Q_EMIT SongRowChanged(idx);

}

void DeviceSyncProgressModel::JobAdded(const quint64 id, const SongList &songs) {

  if (!queue_) return;

  JobBlock block;
  block.info = queue_->job(id);
  block.songs = SongRows(songs);

  const int first = rowCount();
  beginInsertRows(QModelIndex(), first, first + static_cast<int>(block.songs.count()));
  jobs_ << block;
  endInsertRows();

  Q_EMIT SummaryChanged();

}

void DeviceSyncProgressModel::JobSongsChanged(const quint64 id, const SongList &songs) {

  const int position = JobPosition(id);
  if (position < 0) return;

  beginResetModel();
  jobs_[position].songs = SongRows(songs);
  if (queue_) jobs_[position].info = queue_->job(id);
  endResetModel();

  Q_EMIT SummaryChanged();

}

void DeviceSyncProgressModel::JobChanged(const quint64 id) {

  const int position = JobPosition(id);
  if (position < 0 || !queue_) return;

  JobBlock &block = jobs_[position];
  block.info = queue_->job(id);

  // Songs which weren't copied by a job that ended were cancelled.
  if (OrganizeQueue::IsFinalState(block.info.state)) {
    for (int i = 0; i < block.songs.count(); ++i) {
      if (!IsFinalStatus(block.songs[i].status)) {
        block.songs[i].status = Status::Skipped;
        block.songs[i].retry_attempt = 0;
        SongChanged(position, i);
      }
    }
  }

  const QModelIndex idx = index(JobFirstRow(position));
  Q_EMIT dataChanged(idx, idx);
  Q_EMIT SummaryChanged();

}

void DeviceSyncProgressModel::JobRemoved(const quint64 id) {

  const int position = JobPosition(id);
  if (position < 0) return;

  const int first = JobFirstRow(position);
  beginRemoveRows(QModelIndex(), first, first + static_cast<int>(jobs_[position].songs.count()));
  jobs_.removeAt(position);
  endRemoveRows();

  Q_EMIT SummaryChanged();

}

void DeviceSyncProgressModel::SongStarted(const quint64 id, const Song &song) {

  const int position = JobPosition(id);
  if (position < 0) return;
  const int i = FindSong(jobs_[position], song);
  if (i < 0) return;

  SongRow &row = jobs_[position].songs[i];
  if (row.status == Status::InProgress) return;
  row.status = Status::InProgress;
  SongChanged(position, i);

}

void DeviceSyncProgressModel::SongFinished(const quint64 id, const Song &song, const bool success) {

  const int position = JobPosition(id);
  if (position < 0) return;
  const int i = FindSong(jobs_[position], song);
  if (i < 0) {
    qLog(Warning) << "Sync queue: no pending row for finished song" << song.url();
    return;
  }

  SongRow &row = jobs_[position].songs[i];
  row.status = success ? Status::Done : Status::Failed;
  row.retry_attempt = 0;
  row.retry_max = 0;
  row.retry_delay_ms = 0;
  SongChanged(position, i);

}

void DeviceSyncProgressModel::SongRetry(const quint64 id, const Song &song, const int attempt, const int max_attempts, const qint64 delay_ms) {

  const int position = JobPosition(id);
  if (position < 0) return;
  const int i = FindSong(jobs_[position], song);
  if (i < 0) return;

  SongRow &row = jobs_[position].songs[i];
  row.status = Status::Retrying;
  row.retry_attempt = attempt;
  row.retry_max = max_attempts;
  row.retry_delay_ms = delay_ms;
  SongChanged(position, i);

}
