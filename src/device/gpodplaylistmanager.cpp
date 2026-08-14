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

#include "gpodplaylistmanager.h"

#include <glib.h>
#include <gpod/itdb.h>

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>

#include "core/logging.h"

GPodPlaylistManager::GPodPlaylistManager(Itdb_iTunesDB *db, QMutex *db_mutex, QObject *parent)
    : QObject(parent), db_(db), db_mutex_(db_mutex) {}

void GPodPlaylistManager::SetDatabase(Itdb_iTunesDB *db) {
  QMutexLocker locker(db_mutex_);
  db_ = db;
  dirty_ = false;
}

// -----------------------------------------------------------------------------
// Helpers (must be called with db_mutex_ held).
// -----------------------------------------------------------------------------

Itdb_Playlist *GPodPlaylistManager::FindPlaylist(quint64 playlist_id) const {
  if (!db_) return nullptr;
  for (GList *node = db_->playlists; node != nullptr; node = node->next) {
    Itdb_Playlist *pl = static_cast<Itdb_Playlist*>(node->data);
    if (pl && static_cast<quint64>(pl->id) == playlist_id) return pl;
  }
  return nullptr;
}

Itdb_Track *GPodPlaylistManager::FindTrack(quint64 track_dbid) const {
  if (!db_) return nullptr;
  for (GList *node = db_->tracks; node != nullptr; node = node->next) {
    Itdb_Track *t = static_cast<Itdb_Track*>(node->data);
    if (t && static_cast<quint64>(t->dbid) == track_dbid) return t;
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// Read
// -----------------------------------------------------------------------------

QList<GPodPlaylistManager::PlaylistInfo> GPodPlaylistManager::ListPlaylists() const {
  QMutexLocker locker(db_mutex_);
  QList<PlaylistInfo> out;
  if (!db_) return out;
  for (GList *node = db_->playlists; node != nullptr; node = node->next) {
    Itdb_Playlist *pl = static_cast<Itdb_Playlist*>(node->data);
    if (!pl) continue;
    PlaylistInfo info;
    info.id = static_cast<quint64>(pl->id);
    info.name = QString::fromUtf8(pl->name ? pl->name : "");
    info.is_master = itdb_playlist_is_mpl(pl);
    info.is_podcasts = itdb_playlist_is_podcasts(pl);
    info.is_smart = (pl->is_spl != 0);
    info.track_count = static_cast<int>(g_list_length(pl->members));
    info.sort_order = pl->sortorder;
    out.append(info);
  }
  return out;
}

QList<GPodPlaylistManager::TrackInfo> GPodPlaylistManager::GetTracks(quint64 playlist_id) const {
  QMutexLocker locker(db_mutex_);
  QList<TrackInfo> out;
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return out;
  int nr = 0;
  for (GList *node = pl->members; node != nullptr; node = node->next) {
    Itdb_Track *t = static_cast<Itdb_Track*>(node->data);
    if (!t) continue;
    ++nr;
    TrackInfo ti;
    ti.dbid = static_cast<quint64>(t->dbid);
    ti.title = QString::fromUtf8(t->title ? t->title : "");
    ti.artist = QString::fromUtf8(t->artist ? t->artist : "");
    ti.album = QString::fromUtf8(t->album ? t->album : "");
    ti.albumartist = QString::fromUtf8(t->albumartist ? t->albumartist : "");
    ti.track_nr = t->track_nr > 0 ? t->track_nr : nr;
    ti.length_ms = t->tracklen;
    // ipod_path is ":"-separated inside the mountpoint.
    QString ipod_path = QString::fromLocal8Bit(t->ipod_path ? t->ipod_path : "");
    ipod_path.replace(QLatin1Char(':'), QLatin1Char('/'));
    ti.url = QUrl::fromLocalFile(ipod_path);
    out.append(ti);
  }
  return out;
}

quint64 GPodPlaylistManager::MasterPlaylistId() const {
  QMutexLocker locker(db_mutex_);
  if (!db_) return 0;
  Itdb_Playlist *mpl = itdb_playlist_mpl(db_);
  return mpl ? static_cast<quint64>(mpl->id) : 0;
}

// -----------------------------------------------------------------------------
// Write
// -----------------------------------------------------------------------------

quint64 GPodPlaylistManager::CreatePlaylist(const QString &name) {
  QMutexLocker locker(db_mutex_);
  if (!db_) return 0;
  const QByteArray utf8 = name.toUtf8();
  Itdb_Playlist *pl = itdb_playlist_new(utf8.constData(), /*spl=*/FALSE);
  if (!pl) {
    qLog(Warning) << "GPodPlaylistManager::CreatePlaylist: itdb_playlist_new failed";
    return 0;
  }
  // Append at end (position -1).
  itdb_playlist_add(db_, pl, -1);
  dirty_ = true;
  locker.unlock();
  Q_EMIT PlaylistsChanged();
  return static_cast<quint64>(pl->id);
}

bool GPodPlaylistManager::RenamePlaylist(quint64 playlist_id, const QString &new_name) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  if (itdb_playlist_is_mpl(pl)) {
    qLog(Info) << "GPodPlaylistManager::RenamePlaylist: refusing to rename MPL"
               << "(it's the iPod's display name and should be set via device properties)";
    return false;
  }
  g_free(pl->name);
  pl->name = g_strdup(new_name.toUtf8().constData());
  dirty_ = true;
  locker.unlock();
  Q_EMIT PlaylistsChanged();
  return true;
}

bool GPodPlaylistManager::DeletePlaylist(quint64 playlist_id) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  if (itdb_playlist_is_mpl(pl)) {
    qLog(Info) << "GPodPlaylistManager::DeletePlaylist: refusing to delete MPL";
    return false;
  }
  itdb_playlist_remove(pl);   // detach from db + free (libgpod owns the free).
  dirty_ = true;
  locker.unlock();
  Q_EMIT PlaylistsChanged();
  return true;
}

bool GPodPlaylistManager::ReorderPlaylist(quint64 playlist_id, int new_position) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  if (itdb_playlist_is_mpl(pl)) return false;
  itdb_playlist_move(pl, new_position);
  dirty_ = true;
  locker.unlock();
  Q_EMIT PlaylistsChanged();
  return true;
}

bool GPodPlaylistManager::AddTracks(quint64 playlist_id, const QList<quint64> &track_dbids) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  // Guard: adding to MPL is meaningful (that's what CopyToStorage does),
  // but from the UI we don't allow it — MPL membership follows the
  // set of on-device audio files and is maintained by the sync path.
  if (itdb_playlist_is_mpl(pl)) return false;

  int added = 0;
  for (quint64 dbid : track_dbids) {
    Itdb_Track *t = FindTrack(dbid);
    if (!t) continue;
    // Skip if the track is already in the playlist (libgpod would allow
    // dupes, but the iPod firmware collapses them and it confuses users).
    if (itdb_playlist_contains_track(pl, t)) continue;
    itdb_playlist_add_track(pl, t, -1);
    ++added;
  }
  if (added > 0) {
    dirty_ = true;
    locker.unlock();
    Q_EMIT PlaylistsChanged();
  }
  return added > 0;
}

bool GPodPlaylistManager::RemoveTracks(quint64 playlist_id, const QList<quint64> &track_dbids) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  if (itdb_playlist_is_mpl(pl)) return false;

  const QSet<quint64> wanted(track_dbids.begin(), track_dbids.end());
  int removed = 0;
  // Walk a copy of the members list because remove_track mutates it.
  QList<Itdb_Track*> to_remove;
  for (GList *node = pl->members; node != nullptr; node = node->next) {
    Itdb_Track *t = static_cast<Itdb_Track*>(node->data);
    if (t && wanted.contains(static_cast<quint64>(t->dbid))) {
      to_remove.append(t);
    }
  }
  for (Itdb_Track *t : to_remove) {
    itdb_playlist_remove_track(pl, t);
    ++removed;
  }
  if (removed > 0) {
    dirty_ = true;
    locker.unlock();
    Q_EMIT PlaylistsChanged();
  }
  return removed > 0;
}

bool GPodPlaylistManager::MoveTrack(quint64 playlist_id, quint64 track_dbid, int new_position) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  if (itdb_playlist_is_mpl(pl)) return false;
  Itdb_Track *t = FindTrack(track_dbid);
  if (!t) return false;
  if (!itdb_playlist_contains_track(pl, t)) return false;

  // libgpod has no atomic "move" — remove+add-at-position is the idiom.
  itdb_playlist_remove_track(pl, t);
  itdb_playlist_add_track(pl, t, new_position);
  dirty_ = true;
  locker.unlock();
  Q_EMIT PlaylistsChanged();
  return true;
}

bool GPodPlaylistManager::ClearPlaylist(quint64 playlist_id) {
  QMutexLocker locker(db_mutex_);
  Itdb_Playlist *pl = FindPlaylist(playlist_id);
  if (!pl) return false;
  if (itdb_playlist_is_mpl(pl)) return false;

  int removed = 0;
  while (pl->members) {
    Itdb_Track *t = static_cast<Itdb_Track*>(pl->members->data);
    itdb_playlist_remove_track(pl, t);
    ++removed;
  }
  if (removed > 0) {
    dirty_ = true;
    locker.unlock();
    Q_EMIT PlaylistsChanged();
  }
  return removed > 0;
}

QList<quint64> GPodPlaylistManager::FindTrackDbidsForUrls(const QList<QUrl> &urls) const {
  QMutexLocker locker(db_mutex_);
  QList<quint64> out;
  if (!db_ || urls.isEmpty()) return out;

  // Build a set of ":"-separated ipod_path fragments from the input URLs.
  // We match on the trailing segment of the URL path (after the mountpoint)
  // because the Song URLs carry the full local path but Itdb_Track::ipod_path
  // is only ":iPod_Control:Music:F##:xxxx.m4a".
  QSet<QString> wanted;
  for (const QUrl &u : urls) {
    if (!u.isLocalFile()) continue;
    QString path = u.toLocalFile();
    // Strip anything before "/iPod_Control/" so we can match against
    // ipod_path (which starts at ":iPod_Control:").
    const int idx = path.indexOf(QStringLiteral("/iPod_Control/"));
    if (idx >= 0) {
      path = path.mid(idx);
    }
    path.replace(QLatin1Char('/'), QLatin1Char(':'));
    wanted.insert(path);
  }
  if (wanted.isEmpty()) return out;

  for (GList *node = db_->tracks; node != nullptr; node = node->next) {
    Itdb_Track *t = static_cast<Itdb_Track*>(node->data);
    if (!t || !t->ipod_path) continue;
    const QString ipod_path = QString::fromLocal8Bit(t->ipod_path);
    if (wanted.contains(ipod_path)) {
      out.append(static_cast<quint64>(t->dbid));
    }
  }
  return out;
}