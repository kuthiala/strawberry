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
 *
 */

#include "config.h"

#include <algorithm>

#include <QObject>
#include <QCoreApplication>
#include <QEventLoop>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QByteArray>
#include <QDateTime>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QStandardPaths>

#include "core/logging.h"
#include "core/settings.h"
#include "core/song.h"
#include "core/standardpaths.h"
#include "constants/audiobooksettings.h"
#include "tagreader/tagreaderclient.h"
#include "organize/organizedialog.h"

#include "audiobookmerger.h"

using namespace Qt::Literals::StringLiterals;

namespace {

// ffmpeg chapter TIMEBASE is 1/1000 (milliseconds). Song lengths are in ns.
constexpr qint64 kNsecPerMsec = 1000000LL;


// Stable per-book cache key so re-syncing reuses/overwrites one file.
QString BookCacheKey(const QString &album, const QString &albumartist) {
  const QByteArray data = (album + u'\x1f' + albumartist).toUtf8();
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(16));
}

QString SafeName(const QString &in) {
  QString out = in;
  out.replace(QRegularExpression(u"[/\\\\:*?\"<>|]"_s), u"-"_s);
  out = out.trimmed();
  if (out.isEmpty()) out = u"Audiobook"_s;
  return out;
}

}  // namespace

AudiobookMerger::AudiobookMerger(QObject *parent) : QObject(parent), ffmpeg_(FindFfmpeg()) {

  Settings s;
  s.beginGroup(AudiobookSettings::kSettingsGroup);
  force_reencode_ = s.value(AudiobookSettings::kForceReencode, AudiobookSettings::kForceReencodeDefault).toBool();
  aac_bitrate_ = s.value(AudiobookSettings::kAacBitrate, QLatin1String(AudiobookSettings::kAacBitrateDefault)).toString();
  s.endGroup();

  if (aac_bitrate_.trimmed().isEmpty()) aac_bitrate_ = QLatin1String(AudiobookSettings::kAacBitrateDefault);

}

QString AudiobookMerger::FindFfmpeg() {

  QString found = QStandardPaths::findExecutable(u"ffmpeg"_s);
  if (!found.isEmpty()) return found;

  const QStringList candidates = {
    u"/opt/homebrew/bin/ffmpeg"_s,
    u"/usr/local/bin/ffmpeg"_s,
    u"/usr/bin/ffmpeg"_s,
  };
  for (const QString &candidate : candidates) {
    if (QFileInfo::exists(candidate)) return candidate;
  }

  return QString();

}

bool AudiobookMerger::CanStreamCopy(const SongList &chapters) {

  // Lossless `-c copy` concat into .m4b is only valid when every input is
  // already AAC/ALAC inside an MP4 container. Anything else must be re-encoded.
  for (const Song &chapter : chapters) {
    const Song::FileType ft = chapter.filetype();
    if (ft != Song::FileType::MP4 && ft != Song::FileType::ALAC) {
      return false;
    }
  }

  return true;

}

bool AudiobookMerger::WriteMetadataFile(const SongList &chapters, const QString &metadata_path, QString &error) {

  // ffmetadata: global tags followed by one [CHAPTER] block per track.
  QFile file(metadata_path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    error = QObject::tr("Could not write audiobook metadata file %1").arg(metadata_path);
    return false;
  }

  const Song &first = chapters.first();

  auto escape = [](const QString &in) -> QString {
    QString out = in;
    out.replace(u'\\', u"\\\\"_s);
    out.replace(u'=', u"\\="_s);
    out.replace(u';', u"\\;"_s);
    out.replace(u'#', u"\\#"_s);
    out.replace(u'\n', u"\\\n"_s);
    return out;
  };

  QString text = u";FFMETADATA1\n"_s;
  text += u"title="_s + escape(first.effective_album()) + u'\n';
  text += u"album="_s + escape(first.effective_album()) + u'\n';
  text += u"artist="_s + escape(first.effective_albumartist()) + u'\n';
  text += u"album_artist="_s + escape(first.effective_albumartist()) + u'\n';
  if (!first.genre().isEmpty()) text += u"genre="_s + escape(first.genre()) + u'\n';
  text += u"media_type=2\n"_s;  // iTunes "Audiobook".

  qint64 start_ms = 0;
  for (const Song &chapter : chapters) {
    qint64 len_ms = chapter.length_nanosec() > 0 ? chapter.length_nanosec() / kNsecPerMsec : 0;
    if (len_ms <= 0) len_ms = 1;  // never emit a zero-span chapter.
    const qint64 end_ms = start_ms + len_ms;
    text += u"[CHAPTER]\n"_s;
    text += u"TIMEBASE=1/1000\n"_s;
    text += u"START="_s + QString::number(start_ms) + u'\n';
    text += u"END="_s + QString::number(end_ms) + u'\n';
    const QString chapter_title = !chapter.title().isEmpty() ? chapter.title() : QObject::tr("Chapter %1").arg(chapter.track());
    text += u"title="_s + escape(chapter_title) + u'\n';
    start_ms = end_ms;
  }

  const QByteArray bytes = text.toUtf8();
  const bool ok = file.write(bytes) == bytes.size();
  file.close();
  if (!ok) {
    error = QObject::tr("Could not write audiobook metadata file %1").arg(metadata_path);
    return false;
  }
  return true;

}

bool AudiobookMerger::MergeOneBook(const SongList &chapters, const QString &out_m4b, bool *stream_copy, QString &error) const {

  if (ffmpeg_.isEmpty()) {
    error = QObject::tr("ffmpeg not found; cannot combine audiobook chapters.");
    return false;
  }
  if (chapters.isEmpty()) {
    error = QObject::tr("No chapters to merge.");
    return false;
  }

  const QFileInfo out_info(out_m4b);
  const QString work_dir = out_info.absolutePath();
  if (!QDir(work_dir).exists()) QDir().mkpath(work_dir);

  const bool copy = !force_reencode_ && CanStreamCopy(chapters);
  if (stream_copy) *stream_copy = copy;

  // 1. Concat-demuxer listing. Each line: file '<absolute-path>' (single quotes
  //    inside the path escaped per ffmpeg's concat rules).
  const QString list_path = out_m4b + u".concat.txt"_s;
  {
    QFile list_file(list_path);
    if (!list_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
      error = QObject::tr("Could not write audiobook concat listing %1").arg(list_path);
      return false;
    }
    QString listing;
    for (const Song &chapter : chapters) {
      QString path = chapter.url().toLocalFile();
      path.replace(u'\'', u"'\\''"_s);
      listing += u"file '"_s + path + u"'\n"_s;
    }
    list_file.write(listing.toUtf8());
    list_file.close();
  }

  // 2. ffmetadata file (chapter markers + book-level tags).
  const QString metadata_path = out_m4b + u".ffmeta.txt"_s;
  if (!WriteMetadataFile(chapters, metadata_path, error)) {
    QFile::remove(list_path);
    return false;
  }

  // 3. Resolve a cover image for the book. We prefer a user-picked cover
  //    (art_manual), then a sidecar image discovered next to the files
  //    (art_automatic). Both are plain local image files we can hand to ffmpeg
  //    Both are plain local image files we can hand to ffmpeg
  //    as an extra input and embed as an MP4 cover (attached_pic). If neither is
  //    available but the first chapter carries embedded art inside its tags
  //    (art_embedded), we extract that art to a temporary file via the tag reader
  //    and use it as the cover. `temp_cover_path` is removed before we return.
  QString cover_path;
  QString temp_cover_path;
  {
    const Song &first = chapters.first();
    const QUrl manual = first.art_manual();
    const QUrl automatic = first.art_automatic();
    if (manual.isValid() && manual.isLocalFile() && QFileInfo::exists(manual.toLocalFile())) {
      cover_path = manual.toLocalFile();
    }
    else if (automatic.isValid() && automatic.isLocalFile() && QFileInfo::exists(automatic.toLocalFile())) {
      cover_path = automatic.toLocalFile();
    }
    else if (tagreader_client_ && first.art_embedded() && first.url().isValid() && first.url().isLocalFile()) {
      // Extract the embedded picture from the first chapter's tags and write it
      // to a temp file next to the output so ffmpeg can embed it as the cover.
      QByteArray cover_data;
      const TagReaderResult art_result = tagreader_client_->LoadCoverDataBlocking(first.url().toLocalFile(), cover_data);
      if (art_result.success() && !cover_data.isEmpty()) {
        temp_cover_path = out_m4b + u".cover.img"_s;
        QFile cover_file(temp_cover_path);
        if (cover_file.open(QIODevice::WriteOnly) && cover_file.write(cover_data) == cover_data.size()) {
          cover_file.close();
          cover_path = temp_cover_path;
        }
        else {
          cover_file.close();
          QFile::remove(temp_cover_path);
          temp_cover_path.clear();
          qLog(Warning) << "AudiobookMerger: failed to write embedded cover temp file" << temp_cover_path;
        }
      }
    }
  }

  // 4. ffmpeg arguments:
  //    -f concat -safe 0 -i list.txt                    : ordered chapter inputs (input 0)
  //    -i ffmeta.txt                                    : book tags + chapters (input 1)
  //    [-i cover]                                        : optional cover image (input 2)
  //    -map 0:a                                          : audio from the concat
  //    [-map 2:v -c:v:0 copy -disposition:v:0 attached_pic] : embed cover
  //    -map_metadata 1 -map_chapters 1                   : book tags + chapter markers
  //    -c:a copy | aac -b:a <bitrate>                    : lossless or re-encode audio
  //    -movflags +faststart                              : audiobook-friendly MP4
  const bool have_cover = !cover_path.isEmpty();

  QStringList args;
  args << u"-y"_s
       << u"-f"_s << u"concat"_s << u"-safe"_s << u"0"_s << u"-i"_s << list_path
       << u"-i"_s << metadata_path;
  if (have_cover) {
    args << u"-i"_s << cover_path;
  }

  // Map audio from the concat demuxer (input 0).
  args << u"-map"_s << u"0:a"_s;

  // Map the cover (input 2) as a still video stream flagged as attached picture.
  if (have_cover) {
    args << u"-map"_s << u"2:v"_s
         << u"-c:v"_s << u"mjpeg"_s
         << u"-disposition:v:0"_s << u"attached_pic"_s;
  }

  // Book-level tags and chapter markers both come from the ffmetadata input.
  args << u"-map_metadata"_s << u"1"_s
       << u"-map_chapters"_s << u"1"_s;

  if (copy) {
    args << u"-c:a"_s << u"copy"_s;
  }
  else {
    args << u"-c:a"_s << u"aac"_s << u"-b:a"_s << aac_bitrate_;
  }
  args << u"-movflags"_s << u"+faststart"_s
       << u"-f"_s << u"mp4"_s
       << out_m4b;

  qLog(Info) << "AudiobookMerger: running ffmpeg" << (copy ? "(stream copy)" : "(re-encode AAC)")
             << "for" << chapters.count() << "chapters" << (have_cover ? "with cover" : "without cover") << "->" << out_m4b;

  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(ffmpeg_, args);
  if (!process.waitForStarted(30000)) {
    error = QObject::tr("Could not start ffmpeg: %1").arg(process.errorString());
    QFile::remove(list_path);
    QFile::remove(metadata_path);
    if (!temp_cover_path.isEmpty()) QFile::remove(temp_cover_path);
    return false;
  }

  // Pump the event loop while ffmpeg runs so the caller's modal progress dialog
  // stays responsive and its Cancel button can be clicked. waitForFinished()
  // does NOT process GUI events, so we explicitly processEvents() each slice;
  // on cancel we terminate (then kill) ffmpeg and bail out. Audiobooks can be
  // long, so there is no overall timeout here other than cancellation.
  bool cancelled = false;
  while (process.state() != QProcess::NotRunning) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (process.waitForFinished(50)) break;
    if (cancel_callback_ && cancel_callback_()) {
      cancelled = true;
      process.terminate();
      if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(2000);
      }
      break;
    }
  }

  const QString output = QString::fromUtf8(process.readAll());
  QFile::remove(list_path);
  QFile::remove(metadata_path);
  if (!temp_cover_path.isEmpty()) QFile::remove(temp_cover_path);

  if (cancelled) {
    error = QObject::tr("Cancelled.");
    QFile::remove(out_m4b);
    return false;
  }

  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    error = QObject::tr("ffmpeg failed combining audiobook (%1):\n%2")
                .arg(process.exitCode())
                .arg(output.right(2000));
    QFile::remove(out_m4b);
    return false;
  }

  if (!QFileInfo::exists(out_m4b) || QFileInfo(out_m4b).size() <= 0) {
    error = QObject::tr("ffmpeg produced no output for audiobook %1").arg(out_m4b);
    return false;
  }

  return true;

}


AudiobookMerger::Result AudiobookMerger::MergeByBook(const Organize::NewSongInfoList &songs_info, const OrganizeFormat &format) {

  Result result;

  if (ffmpeg_.isEmpty()) {
    result.ok = false;
    result.errors << tr("ffmpeg was not found on this system. Install ffmpeg to combine audiobook chapters before syncing.");
    return result;
  }

  // 1. Group the selection by book. Key = (effective_album, effective_albumartist)
  //    -- the same identity the iPod uses -- so chapters the user considers "the
  //    same book" collapse together even if source tags diverge. Within a group,
  //    insertion order is preserved and later sorted by disc/track.
  QMap<QString, SongList> books;
  QStringList book_order;
  for (const Organize::NewSongInfo &info : songs_info) {
    const Song &song = info.song_;
    const QString key = song.effective_album() + u'\x1f' + song.effective_albumartist();
    if (!books.contains(key)) book_order << key;
    books[key].append(song);
  }

  const QString cache_root = StandardPaths::WritableLocation(StandardPaths::StandardLocation::CacheLocation) + u"/audiobooks"_s;
  QDir().mkpath(cache_root);

  // 2. Merge each book in turn (analyse one album, merge, move to the next).
  SongList merged_songs;
  const int total_books = book_order.count();
  int done_books = 0;
  for (const QString &key : book_order) {
    SongList chapters = books[key];

    // Order chapters by (disc, track, filename) so the book reads in sequence.
    std::stable_sort(chapters.begin(), chapters.end(), [](const Song &a, const Song &b) {
      if (a.disc() != b.disc()) return a.disc() < b.disc();
      if (a.track() != b.track()) return a.track() < b.track();
      return a.url().toLocalFile() < b.url().toLocalFile();
    });

    const Song &first = chapters.first();
    const QString album = first.effective_album();
    const QString albumartist = first.effective_albumartist();

    // Honour a cancel requested before this book starts, and announce progress.
    if (cancel_callback_ && cancel_callback_()) {
      result.cancelled = true;
      result.ok = false;
      result.log << tr("Audiobook combine cancelled by user.");
      break;
    }
    if (progress_callback_) progress_callback_(done_books, total_books, album);

    const QString book_dir = cache_root + u'/' + BookCacheKey(album, albumartist);
    QDir().mkpath(book_dir);
    const QString out_m4b = book_dir + u'/' + SafeName(album) + u".m4b"_s;

    bool stream_copy = false;
    QString error;
    if (!MergeOneBook(chapters, out_m4b, &stream_copy, error)) {
      // MergeOneBook reports "Cancelled." when the user hit Cancel while ffmpeg
      // was running; treat that as an abort of the whole operation rather than a
      // per-book failure.
      if (cancel_callback_ && cancel_callback_()) {
        result.cancelled = true;
        result.ok = false;
        result.log << tr("Audiobook combine cancelled by user.");
        break;
      }
      result.ok = false;
      result.errors << tr("Audiobook \"%1\": %2").arg(album, error);
      result.log << tr("Failed to combine audiobook \"%1\" (%2 chapters): %3").arg(album).arg(chapters.count()).arg(error);
      ++done_books;
      continue;
    }
    ++done_books;
    if (progress_callback_) progress_callback_(done_books, total_books, album);

    result.log << tr("Combined audiobook \"%1\" from %2 chapters into one file (%3).")
                      .arg(album)
                      .arg(chapters.count())
                      .arg(stream_copy ? tr("lossless, quality preserved") : tr("re-encoded to AAC %1").arg(aac_bitrate_));

    // 3. Build the synthetic single-file Song for this book. It inherits covers
    //    / genre / year from the first chapter and points at the merged .m4b. The
    //    existing Audiobook path in GPodDevice::CopyToStorage then flags it and --
    //    because it is a single file -- the iPod shows ONE audiobook titled by the
    //    book name.
    const QFileInfo out_info(out_m4b);
    Song book_song(first);
    book_song.set_id(-1);
    book_song.set_valid(true);
    book_song.set_url(QUrl::fromLocalFile(out_m4b));
    book_song.set_basefilename(out_info.fileName());
    book_song.set_filetype(Song::FileType::MP4);
    book_song.set_filesize(out_info.size());
    book_song.set_mtime(out_info.lastModified().toSecsSinceEpoch());
    book_song.set_ctime(out_info.lastModified().toSecsSinceEpoch());
    book_song.set_title(album);
    book_song.set_album(album);
    book_song.set_albumartist(albumartist);
    if (book_song.artist().isEmpty()) book_song.set_artist(albumartist);
    book_song.set_track(0);
    book_song.set_disc(0);
    book_song.set_compilation(false);

    qint64 total_ns = 0;
    for (const Song &chapter : chapters) {
      if (chapter.length_nanosec() > 0) total_ns += chapter.length_nanosec();
    }
    if (total_ns > 0) book_song.set_length_nanosec(total_ns);

    // Build the chapter marker list for this book, keyed by the merged file's
    // local path. Uses the SAME cumulative start-ms logic as WriteMetadataFile
    // so the iPod chapters line up exactly with the in-file chapter track.
    {
      MusicStorage::AudiobookChapterList chapter_list;
      chapter_list.reserve(chapters.count());
      quint64 chapter_start_ms = 0;
      for (const Song &chapter : chapters) {
        MusicStorage::AudiobookChapter marker;
        marker.start_ms_ = chapter_start_ms;
        marker.title_ = !chapter.title().isEmpty() ? chapter.title() : tr("Chapter %1").arg(chapter.track());
        chapter_list << marker;
        qint64 len_ms = chapter.length_nanosec() > 0 ? chapter.length_nanosec() / kNsecPerMsec : 0;
        if (len_ms <= 0) len_ms = 1;  // never emit a zero-span chapter.
        chapter_start_ms += static_cast<quint64>(len_ms);
      }
      result.chapters_by_file.insert(out_m4b, chapter_list);
    }

    merged_songs << book_song;
  }

  // 4. On cancel, drop any partial results so the caller syncs nothing.
  if (result.cancelled) {
    result.songs_info.clear();
    return result;
  }

  // Recompute on-device filenames via the same path the dialog uses for normal
  // songs so the destination layout is consistent.
  result.songs_info = OrganizeDialog::ComputeNewSongsFilenames(merged_songs, format, u"m4b"_s);

  return result;

}

