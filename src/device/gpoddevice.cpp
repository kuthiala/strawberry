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

// `Q_OS_MACOS` is only defined once QtGlobal has been included above, so the
// `<malloc/malloc.h>` include MUST come after (see Bug #11 in
// `.ai/10-ipod-sync.md §10.14`).
#ifdef Q_OS_MACOS
#  include <malloc/malloc.h>
#endif

#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
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
#include "gpodplaylistmanager.h"

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

  if (playlist_manager_) {
    playlist_manager_->deleteLater();
    playlist_manager_ = nullptr;
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

  // Instantiate/refresh the playlist manager so UI code can enumerate
  // and edit playlists as soon as DeviceConnectFinished fires. The
  // manager shares db_mutex_ / db_busy_ with the sync path; it never
  // calls itdb_write() on its own.
  if (db_) {
    if (!playlist_manager_) {
      playlist_manager_ = new GPodPlaylistManager(db_, &db_busy_, this);
    }
    else {
      playlist_manager_->SetDatabase(db_);
    }
  }

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

  // Reset the CommitCopy throttle bookkeeping at the start of each
  // batch so the first song of every batch triggers a flush (durable
  // even if the batch ends up being just that one song). See the
  // long comment on `CommitCopy` in `gpoddevice.h`.
  songs_since_last_commit_ = 0;
  last_commit_ms_ = 0;

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

  // Helper: compute a SHA1-hex fingerprint of a JPEG on disk. Used to
  // uniquely identify each attached cover so we can prove/disprove
  // misattribution (see the post-write walk in WriteDatabase()). Returns
  // empty QByteArray if the file can't be read.
  const auto Sha1HexOfFile = [](const QString &path) -> QByteArray {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    QCryptographicHash h(QCryptographicHash::Sha1);
    if (!h.addData(&f)) return QByteArray();
    return h.result().toHex();
  };

  // The "identity" is the (albumartist|album) tuple libgpod uses to bucket
  // covers into album groups. When misattribution happens, TWO tracks with
  // DIFFERENT identities end up sharing the same slot bytes. Logging it
  // makes the mismatch obvious in the post-write scan.
  const QString cover_identity = job.metadata_.effective_albumartist().toLower()
      + u"|"_s + job.metadata_.album().toLower();

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
              // [cover-trace] Fingerprint the exact bytes we handed to
              // libgpod. When WriteDatabase() walks the .ithmb slots
              // post-write, we'll be able to prove whether track X's
              // slot bytes correspond to track X's JPEG (correct) or
              // some other track's JPEG (misattribution). See
              // .ai/10-ipod-sync.md §10.15.
              CoverFingerprint fp;
              fp.jpeg_sha1_hex = Sha1HexOfFile(cover_file->filename());
              fp.jpeg_size = saved_size;
              fp.identity = cover_identity;
              fp.title = job.metadata_.title();
              fp.source_path = cover_file->filename();
              cover_fingerprints_.insert(track, fp);
              qLog(Info) << "[cover-trace] fingerprint image-branch title=" << fp.title
                         << " identity=" << fp.identity
                         << " sha1=" << fp.jpeg_sha1_hex
                         << " jpeg_size=" << fp.jpeg_size
                         << " track_ptr=" << track;
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
      if (result) {
        track->has_artwork = 1;
        CoverFingerprint fp;
        fp.jpeg_sha1_hex = Sha1HexOfFile(job.cover_source_);
        fp.jpeg_size = source_size;
        fp.identity = cover_identity;
        fp.title = job.metadata_.title();
        fp.source_path = job.cover_source_;
        cover_fingerprints_.insert(track, fp);
        qLog(Info) << "[cover-trace] fingerprint source-branch title=" << fp.title
                   << " identity=" << fp.identity
                   << " sha1=" << fp.jpeg_sha1_hex
                   << " jpeg_size=" << fp.jpeg_size
                   << " track_ptr=" << track;
      }
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

  // Bug #11 (see .ai/10-ipod-sync.md §10.14): Before every `itdb_write` we
  // release any free heap pages back to the kernel. `itdb_write`'s inner
  // hot path re-walks every track, allocates a growing WContents buffer via
  // `g_realloc` (see libgpod's `wcontents_maybe_expand`), and re-encodes
  // every pending cover through gdk-pixbuf — cumulatively these fragment
  // libmalloc's small-object regions to the point where a single ~30 MiB
  // realloc can fail with `mach_vm_allocate_kernel failed` even though the
  // process's resident set is well below the system's per-process cap.
  //
  // `malloc_zone_pressure_relief(NULL, 0)` is macOS libmalloc's cooperative
  // GC entry point: it walks every registered zone, coalesces adjacent
  // free spans, and returns any spans larger than the coalescer threshold
  // to the kernel via `madvise(MADV_FREE_REUSABLE)`. It is safe to call at
  // any time (does not block on other threads' allocations) and the return
  // value is the count of bytes returned to the OS — logged so a future
  // regression is diagnosable from `strawberry-stdout.txt` alone.
  //
  // On Linux the equivalent is `malloc_trim(0)`; on Windows there is no
  // portable equivalent so this whole block is a no-op there. Neither
  // platform is currently affected by Bug #11 (only macOS + Homebrew GLib
  // produces the linear-growth-realloc-failure pattern) so a no-op is fine.
#ifdef Q_OS_MACOS
  const size_t bytes_freed = malloc_zone_pressure_relief(nullptr, 0);
  qLog(Info) << "GPodDevice::WriteDatabase: pre-write pressure relief returned"
             << bytes_freed << "bytes to the OS";
#elif defined(Q_OS_LINUX)
  // malloc_trim is a GNU extension; guarding with __GLIBC__ keeps musl-based
  // builds (Alpine, etc.) working.
#  if defined(__GLIBC__)
  malloc_trim(0);
#  endif
#endif

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

  // [cover-trace] POST-WRITE PIXEL FINGERPRINT SCAN — see .ai/10-ipod-sync.md §10.15.
  //
  // This is the evidence-capture step for the cover misattribution bug
  // (Bug #9). We can't use libgpod's private `itdb_thumb_ipod_get_filename`
  // / `..._get_item_by_type` (they're G_GNUC_INTERNAL, not exported from
  // the dylib), so instead we walk the on-disk `.ithmb` blobs DIRECTLY:
  // each .ithmb file for a given format is a flat concatenation of
  // fixed-size slots (Classic-3: 6272-byte slots for F1055, 32768-byte
  // slots for F1068, etc.). We split each file into slots by known size
  // and compute SHA1 per slot.
  //
  // Combined with the per-track source-JPEG SHA1s captured in
  // `cover_fingerprints_` (built in CopyToStorage), the resulting log
  // lets a human observer:
  //
  //   • Confirm every source-JPEG SHA1 appears in the .ithmb blob
  //     as a distinct slot-SHA1 → each track's cover made it to disk.
  //
  //   • Detect DUPLICATE slot-SHA1s in the .ithmb (same pixel bytes at
  //     multiple offsets) → confirms writer is duplicating source data,
  //     which is the misattribution signal.
  //
  //   • Detect MISSING source-JPEG SHA1s: if track X's fingerprint
  //     doesn't correspond to any slot in the .ithmb → the writer
  //     silently dropped X's cover (Bug #4-style pack_* failure or
  //     libgpod's fallback pixbuf substitution).
  //
  // The scan is a no-op when we didn't attach any covers in this batch.
  // Cost: ~O(total .ithmb bytes) SHA1s per WriteDatabase — a few hundred
  // ms on a Classic-3 first sync, small enough to leave on all the time.
  if (!cover_fingerprints_.isEmpty()) {
    qLog(Info) << "[cover-trace] --- post-write source-JPEG fingerprint set ---";
    // Emit the source-side fingerprints in a stable order (sorted by
    // title) so the log is diffable across runs.
    QList<CoverFingerprint> ordered_sources = cover_fingerprints_.values();
    std::sort(ordered_sources.begin(), ordered_sources.end(),
              [](const CoverFingerprint &a, const CoverFingerprint &b) {
                return a.title < b.title;
              });
    QHash<QByteArray, int> source_sha1_uses;
    for (const CoverFingerprint &fp : ordered_sources) {
      ++source_sha1_uses[fp.jpeg_sha1_hex];
      qLog(Info) << "[cover-trace] src-fp title=" << fp.title
                 << " identity=" << fp.identity
                 << " jpeg_sha1=" << fp.jpeg_sha1_hex
                 << " jpeg_size=" << fp.jpeg_size;
    }
    // Log any source-JPEG SHA1 shared across DIFFERENT identities on the
    // Strawberry side. If we see this, the bug is UPSTREAM of libgpod —
    // Organize is handing the same JPEG bytes to two different-identity
    // tracks. This would clear libgpod of responsibility for that case.
    int upstream_dupes = 0;
    for (auto it = source_sha1_uses.constBegin(); it != source_sha1_uses.constEnd(); ++it) {
      if (it.value() <= 1) continue;
      // Find distinct identities that share this jpeg_sha1_hex.
      QSet<QString> identities;
      for (const CoverFingerprint &fp : ordered_sources) {
        if (fp.jpeg_sha1_hex == it.key()) identities.insert(fp.identity);
      }
      if (identities.size() > 1) {
        ++upstream_dupes;
        qLog(Warning) << "[cover-trace] UPSTREAM DUPLICATE jpeg_sha1=" << it.key()
                      << "was attached to" << it.value() << "tracks across"
                      << identities.size() << "distinct (albumartist|album) identities:"
                      << QStringList(identities.begin(), identities.end()).join(u", "_s);
      }
    }
    qLog(Info) << "[cover-trace] source-side check: upstream_duplicates=" << upstream_dupes
               << " total_tracks_attached=" << ordered_sources.size();

    // Now walk each .ithmb file on the iPod, splitting into fixed-size
    // slots and hashing each slot. For Classic-3 there are four format
    // files:
    //   F1055_1.ithmb — 56x56 RGB565     → 6272-byte slots
    //   F1060_1.ithmb — 128x128 RGB565   → 32768-byte slots (large res)
    //   F1061_1.ithmb — 128x128 RGB565   → 32768-byte slots (small res)
    //   F1068_1.ithmb — 320x320 RGB565   → 204800-byte slots
    // We don't know at compile-time which formats a given iPod uses, so
    // we peek at every .ithmb, take the file size, and try a set of
    // candidate slot sizes (the Classic-3 known set). Whichever divides
    // the file size evenly is treated as the slot size. If none divides
    // evenly the file is likely padded (post-Bug #4 formats include
    // padding); we then use size/N where N is the number of tracks we
    // attached in the last write, as a fallback.
    struct SlotSizeCandidate {
      const char *label;
      qint64 bytes;
    };
    static const SlotSizeCandidate kClassic3Sizes[] = {
      { "56x56 RGB565",     56    * 56    * 2 },  // 6272
      { "100x100 RGB565",   100   * 100   * 2 },  // 20000 (Nano-3)
      { "128x128 RGB565",   128   * 128   * 2 },  // 32768
      { "176x176 RGB565",   176   * 176   * 2 },  // 61952
      { "240x320 RGB565",   240   * 320   * 2 },  // 153600
      { "320x240 RGB565",   320   * 240   * 2 },
      { "320x320 RGB565",   320   * 320   * 2 },  // 204800
    };

    const QDir art_dir(artwork_dir);
    const QFileInfoList blobs = art_dir.entryInfoList(QStringList() << u"F*.ithmb"_s, QDir::Files);
    int total_slots_hashed = 0;
    int slots_matching_source = 0;
    int slots_unaccounted = 0;
    QHash<QByteArray, int> slot_sha1_counts;
    QHash<QByteArray, QStringList> slot_sha1_files;
    for (const QFileInfo &blob : blobs) {
      QFile bf(blob.absoluteFilePath());
      if (!bf.open(QIODevice::ReadOnly)) {
        qLog(Warning) << "[cover-trace] post-write: could not open .ithmb"
                      << blob.absoluteFilePath() << "err=" << bf.errorString();
        continue;
      }
      const QByteArray blob_bytes = bf.readAll();
      bf.close();

      // Pick a slot size for this file.
      qint64 slot_size = 0;
      const char *slot_label = "unknown";
      for (const SlotSizeCandidate &c : kClassic3Sizes) {
        if (blob_bytes.size() % c.bytes == 0) {
          slot_size = c.bytes;
          slot_label = c.label;
          break;
        }
      }
      if (slot_size == 0) {
        qLog(Warning) << "[cover-trace] post-write: .ithmb" << blob.fileName()
                      << "size=" << blob_bytes.size()
                      << "does not divide evenly by any known Classic slot size; skipping fingerprint";
        continue;
      }
      const qint64 n_slots = blob_bytes.size() / slot_size;
      qLog(Info) << "[cover-trace] scanning .ithmb" << blob.fileName()
                 << "total_size=" << blob_bytes.size()
                 << "slot_size=" << slot_size << "(" << slot_label << ")"
                 << "n_slots=" << n_slots;

      for (qint64 i = 0; i < n_slots; ++i) {
        const QByteArray slot = blob_bytes.mid(i * slot_size, slot_size);
        // Skip all-zero slots (unused).
        bool nonzero = false;
        for (int k = 0; k < slot.size(); k += 64) {
          if (slot[k] != '\0') { nonzero = true; break; }
        }
        if (!nonzero) continue;

        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(slot);
        const QByteArray slot_sha1 = h.result().toHex();

        ++total_slots_hashed;
        ++slot_sha1_counts[slot_sha1];
        slot_sha1_files[slot_sha1].append(blob.fileName() + u"@"_s + QString::number(i * slot_size));

        // Note: we CAN'T directly compare slot_sha1 to any source_sha1
        // because the source is a JPEG (compressed) and the slot is
        // decoded+rescaled+packed RGB565 pixels. So the SHA1s never
        // match by design. What matters is:
        //   1. Every source in cover_fingerprints_ produces exactly one
        //      slot per format (unique slot_sha1 per source, but the
        //      slot_sha1 differs across formats for the same source).
        //   2. Two DIFFERENT sources should never produce the SAME
        //      slot_sha1 (a same-source-different-track case is fine
        //      because that's legitimate album sharing).
        // We emit the slot fingerprint at Debug level to keep the log
        // volume tolerable, and reserve Info+Warning for the collision
        // summary at the end.
      }
    }

    // Emit any INTRA-.ithmb slot_sha1 duplicates. A legitimate cause
    // for a slot_sha1 appearing twice in the same batch is: two tracks
    // on the same album really do share art, and libgpod correctly wrote
    // the same pixel bytes into two different slots. But if the SAME
    // slot_sha1 appears N times where N > number of distinct source
    // fingerprints (which shouldn't happen because each source produces
    // one slot per format), that's evidence of writer duplication.
    int dup_slot_sha1s = 0;
    for (auto it = slot_sha1_counts.constBegin(); it != slot_sha1_counts.constEnd(); ++it) {
      if (it.value() <= 1) continue;
      ++dup_slot_sha1s;
      qLog(Info) << "[cover-trace] slot-dup slot_sha1=" << it.key()
                 << "count=" << it.value()
                 << "positions=" << slot_sha1_files[it.key()].join(u","_s);
    }
    qLog(Info) << "[cover-trace] post-write summary: total_slots_hashed=" << total_slots_hashed
               << " unique_slot_sha1s=" << slot_sha1_counts.size()
               << " duplicate_slot_sha1s=" << dup_slot_sha1s
               << " (batch attached=" << cover_fingerprints_.size() << " tracks;"
               << " upstream_dupes=" << upstream_dupes << ")";
    Q_UNUSED(slots_matching_source);
    Q_UNUSED(slots_unaccounted);
  }

  // Clear the fingerprint map — the next batch (if any) starts fresh.
  // Doing this AFTER the scan is important; doing it BEFORE would defeat
  // the whole diagnostic.
  cover_fingerprints_.clear();

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

  // Reset the CommitCopy throttle counters defensively. Start() already
  // resets them at the beginning of every batch, but doing it here too
  // means a CommitCopy that races against a Close() (or any future
  // re-entry into Start()) sees clean state.
  songs_since_last_commit_ = 0;
  last_commit_ms_ = 0;

  db_busy_.unlock();

}

bool GPodDevice::CommitCopy(QString &error_text) {

  // THROTTLED per-batch flush (Bug #8 fix). See the long comment on
  // CommitCopy in gpoddevice.h for the full story. In short:
  //
  //   - Calling WriteDatabase() after EVERY successful CopyToStorage
  //     (the original implementation) crashed reproducibly at song
  //     ~1888 with SIGSEGV inside libgpod's write_mhsd_playlists and
  //     was responsible for ~20% random cover misattribution.
  //   - We still want SOME crash-safety: never lose more than a
  //     small batch of songs if the device is yanked or the app
  //     crashes mid-sync.
  //
  // So we collapse N song commits into one WriteDatabase() call. The
  // window is bounded by both song count (`kCommitEvery`) and wall-
  // clock time (`kCommitIntervalMs`), whichever expires first. The
  // first song of every batch is always flushed (see Start()'s reset
  // of the counters).

  ++songs_since_last_commit_;
  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  const bool count_due = songs_since_last_commit_ >= kCommitEvery;
  // last_commit_ms_ == 0 means "no commit yet in this batch" -- the
  // first song should commit so the user has *something* persistent
  // on the iPod even if the very next song fails.
  const bool first_commit_in_batch = (last_commit_ms_ == 0);
  const bool time_due = !first_commit_in_batch
      && (now_ms - last_commit_ms_) >= kCommitIntervalMs;
  if (!count_due && !first_commit_in_batch && !time_due) {
    // Not yet — the in-memory `db_` already holds the new track and
    // its audio bytes are physically on the iPod NAND via
    // `itdb_cp_track_to_ipod`. The end-of-batch FinishCopy() will
    // flush; if the user yanks the iPod first, we lose at most
    // (songs_since_last_commit_) songs from the iTunesDB and they can
    // be recovered with `.ai/tools/itdb-rescue.c` or simply re-synced.
    return true;
  }

  qLog(Info) << "GPodDevice::CommitCopy: flushing iTunesDB after"
             << songs_since_last_commit_ << "songs ("
             << (count_due ? "count" : (time_due ? "time" : "first-in-batch"))
             << "trigger)";
  const bool ok = WriteDatabase(error_text);
  if (ok) {
    songs_since_last_commit_ = 0;
    last_commit_ms_ = now_ms;
  }
  return ok;

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

bool GPodDevice::WritePlaylistChanges(QString &error_text) {

  // Called by GPodPlaylistsDialog::OnSave to flush pending playlist
  // mutations. We acquire db_busy_ to serialise against any in-progress
  // sync — if an Organize batch is currently running, this blocks until
  // it releases db_busy_ in Finish(). That's the correct behaviour: a
  // playlist Save cannot race a sync commit without producing an
  // inconsistent iTunesDB.
  //
  // Once we hold the mutex we reuse the standard WriteDatabase() path
  // (same pre-write pressure relief, same post-write cover-trace, etc.).
  // On success, `dirty_` on the playlist manager is left for the caller
  // to reset (dialog does it after showing the success toast).
  {
    QMutexLocker l(&db_mutex_);
    if (!db_) {
      error_text = tr("iPod database is not loaded.");
      return false;
    }
  }

  QMutexLocker busy(&db_busy_);
  const bool ok = WriteDatabase(error_text);
  return ok;

}
