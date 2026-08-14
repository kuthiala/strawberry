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

#ifndef GPODPLAYLISTMANAGER_H
#define GPODPLAYLISTMANAGER_H

#include "config.h"

#include <gpod/itdb.h>

#include <QObject>
#include <QList>
#include <QMutex>
#include <QString>
#include <QUrl>
#include <QVector>

#include "core/song.h"

// GPodPlaylistManager — high-level, thread-safe playlist CRUD on a loaded
// Itdb_iTunesDB. Sits alongside GPodDevice but does NOT own the db pointer:
// the GPodDevice remains the single owner. The manager is instantiated by
// GPodDevice once the loader finishes and torn down in Finish().
//
// All mutating calls take the caller-supplied db_busy_ mutex from the
// owning GPodDevice before touching Itdb_iTunesDB, so playlist edits are
// serialised against copies/deletes.
//
// No call inside this class invokes itdb_write(). Persistence is the
// caller's job — the GPodDevice::WriteDatabase() throttle rules from
// Bug #8/#9/#11 (§10.12, §10.14) still apply, and the playlist dialog
// invokes WriteDatabase() exactly once when the user clicks "Save".
//
// See .ai/10-ipod-sync.md §10.2 for the libgpod mental model.
class GPodPlaylistManager : public QObject {
  Q_OBJECT

 public:
  // Immutable snapshot of one playlist for UI consumption. Every mutating
  // op returns a fresh snapshot list; the UI must not cache Itdb_Playlist*
  // pointers directly because they can be freed by DeletePlaylist().
  struct PlaylistInfo {
    quint64 id = 0;              // itdb_playlist->id (stable across writes)
    QString name;
    bool is_master = false;      // Master Playlist (MPL) — read-only
    bool is_podcasts = false;    // Podcasts pseudo-playlist
    bool is_smart = false;       // has non-null splpref
    int track_count = 0;
    int sort_order = 0;          // ITDB_PSO_MANUAL etc.
  };

  // One track as it appears inside a playlist. url is the collection URL
  // (post InitFromItdb), and dbid is the libgpod-side stable track id
  // that survives writes.
  struct TrackInfo {
    quint64 dbid = 0;
    QString title;
    QString artist;
    QString album;
    QString albumartist;
    int track_nr = 0;
    int length_ms = 0;
    QUrl url;
  };

  explicit GPodPlaylistManager(Itdb_iTunesDB *db, QMutex *db_mutex, QObject *parent = nullptr);
  ~GPodPlaylistManager() override = default;

  // Re-point at a fresh db (called by GPodDevice::LoadFinished on reconnect).
  void SetDatabase(Itdb_iTunesDB *db);

  // --- Read ---
  QList<PlaylistInfo> ListPlaylists() const;
  QList<TrackInfo> GetTracks(quint64 playlist_id) const;

  // --- Write --- return the id of the affected playlist (0 on failure).
  quint64 CreatePlaylist(const QString &name);
  bool RenamePlaylist(quint64 playlist_id, const QString &new_name);
  bool DeletePlaylist(quint64 playlist_id);
  bool ReorderPlaylist(quint64 playlist_id, int new_position);

  bool AddTracks(quint64 playlist_id, const QList<quint64> &track_dbids);
  bool RemoveTracks(quint64 playlist_id, const QList<quint64> &track_dbids);
  bool MoveTrack(quint64 playlist_id, quint64 track_dbid, int new_position);
  bool ClearPlaylist(quint64 playlist_id);

  // Convenience: locate track dbids by matching Song::url() against
  // Itdb_Track::ipod_path.
  QList<quint64> FindTrackDbidsForUrls(const QList<QUrl> &urls) const;

  // Master playlist id (for "all tracks" browsing in UI).
  quint64 MasterPlaylistId() const;

  // Whether any mutation happened since the last ResetDirty(). Used by the
  // playlist dialog to gate the Save button and warn on unsaved close.
  bool IsDirty() const { return dirty_; }
  void ResetDirty() { dirty_ = false; }

 Q_SIGNALS:
  void PlaylistsChanged();

 private:
  // All private helpers assume db_mutex_ is held.
  Itdb_Playlist *FindPlaylist(quint64 playlist_id) const;
  Itdb_Track *FindTrack(quint64 track_dbid) const;

 private:
  Itdb_iTunesDB *db_;    // NOT owned.
  QMutex *db_mutex_;     // NOT owned; shared with GPodDevice::db_busy_.
  bool dirty_ = false;
};

#endif  // GPODPLAYLISTMANAGER_H