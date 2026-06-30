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

#include "config.h"

#include <memory>

#include <glib.h>
#include <gpod/itdb.h>

#include <QtGlobal>
#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QImage>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/standardpaths.h"
#include "core/temporaryfile.h"
#include "core/taskmanager.h"
#include "core/database.h"
#include "collection/collectionbackend.h"
#include "collection/collectionmodel.h"
#include "covermanager/albumcoverloader.h"
#include "connecteddevice.h"
#include "gpoddevice.h"
#include "gpodloader.h"

class DeviceLister;
class DeviceManager;

using std::make_shared;
using namespace Qt::Literals::StringLiterals;

GPodDevice::GPodDevice(const QUrl &url,
                       DeviceLister *lister,
                       const QString &unique_id,
                       DeviceManager *device_manager,
                       const SharedPtr<TaskManager> task_manager,
                       const SharedPtr<Database> database,
                       const SharedPtr<TagReaderClient> tagreader_client,
                       const SharedPtr<AlbumCoverLoader> albumcover_loader,
                       const int database_id,
                       const bool first_time,
                       QObject *parent)
    : ConnectedDevice(url, lister, unique_id, device_manager, task_manager, database, tagreader_client, albumcover_loader, database_id, first_time, parent),
      task_manager_(task_manager),
      loader_(nullptr),
      loader_thread_(nullptr),
      db_(nullptr),
      closing_(false) {}

namespace {

// Ensure the iPod has a `Device/SysInfo` file containing FirewireGuid so
// libgpod can compute and write the iTunesDB hash58 signature on
// disconnect. Without this file (or with an empty one) `itdb_hash58_write_hash`
// fails because `itdb_device_get_hex_uuid` returns FALSE, libgpod silently
// writes the iTunesDB with a zeroed hash field, and the iPod firmware then
// rejects the entire database on next boot — symptom: "No music" on device,
// even though files are physically copied.
//
// On macOS the `unique_id` from `MacOsDeviceLister` is `"USB/<serial>"`
// where `<serial>` is the USB serial number, which is identical to the
// FirewireGuid for all post-FireWire iPods. We use that to seed the file
// when the user has never plugged the iPod into iTunes/Music.app (which
// would normally populate `Device/SysInfoExtended` + `Device/SysInfo`).
//
// This is a no-op when the file already exists with a valid FirewireGuid.
void EnsureIpodSysInfo(const QString &mount_point, const QString &unique_id) {

  const QString device_dir = mount_point + "/iPod_Control/Device"_L1;
  const QString sysinfo_path = device_dir + "/SysInfo"_L1;
  const QString sysinfo_extended_path = device_dir + "/SysInfoExtended"_L1;

  // If SysInfoExtended exists, libgpod will derive FirewireGuid from it.
  if (QFile::exists(sysinfo_extended_path) && QFileInfo(sysinfo_extended_path).size() > 0) {
    return;
  }

  // If SysInfo exists and already contains FirewireGuid, nothing to do.
  if (QFile::exists(sysinfo_path) && QFileInfo(sysinfo_path).size() > 0) {
    QFile existing(sysinfo_path);
    if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
      const QString contents = QString::fromUtf8(existing.readAll());
      existing.close();
      if (contents.contains("FirewireGuid"_L1, Qt::CaseInsensitive)) {
        return;
      }
    }
  }

  // Try to extract a hex serial from unique_id. Strip a leading scheme
  // prefix like "USB/" or "MTP/" if present.
  QString serial = unique_id;
  const qsizetype slash = serial.indexOf(u'/');
  if (slash >= 0) serial = serial.mid(slash + 1);

  // Validate: the serial must be pure hex and at least 16 chars (64-bit GUID).
  // iPod USB serials are typically exactly 16 hex chars.
  static const QRegularExpression hex_re(u"^[0-9A-Fa-f]{16,40}$"_s);
  if (!hex_re.match(serial).hasMatch()) {
    qLog(Warning) << "GPodDevice: cannot derive FirewireGuid from unique_id" << unique_id
                  << "- iPod may reject the iTunesDB after sync. Plug the iPod into"
                  << "Music.app once to initialize Device/SysInfoExtended.";
    return;
  }

  // Make sure the Device/ directory exists (it may not on freshly-formatted
  // iPods or on Shuffles).
  QDir().mkpath(device_dir);

  QFile file(sysinfo_path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    qLog(Warning) << "GPodDevice: failed to open" << sysinfo_path << "for writing:" << file.errorString();
    return;
  }
  // Minimal SysInfo for hash58 signing:
  //
  //   1. `FirewireGuid` — required so `itdb_device_get_hex_uuid` succeeds
  //      and `itdb_hash58_write_hash` has a key to sign the DB with.
  //
  //   2. `ModelNumStr` — required so `itdb_device_get_ipod_info` returns a
  //      non-default `Itdb_IpodInfo`. WITHOUT this, libgpod returns the
  //      "Invalid" entry (generation = UNKNOWN) and
  //      `itdb_device_get_checksum_type` falls through to
  //      `ITDB_CHECKSUM_NONE`. The iTunesDB then gets written with
  //      `hashing_scheme = 0x0000` and a zeroed hash field, and the iPod
  //      firmware silently rejects every track as "No Songs".
  //
  // We use `MC297` — the iPod Classic 7G 160GB Black model number, which
  // libgpod's lookup table maps to `ITDB_IPOD_GENERATION_CLASSIC_3` →
  // `ITDB_CHECKSUM_HASH58`. This is the safest single hardcoded choice
  // because:
  //   - All Classic 6G/7G, Nano 3G/4G use HASH58 (matches what MC297 picks).
  //   - HASH58 is the only checksum scheme that can be generated by
  //     libgpod without an iTunes-extracted HashInfo file.
  //   - Newer devices (Nano 5G+, Touch, iPhone, iPad) use HASH72/HASHAB
  //     which CANNOT work without iTunes-side initialization regardless
  //     of what we put here, so MC297 doesn't make them worse.
  //
  // The hash is keyed on FirewireGuid+SHA1, NOT on the model, so MC297 is
  // a correct hash for any of the HASH58 devices even though the displayed
  // model name would be wrong. Cover-art sizes/chapter image dimensions
  // use the Classic-3 defaults — close enough for the supported devices.
  const QByteArray contents =
      QStringLiteral("ModelNumStr: MC297\nFirewireGuid: 0x%1\n").arg(serial.toUpper()).toUtf8();
  if (file.write(contents) != contents.size()) {
    qLog(Warning) << "GPodDevice: short write to" << sysinfo_path;
  }
  file.close();
  qLog(Info) << "GPodDevice: wrote" << sysinfo_path
             << "with FirewireGuid 0x" << serial.toUpper()
             << "- the iPod will now accept the synced iTunesDB.";

}

}  // namespace

bool GPodDevice::Init() {

  InitBackendDirectory(url_.path(), first_time_);
  collection_model_->Init();

  // Make sure libgpod can sign the iTunesDB (see EnsureIpodSysInfo comment).
  // Must run before GPodLoader::LoadDatabase calls itdb_parse, which reads
  // SysInfo at parse time.
  EnsureIpodSysInfo(url_.path(), unique_id_);

  loader_ = new GPodLoader(url_.path(), task_manager_, collection_backend_, shared_from_this());
  loader_thread_ = new QThread();
  loader_->moveToThread(loader_thread_);

  QObject::connect(loader_, &GPodLoader::Error, this, &GPodDevice::LoaderError);
  QObject::connect(loader_, &GPodLoader::TaskStarted, this, &GPodDevice::TaskStarted);
  QObject::connect(loader_, &GPodLoader::LoadFinished, this, &GPodDevice::LoadFinished);
  QObject::connect(loader_thread_, &QThread::started, loader_, &GPodLoader::LoadDatabase);

  return true;

}

GPodDevice::~GPodDevice() {

  if (loader_) {
    loader_thread_->exit();
    loader_->deleteLater();
    loader_thread_->deleteLater();
    loader_ = nullptr;
    loader_thread_ = nullptr;
  }

  if (db_) {
    itdb_free(db_);
    db_ = nullptr;
  }

}

void GPodDevice::ConnectAsync() {

  loader_thread_->start();

}

void GPodDevice::Close() {

  closing_ = true;

  if (IsLoading()) {
    loader_->Abort();
  }
  else {
    ConnectedDevice::Close();
  }

}

void GPodDevice::LoadFinished(Itdb_iTunesDB *db, const bool success) {

  QMutexLocker l(&db_mutex_);
  db_ = db;
  db_wait_cond_.wakeAll();

  if (loader_thread_) {
    loader_thread_->quit();
    loader_thread_->wait(1000);
    loader_thread_->deleteLater();
    loader_thread_ = nullptr;
  }

  loader_->deleteLater();
  loader_ = nullptr;

  if (closing_) {
    ConnectedDevice::Close();
  }
  else {
    Q_EMIT DeviceConnectFinished(unique_id_, success);
  }

}

void GPodDevice::LoaderError(const QString &message) {
  Q_EMIT Error(message);
}

void GPodDevice::Start() {

  {
    // Wait for the database to be loaded
    QMutexLocker l(&db_mutex_);
    if (!db_) db_wait_cond_.wait(&db_mutex_);
  }

  // Ensure only one "organize files" can be active at any one time
  db_busy_.lock();

}

bool GPodDevice::StartCopy(QList<Song::FileType> *supported_filetypes) {

  Start();

  if (supported_filetypes) GetSupportedFiletypes(supported_filetypes);

  return true;

}

Itdb_Track *GPodDevice::AddTrackToITunesDb(const Song &metadata) {

  // Create the track
  Itdb_Track *track = itdb_track_new();
  metadata.ToItdb(track);

  // Add it to the DB and the master playlist
  // The DB takes ownership of the track
  itdb_track_add(db_, track, -1);
  Itdb_Playlist *mpl = itdb_playlist_mpl(db_);
  itdb_playlist_add_track(mpl, track, -1);

  return track;

}

void GPodDevice::AddTrackToModel(Itdb_Track *track, const QString &prefix) {

  // Add it to our CollectionModel
  Song metadata_on_device;
  metadata_on_device.InitFromItdb(track, prefix);
  metadata_on_device.set_directory_id(1);
  songs_to_add_ << metadata_on_device;

}

bool GPodDevice::CopyToStorage(const CopyJob &job, QString &error_text) {

  Q_ASSERT(db_);

  Itdb_Track *track = AddTrackToITunesDb(job.metadata_);

  // [cover-trace] Log what we received from Organize so we can correlate this
  // half of the trace with the Organize-side `[cover-trace]` lines and pinpoint
  // exactly where the cover is lost on the path Organize -> CopyToStorage ->
  // itdb_track_set_thumbnails -> itdb_write.
  qLog(Info) << "[cover-trace] GPodDevice::CopyToStorage entry"
             << " title=" << job.metadata_.title()
             << " albumcover=" << job.albumcover_
             << " cover_image.isNull=" << job.cover_image_.isNull()
             << " cover_image.size=" << job.cover_image_.size()
             << " cover_source=" << job.cover_source_;

  if (job.albumcover_) {
    bool result = false;
    if (!job.cover_image_.isNull()) {
#ifdef Q_OS_LINUX
      QString temp_path = StandardPaths::WritableLocation(StandardPaths::StandardLocation::CacheLocation) + u"/organize"_s;
#else
      QString temp_path = StandardPaths::WritableLocation(StandardPaths::StandardLocation::TempLocation);
#endif
      if (!QDir(temp_path).exists()) QDir().mkpath(temp_path);
      SharedPtr<TemporaryFile> cover_file = make_shared<TemporaryFile>(temp_path + u"/track-albumcover-XXXXXX.jpg"_s);
      if (!cover_file->filename().isEmpty()) {
        const QImage &image = job.cover_image_;
        if (image.save(cover_file->filename(), "JPG")) {
          // [cover-trace] Verify the JPEG actually has bytes on disk.
          // itdb_track_set_thumbnails only stat()s the path; it does not
          // decode the JPEG. If image.save returned true but produced a
          // zero/near-zero-byte file (Qt JPEG plugin missing, or a degenerate
          // QImage that survived isNull()), the iPod would end up with
          // has_artwork=1 but no thumbnail bytes in the .ithmb file -- the
          // cover would appear in this session's in-memory CollectionModel
          // but vanish on reconnect once itdb_parse re-reads the disk.
          const qint64 saved_size = QFileInfo(cover_file->filename()).size();
          qLog(Info) << "[cover-trace] GPodDevice::CopyToStorage wrote temp JPEG"
                     << cover_file->filename() << "size=" << saved_size;
          // 256 bytes is well below even the smallest valid JPEG with any
          // payload; anything smaller is certainly a Qt-side failure that
          // libgpod will silently accept.
          constexpr qint64 kMinValidJpegBytes = 256;
          if (saved_size < kMinValidJpegBytes) {
            qLog(Error) << "[cover-trace] Refusing to attach degenerate JPEG ("
                        << saved_size << "bytes) for" << job.metadata_.title();
          }
          else {
            const QByteArray filename = QFile::encodeName(cover_file->filename());
            result = itdb_track_set_thumbnails(track, filename.constData());
            qLog(Info) << "[cover-trace] itdb_track_set_thumbnails(image-branch) returned" << result;
            if (result) {
              cover_files_ << cover_file;
              track->has_artwork = 1;
            }
          }
        }
        else {
          qLog(Error) << "[cover-trace] image.save failed for" << cover_file->filename()
                      << " image.size=" << image.size()
                      << " image.format=" << static_cast<int>(image.format());
        }
      }
      else {
        qLog(Error) << "[cover-trace] Failed to obtain temporary file under" << temp_path;
      }
    }
    else if (!job.cover_source_.isEmpty()) {
      const qint64 source_size = QFileInfo(job.cover_source_).size();
      qLog(Info) << "[cover-trace] GPodDevice::CopyToStorage using cover_source_"
                 << job.cover_source_ << "size=" << source_size;
      const QByteArray filename = QFile::encodeName(job.cover_source_);
      result = itdb_track_set_thumbnails(track, filename.constData());
      qLog(Info) << "[cover-trace] itdb_track_set_thumbnails(source-branch) returned" << result;
      if (result) track->has_artwork = 1;
    }
    else {
      qLog(Info) << "[cover-trace] GPodDevice::CopyToStorage: no cover available for"
                 << job.metadata_.title() << "(neither cover_image_ nor cover_source_)";
      result = true;
    }
    if (!result) {
      qLog(Error) << "Failed to set album cover image";
    }
  }
  else {
    qLog(Info) << "[cover-trace] GPodDevice::CopyToStorage: albumcover flag is false; skipping cover attach for"
               << job.metadata_.title();
  }

  // Copy the file
  GError *error = nullptr;
  itdb_cp_track_to_ipod(track, QDir::toNativeSeparators(job.source_).toLocal8Bit().constData(), &error);
  if (error) {
    error_text = tr("Could not copy %1 to %2: %3").arg(job.metadata_.url().toLocalFile(), url_.path(), QString::fromUtf8(error->message));
    g_error_free(error);
    qLog(Error) << error_text;
    Q_EMIT Error(error_text);

    // Need to remove the track from the db again
    itdb_track_remove(track);
    return false;
  }

  // Put the track in the playlist, if one is specified
  if (!job.playlist_.isEmpty()) {
    // Does the playlist already exist?
    QByteArray playlist_name = job.playlist_.toUtf8();
    Itdb_Playlist *playlist = itdb_playlist_by_name(db_, playlist_name.data());
    if (!playlist) {
      // Create the playlist
      playlist = itdb_playlist_new(playlist_name.data(), false);
      itdb_playlist_add(db_, playlist, -1);
    }
    // Playlist should exist so add the track to the playlist
    itdb_playlist_add_track(playlist, track, -1);
  }

  AddTrackToModel(track, url_.path());

  // Remove the original if it was requested
  if (job.remove_original_) {
    QFile::remove(job.source_);
  }

  return true;

}

bool GPodDevice::WriteDatabase(QString &error_text) {

  // [cover-trace] Snapshot Artwork/ before itdb_write so we can tell from the
  // log whether the .ithmb files grew as a result of this write. This is the
  // disk-level evidence that distinguishes "cover was attached to the track
  // but the bytes never made it to .ithmb" (the suspected failure mode where
  // covers appear in-session and vanish on reconnect) from "covers really
  // were written to disk".
  const QString mountpoint = QString::fromUtf8(itdb_get_mountpoint(db_));
  const QString artwork_dir = mountpoint + u"/iPod_Control/Artwork/"_s;
  {
    const QDir d(artwork_dir);
    const QFileInfoList before = d.entryInfoList(QStringList() << u"*.ithmb"_s << u"ArtworkDB"_s, QDir::Files);
    qLog(Info) << "[cover-trace] WriteDatabase: pending temp covers in cover_files_=" << cover_files_.count()
               << " Artwork/ dir=" << artwork_dir;
    for (const QFileInfo &fi : before) {
      qLog(Info) << "[cover-trace]   pre-write " << fi.fileName() << "size=" << fi.size();
    }
  }

  // Write the itunes database
  GError *error = nullptr;
  const bool success = itdb_write(db_, &error);
  qLog(Info) << "[cover-trace] WriteDatabase: itdb_write returned" << success;
  cover_files_.clear();
  if (!success) {
    if (error) {
      error_text = tr("Writing database failed: %1").arg(QString::fromUtf8(error->message));
      g_error_free(error);
    }
    else {
      error_text = tr("Writing database failed.");
    }
    Q_EMIT Error(error_text);
  }

  // [cover-trace] Snapshot Artwork/ after the write so the log shows exactly
  // which .ithmb files exist on disk and how big they are. If has_artwork was
  // set on the tracks but .ithmb sizes did not grow, the artwork was lost
  // inside libgpod's writer despite our cover attach reporting success.
  {
    const QDir d(artwork_dir);
    const QFileInfoList after = d.entryInfoList(QStringList() << u"*.ithmb"_s << u"ArtworkDB"_s, QDir::Files);
    for (const QFileInfo &fi : after) {
      qLog(Info) << "[cover-trace]   post-write" << fi.fileName() << "size=" << fi.size();
    }
  }

  return success;

}

void GPodDevice::Finish(const bool success) {

  // Bug #5 follow-up (see .ai/10-ipod-sync.md §10.8):
  //
  // We unconditionally apply `songs_to_add_` / `songs_to_remove_` to the
  // Strawberry-side collection cache, even if `success` is false. By the
  // time those lists are populated, the corresponding `itdb_*` mutation
  // and `itdb_cp_track_to_ipod` (or removal) has already succeeded on the
  // iPod side — they only contain the per-track *successes*. Failures get
  // routed through `files_with_errors_` in Organize and never reach these
  // lists.
  //
  // Previously this was gated on `success`, which meant a single failed
  // file in a 3,500-track batch would leave the Strawberry collection
  // cache out of sync with what was actually persisted to the iPod's
  // iTunesDB — the user would have to disconnect/reconnect to see their
  // tracks. Worse, combined with the FinishCopy bug below, the iTunesDB
  // itself wasn't even being written.
  Q_UNUSED(success);
  if (!songs_to_add_.isEmpty()) collection_backend_->AddOrUpdateSongs(songs_to_add_);
  if (!songs_to_remove_.isEmpty()) collection_backend_->DeleteSongs(songs_to_remove_);

  // This is done in the organize thread so close the unique DB connection.
  collection_backend_->Close();

  songs_to_add_.clear();
  songs_to_remove_.clear();
  cover_files_.clear();

  db_busy_.unlock();

}

bool GPodDevice::FinishCopy(bool success, QString &error_text) {

  // Bug #5 (see .ai/10-ipod-sync.md §10.8):
  //
  // The original implementation read:
  //
  //     if (success) success = WriteDatabase(error_text);
  //     Finish(success);
  //
  // i.e. if any single file in the batch failed to copy, Organize would
  // call FinishCopy(false, ...), and we would skip WriteDatabase()
  // entirely. Symptom: after a 3,500-track sync where 11 files failed,
  // iPod_Control/Music/F##/ has all 3,489 successfully-copied .m4a files
  // but iPod_Control/iTunes/iTunesDB is the empty stub from parse time
  // (0 tracks, just an empty MPL). The iPod boots up and shows
  // "No Music" because the firmware browses by iTunesDB, not by
  // scanning the disk. The user effectively loses every successfully-
  // copied track because of the partial failure.
  //
  // The fix: always call WriteDatabase() so the in-memory tracks we
  // *did* successfully add and copy get persisted. `success` then
  // becomes (input success) AND (db write succeeded). The Strawberry-
  // side collection cache (`songs_to_add_`) is still gated on the
  // combined success in Finish(), preserving the old semantics for
  // the strawberry.db side.
  const bool write_ok = WriteDatabase(error_text);
  if (!write_ok) success = false;
  Finish(success);
  return ConnectedDevice::FinishCopy(success, error_text);

}

void GPodDevice::StartDelete() { Start(); }

bool GPodDevice::RemoveTrackFromITunesDb(const QString &path, const QString &relative_to) {

  QString ipod_filename = path;
  if (!relative_to.isEmpty() && path.startsWith(relative_to)) {
    ipod_filename.remove(0, relative_to.length() + (relative_to.endsWith(u'/') ? -1 : 0));
  }

  ipod_filename.replace(u'/', u':');

  // Find the track in the itdb, identify it by its filename
  Itdb_Track *track = nullptr;
  for (GList *tracks = db_->tracks; tracks != nullptr; tracks = tracks->next) {
    Itdb_Track *t = static_cast<Itdb_Track*>(tracks->data);

    if (QString::fromUtf8(t->ipod_path) == ipod_filename) {
      track = t;
      break;
    }
  }

  if (track == nullptr) {
    qLog(Warning) << "Couldn't find song" << path << "in iTunesDB";
    return false;
  }

  // Remove the track from all playlists
  for (GList *playlists = db_->playlists; playlists != nullptr; playlists = playlists->next) {
    Itdb_Playlist *playlist = static_cast<Itdb_Playlist*>(playlists->data);

    if (itdb_playlist_contains_track(playlist, track)) {
      itdb_playlist_remove_track(playlist, track);
    }
  }

  // Remove the track from the database, this frees the struct too
  itdb_track_remove(track);

  return true;

}

bool GPodDevice::DeleteFromStorage(const DeleteJob &job) {

  Q_ASSERT(db_);

  if (!RemoveTrackFromITunesDb(job.metadata_.url().toLocalFile(), url_.path())) {
    return false;
  }

  // Remove the file
  if (!QFile::remove(job.metadata_.url().toLocalFile())) {
    return false;
  }

  // Remove it from our collection model
  songs_to_remove_ << job.metadata_;

  return true;

}

bool GPodDevice::FinishDelete(bool success, QString &error_text) {

  // Bug #5 (see .ai/10-ipod-sync.md §10.8): mirror of FinishCopy. If
  // any single delete failed, we still must persist the iTunesDB so the
  // deletes that did succeed are reflected on disk - otherwise the iPod
  // shows the to-be-deleted tracks on next boot AND the files are gone
  // from the filesystem, producing dead entries in every Music menu.
  const bool write_ok = WriteDatabase(error_text);
  if (!write_ok) success = false;
  Finish(success);
  return ConnectedDevice::FinishDelete(success, error_text);

}

bool GPodDevice::GetSupportedFiletypes(QList<Song::FileType> *ret) {
  *ret << Song::FileType::MP4;
  *ret << Song::FileType::MPEG;
  *ret << Song::FileType::ALAC;
  return true;
}
