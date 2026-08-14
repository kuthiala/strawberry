/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
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

#ifndef GPODDEVICE_H
#define GPODDEVICE_H

#include "config.h"

#include <gpod/itdb.h>

#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "includes/shared_ptr.h"
#include "core/song.h"
#include "core/musicstorage.h"
#include "core/temporaryfile.h"
#include "connecteddevice.h"
#include "gpodloader.h"

class QThread;
class DeviceLister;
class DeviceManager;
class TaskManager;
class Database;
class TagReaderClient;
class AlbumCoverLoader;

class GPodDevice : public ConnectedDevice, public virtual MusicStorage {
  Q_OBJECT

 public:
  Q_INVOKABLE GPodDevice(const QUrl &url,
                         DeviceLister *lister,
                         const QString &unique_id,
                         DeviceManager *device_manager,
                         const SharedPtr<TaskManager> task_manager,
                         const SharedPtr<Database> database,
                         const SharedPtr<TagReaderClient> tagreader_client,
                         const SharedPtr<AlbumCoverLoader> albumcover_loader,
                         const int database_id,
                         const bool first_time,
                         QObject *parent = nullptr);

  ~GPodDevice() override;

  bool Init() override;
  void ConnectAsync() override;
  void Close() override;
  bool IsLoading() override { return loader_; }
  QObject *Loader() { return loader_; }

  static QStringList url_schemes() { return QStringList() << QStringLiteral("ipod"); }

  bool GetSupportedFiletypes(QList<Song::FileType> *ret) override;

  bool StartCopy(QList<Song::FileType> *supported_filetypes) override;
  bool CopyToStorage(const CopyJob &job, QString &error_text) override;
  // Per-song commit point. Originally this called WriteDatabase() after
  // *every* track for maximum crash-safety, but doing so triggered a
  // user-reproducible SIGSEGV inside libgpod's `write_mhsd_playlists` at
  // roughly 1,888 synced songs on macOS (KERN_INVALID_ADDRESS at offset
  // 0xc, with `ktriageinfo` showing `mach_vm_allocate_kernel failed`
  // i.e. virtual-memory exhaustion). Each `itdb_write` re-serialises the
  // full iTunesDB and (via `itdb_write_ithumb_files →
  // ithmb_rearrange_existing_thumbnails`) re-walks every existing
  // `.ithmb` slot, lseek/read/write/truncating in place. After ~1,888
  // such cycles libgpod's internal state corrupts (the cycle also
  // produced ~20% random cover misattribution as `.ithmb` slot offsets
  // got shuffled by the in-place compaction under memory pressure).
  //
  // The fix: throttle. We still want crash-safety, just not every song.
  // We call WriteDatabase() at most once per `kCommitEvery` songs and
  // once per `kCommitInterval` of wall-clock time, whichever comes
  // first. The end-of-batch `FinishCopy()` continues to flush
  // unconditionally so nothing the user actually completed is ever lost.
  // See `.ai/10-ipod-sync.md` §10.12 (Bug #8 / Bug #9) for the full
  // diagnosis and the crash log (`~/Library/Logs/DiagnosticReports/
  // strawberry-2026-06-30-140519.ips`) that drove the fix.
  bool CommitCopy(QString &error_text) override;
  bool FinishCopy(bool success, QString &error_text) override;

  // Throttle parameters for CommitCopy. Exposed as public constants so
  // tests / future tuning can reference them by name.
  //
  // kCommitEvery: number of successful `CopyToStorage` calls between
  //   forced iTunesDB writes. Picked empirically: at 1 the user crashes
  //   at 1888 songs; at 50 each batch of 50 needs ~1 `itdb_write` call
  //   (sub-second on the Classic-3 hardware) and the worst-case
  //   crash-safety window is 50 songs — small enough that re-running
  //   `Organize` after an iPod yank quickly recovers them. The
  //   user-visible "songs at risk" indicator the device pane shows is
  //   derived from this number.
  // kCommitInterval: maximum wall-clock interval between forced writes,
  //   independent of song count. Catches the case where a sync stalls
  //   on transcoding (e.g. only 3 songs in 60 s but we still want the
  //   3 of them durable in case the user yanks the iPod).
  // The first CopyToStorage in a batch always commits — see
  //   FinishCopy's reset of the counter so the next batch's first song
  //   is durable immediately even if it's the only song.
  static constexpr int kCommitEvery = 50;
  static constexpr qint64 kCommitIntervalMs = 30 * 1000;  // 30 s

  void StartDelete() override;
  bool DeleteFromStorage(const DeleteJob &job) override;
  bool FinishDelete(bool success, QString &error_text) override;

 protected Q_SLOTS:
  void LoadFinished(Itdb_iTunesDB *db, const bool success);
  void LoaderError(const QString &message);

 protected:
  Itdb_Track *AddTrackToITunesDb(const Song &metadata);
  void AddTrackToModel(Itdb_Track *track, const QString &prefix);
  bool RemoveTrackFromITunesDb(const QString &path, const QString &relative_to = QString());

 private:
  void Start();
  void Finish(const bool success);
  bool WriteDatabase(QString &error_text);

 protected:
  const SharedPtr<TaskManager> task_manager_;
  GPodLoader *loader_;
  QThread *loader_thread_;

  QWaitCondition db_wait_cond_;
  QMutex db_mutex_;
  Itdb_iTunesDB *db_;
  bool closing_;

  QMutex db_busy_;
  SongList songs_to_add_;
  SongList songs_to_remove_;
  QList<SharedPtr<TemporaryFile>> cover_files_;

  // Per-track cover-attach fingerprints (see .ai/10-ipod-sync.md §10.15).
  // Populated in CopyToStorage every time we call itdb_track_set_thumbnails:
  // the QByteArray is a hex-encoded SHA1 of the JPEG bytes we attached, plus
  // an "expected cover identity" derived from (albumartist|album). After
  // itdb_write, WriteDatabase() reads the actual on-disk .ithmb slot bytes
  // for every track that ended up with has_artwork=1 and computes a
  // SHA1-of-slot. Comparing (source-JPEG-SHA1, slot-SHA1) across tracks
  // is what lets us confirm empirically whether misattribution happens
  // client-side (wrong JPEG bytes attached at CopyToStorage time), or in
  // libgpod's ithumb-writer (right JPEG going in, wrong slot going out).
  // Cleared alongside cover_files_ in WriteDatabase() and Finish().
  struct CoverFingerprint {
    QByteArray jpeg_sha1_hex;    // hash of the exact bytes we handed to libgpod
    qint64     jpeg_size = 0;    // size of that JPEG on disk
    QString    identity;         // "albumartist|album" for cross-track dedup analysis
    QString    title;            // for log correlation
    QString    source_path;      // temp JPEG or embedded-cover source
  };
  QHash<Itdb_Track *, CoverFingerprint> cover_fingerprints_;

  // Throttle bookkeeping for CommitCopy. Reset to 0 in Start()/Finish()
  // so each batch begins with a guaranteed flush on its first song.
  //   songs_since_last_commit_  — how many CopyToStorage successes have
  //                               accumulated without WriteDatabase().
  //   last_commit_ms_           — Qt epoch ms of the last successful
  //                               WriteDatabase() (0 = never in this batch).
  // Not guarded by a mutex: all access is serialised through the
  // Organize worker thread that owns db_busy_ during a batch.
  int songs_since_last_commit_ = 0;
  qint64 last_commit_ms_ = 0;
};

#endif  // GPODDEVICE_H
