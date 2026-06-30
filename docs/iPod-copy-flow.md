# iPod copy flow on macOS

This document walks through the end-to-end path that takes a song from your
Strawberry collection and lands it onto an iPod Classic, ending with two
diagnosed bugs you can reproduce today and a sketch of the fixes.

The concrete scenario we follow throughout:

> You connect an iPod Classic over USB on macOS Sequoia, set the iPod's
> *Preferred format* to **ALAC** in Strawberry's device properties, then
> right-click a FLAC file in the collection view and choose **Copy to
> device…**. What happens, step by step?

For comparison we also follow what happens when you do the same with an
**MP3** file (which exposes both bugs).

The walkthrough cites the source so you can jump straight to it — every
mention of `name@line` refers to the in-tree file.

---

## 1. The iPod connects (mount + recognition)

### 1.1 Disk Arbitration callback

`MacOsDeviceLister` is created when Strawberry starts. In its constructor it
registers a single Disk Arbitration callback:

```cpp
// src/device/macosdevicelister.mm:164
DARegisterDiskAppearedCallback(
    loop_session_,
    kDADiskDescriptionMatchVolumeMountable,
    &DiskAddedCallback,
    reinterpret_cast<void*>(this));
```

`DARegisterDiskAppearedCallback` is part of macOS's `DiskArbitration.framework`.
It fires the callback every time the kernel finishes mounting a removable
volume — this is how Finder, Notification Center, and Strawberry all learn
that a USB drive has appeared.

When you plug the iPod in:

1. macOS enumerates the USB device, creates a BSD block node
   (`/dev/disk5s1` or similar).
2. `diskarbitrationd` mounts the HFS+/FAT32 filesystem at
   `/Volumes/<volume label>` (typically `/Volumes/IPOD`).
3. `DiskAddedCallback` is invoked with a `DADiskRef`.

### 1.2 What `DiskAddedCallback` does

```cpp
// src/device/macosdevicelister.mm:397
void MacOsDeviceLister::DiskAddedCallback(DADiskRef disk, void *context) {
  ...
  if ([[dict objectForKey:@"Removable"] intValue] == 1) {
    const QString serial = GetSerialForDevice(device.get());
    if (!serial.isEmpty()) {
      me->current_devices_[serial] = QString::fromLatin1(DADiskGetBSDName(disk));
      Q_EMIT me->DeviceAdded(serial);
    }
  }
}
```

For a removable USB device, the lister stashes the `serial → /dev/diskN`
mapping in `current_devices_` and emits `DeviceAdded(serial)`.

### 1.3 Recognising it as an iPod (the magic directory)

The lister doesn't decide "this is an iPod" by USB VID/PID. It does it by
filesystem inspection. When `DeviceManager` later asks the lister for the
URL of this serial, it ends up in:

```cpp
// src/device/devicelister.cpp:217
QUrl DeviceLister::MakeUrlFromLocalPath(const QString &path) const {
  if (IsIpod(path)) {
    QUrl ret;
    ret.setScheme(u"ipod"_s);
    ret.setPath(QDir::fromNativeSeparators(path));
    return ret;
  }
  return QUrl::fromLocalFile(path);
}

// src/device/devicelister.cpp:230
bool DeviceLister::IsIpod(const QString &path) {
  return QFile::exists(path + "/iTunes_Control"_L1) ||
         QFile::exists(path + "/iPod_Control"_L1) ||
         QFile::exists(path + "/iTunes/iTunes_Control"_L1);
}
```

An iPod Classic has `/iPod_Control/` at the root of its HFS+ partition, so
its URL becomes `ipod:///Volumes/IPOD`. A vanilla USB stick stays
`file:///Volumes/MY_STICK`.

### 1.4 DeviceManager picks `GPodDevice`

`DeviceManager` keeps a table of `URL-scheme → factory` registrations:

```cpp
// src/device/devicemanager.cpp:136-146
AddDeviceClass<CDDADevice>();
AddDeviceClass<FilesystemDevice>();
AddDeviceClass<GPodDevice>();    // registers scheme "ipod"
AddDeviceClass<MtpDevice>();
```

`AddDeviceClass<T>()` reads `T::url_schemes()` and routes every URL with that
scheme to `T`. For `GPodDevice`:

```cpp
// src/device/gpoddevice.h:76
static QStringList url_schemes() { return QStringList() << QStringLiteral("ipod"); }
```

So `ipod://` → `GPodDevice`, `file://` → `FilesystemDevice`, etc. At this
point the iPod shows up in the Devices sidebar but is **not connected** yet.

---

## 2. Connecting (loading the iTunesDB)

When you double-click the iPod (or it auto-connects), `DeviceManager`
instantiates `GPodDevice` and calls `Init()`:

```cpp
// src/device/gpoddevice.cpp:77
bool GPodDevice::Init() {
  InitBackendDirectory(url_.path(), first_time_);
  collection_model_->Init();

  loader_ = new GPodLoader(url_.path(), task_manager_, collection_backend_, shared_from_this());
  loader_thread_ = new QThread();
  loader_->moveToThread(loader_thread_);
  ...
  return true;
}
```

The heavy work happens on `loader_thread_`:

```cpp
// src/device/gpodloader.cpp (LoadDatabase, abbreviated)
GError *error = nullptr;
db_ = itdb_parse(mount_point_.toLocal8Bit().constData(), &error);
```

`itdb_parse()` is the libgpod entry point that parses every byte of the
iPod's database files:

```
<mount>/iPod_Control/iTunes/iTunesDB         ← the main binary DB
<mount>/iPod_Control/iTunes/ArtworkDB        ← cover art DB
<mount>/iPod_Control/iTunes/Photo Database   ← (if photos sync was used)
```

The result is an `Itdb_iTunesDB*` — an in-memory tree of `Itdb_Track`,
`Itdb_Playlist`, and `Itdb_Device` structs. The size of the tree scales with
the library: a 100 GB iPod with 20 000 tracks parses in ~1–3 seconds on a
modern Mac.

Once `LoadFinished` arrives back on the GUI thread:

```cpp
// src/device/gpoddevice.cpp:131-154
void GPodDevice::LoadFinished(Itdb_iTunesDB *db, const bool success) {
  QMutexLocker l(&db_mutex_);
  db_ = db;
  db_wait_cond_.wakeAll();   // any pending CopyToStorage callers wake up
  ...
  Q_EMIT DeviceConnectFinished(unique_id_, success);
}
```

The iPod's existing songs now appear in its panel inside Strawberry.

---

## 3. You right-click a track and choose "Copy to device…"

### 3.1 UI handler

In the Collection view, the context-menu entry is wired to
`CollectionView::CopyToDevice`:

```cpp
// src/collection/collectionview.cpp:747
void CollectionView::CopyToDevice() {
  if (!organize_dialog_) {
    organize_dialog_ = make_unique<OrganizeDialog>(task_manager_, tagreader_client_, nullptr, this);
  }
  organize_dialog_->SetDestinationModel(device_manager_->connected_devices_model(), true);
  organize_dialog_->SetCopy(true);
  organize_dialog_->SetSongs(GetSelectedSongs());
  organize_dialog_->show();
}
```

The same `OrganizeDialog` is used for "Organise files" inside the local
collection — for a device, "destination" is the device storage rather than a
folder.

### 3.2 Dialog → background worker

When you click *OK*:

```cpp
// src/organize/organizedialog.cpp:202
Organize *organize = new Organize(task_manager_, tagreader_client_, storage, format_,
                                  copy, ui_->overwrite->isChecked(), ui_->albumcover->isChecked(),
                                  new_songs_info_, ui_->eject_after->isChecked(), playlist_);
QObject::connect(organize, &Organize::Finished, this, &OrganizeDialog::OrganizeFinished);
organize->Start();
QDialog::accept();
```

`Organize::Start()` moves the worker onto a fresh QThread so the GUI stays
responsive. From here on, everything happens off-thread. The user sees a
progress task in the bottom-left task panel.

---

## 4. Inside the Organise loop

`Organize` is a small state machine driven by `ProcessSomeFiles`. The first
call does one-off setup; subsequent calls process up to 10 tasks per batch
and re-schedule themselves via a 100 ms `QTimer`. This is what makes the
operation cancellable mid-flight.

### 4.1 First entry: `StartCopy`

```cpp
// src/organize/organize.cpp:133
if (!started_) {
  if (!destination_->StartCopy(&supported_filetypes_)) { ... }
  started_ = true;
}
```

For an iPod this lands in:

```cpp
// src/device/gpoddevice.cpp:173
bool GPodDevice::StartCopy(QList<Song::FileType> *supported_filetypes) {
  Start();                                            // wait for db_, then grab db_busy_
  if (supported_filetypes) GetSupportedFiletypes(supported_filetypes);
  return true;
}

// src/device/gpoddevice.cpp:414
bool GPodDevice::GetSupportedFiletypes(QList<Song::FileType> *ret) {
  *ret << Song::FileType::MP4;
  *ret << Song::FileType::MPEG;
  *ret << Song::FileType::ALAC;
  return true;
}
```

So Organize's `supported_filetypes_` ends up as `{MP4, MPEG, ALAC}` — the
three audio formats an iPod Classic can play natively. Anything else will
need transcoding.

`Start()` additionally:

* **Blocks until the database is loaded** — uses the
  `db_wait_cond_` from §2.
* **Locks `db_busy_`** — a single per-device mutex that serialises copies and
  deletes. This is why you can't have two "Copy to device" jobs running on
  the same iPod simultaneously.

### 4.2 Per-task processing

For each `Task` (one Task per source song), `Organize::ProcessSomeFiles`
does:

```cpp
// src/organize/organize.cpp:208-235 (paraphrased)
Song::FileType dest_type = CheckTranscode(song.filetype());
if (dest_type != Song::FileType::Unknown) {
  TranscoderPreset preset = Transcoder::PresetForFileType(dest_type);
  task.transcoded_filename_ = transcoder_->GetFile(...);
  task.new_extension_       = preset.extension_;
  task.new_filetype_        = dest_type;
  tasks_transcoding_[input] = task;
  transcoder_->AddJob(input, preset, task.transcoded_filename_);
  transcoder_->Start();
  continue;                     // come back here when transcoding finishes
}
```

### 4.3 `CheckTranscode` — the decision tree

```cpp
// src/organize/organize.cpp:324
Song::FileType Organize::CheckTranscode(const Song::FileType original_type) const {
  const MusicStorage::TranscodeMode mode = destination_->GetTranscodeMode();
  const Song::FileType            format = destination_->GetTranscodeFormat();

  switch (mode) {
    case Transcode_Never:
      return Song::FileType::Unknown;            // no transcoding ever

    case Transcode_Always:
      if (original_type == format) return Unknown;
      return format;                              // always re-encode

    case Transcode_Unsupported:                   // ← default for devices
      if (supported_filetypes_.isEmpty() || supported_filetypes_.contains(original_type))
        return Song::FileType::Unknown;           // already iPod-native, no transcode
      if (format != Song::FileType::Unknown)
        return format;                            // transcode to user's preferred
      return Transcoder::PickBestFormat(supported_filetypes_);
  }
}
```

For our scenario:

| Input | `original_type` | iPod-native? | Decision                       |
| ----- | --------------- | ------------ | ------------------------------ |
| FLAC  | `FLAC`          | no           | transcode → user's choice = `ALAC` |
| MP3   | `MPEG`          | **yes**      | **direct copy, no transcoding** |
| AAC   | `MP4`           | yes          | direct copy                    |
| ALAC  | `ALAC`          | yes          | direct copy                    |

This already answers part of bug #1: **MP3 files are copied without
transcoding because the iPod plays MP3 natively** — the *preferred format*
setting only applies to files the iPod can't play directly.

### 4.4 Transcoding FLAC → ALAC

```cpp
// src/transcoder/transcoder.cpp:260
case Song::FileType::ALAC:
  return TranscoderPreset(filetype, u"ALAC"_s, u"m4a"_s,
                          u"audio/x-alac"_s, u"audio/mp4"_s);
```

`Transcoder::AddJob` builds a GStreamer pipeline equivalent to:

```
filesrc location=<input>.flac
  ! decodebin                       # any decoder GStreamer has → raw PCM
  ! audioconvert                    # PCM normalisation
  ! alacenc                         # ALAC encoder (gst-plugins-bad)
  ! mp4mux                          # MP4 container
  ! filesink location=<tmp>.m4a
```

The output goes to a temp file like
`~/Library/Caches/strawberry-install/transcoder/01 Song Title.m4a`.

When the pipeline emits EOS, the `GstBus` watcher calls `Transcoder::JobFinished`,
which (via signal/slot) lands in:

```cpp
// src/organize/organize.cpp:391
void Organize::FileTranscoded(const QString &input, const QString &output, const bool success) {
  Task task = tasks_transcoding_.take(input);
  if (success) tasks_pending_ << task;     // re-queue with transcoded_filename_ set
  process_files_timer_->start();
}
```

### 4.5 Second pass on a transcoded task

```cpp
// src/organize/organize.cpp:191
if (!task.transcoded_filename_.isEmpty()) {
  song.set_filetype(task.new_filetype_);                 // ALAC now
  song.set_url(QUrl::fromLocalFile(Utilities::FiddleFileExtension(
      song.basefilename(), task.new_extension_)));       // ".m4a"
  song.set_filesize(QFileInfo(task.transcoded_filename_).size());
}
```

Then it builds a `MusicStorage::CopyJob`:

```cpp
job.source_         = task.transcoded_filename_;     // .../tmp/Song.m4a
job.destination_    = task.song_info_.new_filename_;  // path inside iPod (informational)
job.metadata_       = song;                           // updated Song
job.overwrite_      = overwrite_;
job.albumcover_     = albumcover_;
job.remove_original_= !copy_;                         // false for "Copy to device"
job.playlist_       = playlist_;
job.progress_       = std::bind(&Organize::SetSongProgress,
                                this, _1,
                                !task.transcoded_filename_.isEmpty());
```

Album-art selection runs in parallel:

1. Prefer manual artwork (`song.art_manual()`).
2. Fall back to automatic artwork (`song.art_automatic()`).
3. If neither is on disk and the source is from a Device collection, load
   embedded art via `tagreader_client_->LoadCoverImageBlocking`.

Finally:

```cpp
destination_->CopyToStorage(job, error_text);
```

For an iPod, this is the next section.

---

## 5. `GPodDevice::CopyToStorage` — the actual transfer

```cpp
// src/device/gpoddevice.cpp:209-293
bool GPodDevice::CopyToStorage(const CopyJob &job, QString &error_text) {
  Itdb_Track *track = AddTrackToITunesDb(job.metadata_);   // 5.1

  if (job.albumcover_) {                                   // 5.2
    ... itdb_track_set_thumbnails(...) ...
  }

  GError *error = nullptr;
  itdb_cp_track_to_ipod(track,                             // 5.3
                        QDir::toNativeSeparators(job.source_).toLocal8Bit().constData(),
                        &error);
  if (error) {
    itdb_track_remove(track);                              // rollback
    return false;
  }

  if (!job.playlist_.isEmpty()) { ... add to playlist ... }  // 5.4
  AddTrackToModel(track, url_.path());                       // 5.5
  if (job.remove_original_) QFile::remove(job.source_);
  return true;
}
```

### 5.1 `AddTrackToITunesDb` — build the in-memory record

```cpp
// src/device/gpoddevice.cpp:183
Itdb_Track *track = itdb_track_new();        // libgpod helper, sets visible=1
metadata.ToItdb(track);                      // copy Song fields → track fields
itdb_track_add(db_, track, -1);              // ← runs itdb_track_set_defaults inside
Itdb_Playlist *mpl = itdb_playlist_mpl(db_);
itdb_playlist_add_track(mpl, track, -1);     // also add to master playlist
return track;
```

`Song::ToItdb` (`src/core/song.cpp:1762`) maps strawberry's Song to libgpod's
fields:

```cpp
track->title       = strdup(d->title_...);
track->album       = strdup(d->album_...);
... artist, album-artist, year, genre, comment, composer, grouping ...
track->tracklen    = length_nanosec / kNsecPerMsec;
track->bitrate     = d->bitrate_;
track->samplerate  = d->samplerate_;
track->type1       = (filetype == MPEG ? 1 : 0);
track->type2       = (filetype == MPEG ? 1 : 0);
track->mediatype   = 1;                       // Audio
track->size        = d->filesize_;
track->time_modified = d->mtime_;
track->time_added  = d->ctime_;
... playcount, skipcount, lastplayed ...
```

Notice: **`track->filetype` is never set**. We'll come back to this in
"Bug #2" — it's the root cause of the "MP3 invisible on iPod" issue.

Inside libgpod, `itdb_track_add` calls `itdb_track_set_defaults` (see
`.idea/strawberry-libgpod/src/itdb_track.c:67`) which fills in a bunch of
"magic" fields by **dispatching on `track->filetype`**:

```c
gchar *mp3_desc[] = {"MPEG", "MP3", "mpeg", "mp3", NULL};
gchar *mp4_desc[] = {"AAC", "MP4", "M4A", "aac", "mp4", "m4a", NULL};

if (haystack(tr->filetype, mp3_desc))      { tr->unk126 = 0xffff; tr->unk144 = 0x000c; }
else if (haystack(tr->filetype, mp4_desc)) { tr->unk126 = 0xffff; tr->unk144 = 0x0033; }
else if (haystack(tr->filetype, wav_desc)) { tr->unk126 = 0x0000; tr->unk144 = 0x0000; }
else                                       { tr->unk126 = 0x0000; tr->unk144 = 0x0000; }  // ← we land here
```

### 5.2 Cover art

If `job.albumcover_` and the cover is in memory (a `QImage`), it's saved to a
temp JPEG under `$TMPDIR/track-albumcover-XXXXXX.jpg`, the temp file is
handed to `itdb_track_set_thumbnails` (which encodes the right pixel formats
for this iPod's `Itdb_Device`'s artwork capabilities) and the `cover_files_`
list owns the temp file until `WriteDatabase` runs (libgpod re-reads the
files on `itdb_write`). If only a path is known, it's passed directly.

### 5.3 `itdb_cp_track_to_ipod` — actual byte transfer

This is the heavy lifter inside libgpod. Conceptually:

1. **Generate a unique iPod-side path**:
   `<mount>/iPod_Control/Music/F<NN>/<RANDOM4>.m4a`
   where `NN` is one of the round-robin music folders (`F00`–`F49` on
   recent iPods) and `RANDOM4` is four random uppercase characters.
2. **Copy the bytes** with `g_file_copy` (uses 64 KB-ish buffers under the
   hood). This is **synchronous** and there is **no progress callback** in
   the libgpod API — it returns when the whole file is done.
3. **Set `track->ipod_path`** to the new path with `/` swapped for `:`
   (legacy HFS path style required by the iPod firmware).
4. **Set `track->filetype_marker`** from the extension:
   `.mp3` → `0x4D503320` ('M','P','3',' '); `.m4a` → `0x4D344120` ('M','4','A',' ').

If anything fails, we roll back with `itdb_track_remove`.

### 5.4 Playlist update

If the Organise dialog had a target playlist:

```cpp
Itdb_Playlist *playlist = itdb_playlist_by_name(db_, playlist_name.data());
if (!playlist) {
  playlist = itdb_playlist_new(playlist_name.data(), false);
  itdb_playlist_add(db_, playlist, -1);
}
itdb_playlist_add_track(playlist, track, -1);
```

### 5.5 Reflect in Strawberry's UI

`AddTrackToModel` constructs a Song from the just-written `Itdb_Track` and
appends it to `songs_to_add_`. These are bulk-applied in `Finish()` (§6.2).

---

## 6. Finishing up

### 6.1 `FinishCopy` → `WriteDatabase`

```cpp
// src/device/gpoddevice.cpp:335
bool GPodDevice::FinishCopy(bool success, QString &error_text) {
  if (success) success = WriteDatabase(error_text);
  Finish(success);
  return ConnectedDevice::FinishCopy(success, error_text);
}

// src/device/gpoddevice.cpp:295
bool GPodDevice::WriteDatabase(QString &error_text) {
  GError *error = nullptr;
  const bool success = itdb_write(db_, &error);
  cover_files_.clear();
  ...
}
```

`itdb_write` is the inverse of `itdb_parse`: it serialises every byte of the
in-memory tree back to disk, regenerating:

* `iTunesDB`, `iTunesDBHash58`, `iTunesDBHash72` (firmware-specific hashes —
  this is why an iPod plugged into iTunes after Strawberry won't get its
  Strawberry-added tracks wiped, *if* libgpod knows the device's hash key).
* `ArtworkDB` and the rendered thumbnail blobs for the cover art added in
  this session.
* Per-playlist files where applicable.

This is the **only** point where the iPod's filesystem actually gets the new
metadata — until `itdb_write` runs, you can yank the cable and lose every
"copied" file (they're on disk, but the iPod's UI won't know about them).

### 6.2 `Finish` — local bookkeeping + unlock

```cpp
// src/device/gpoddevice.cpp:316
void GPodDevice::Finish(const bool success) {
  if (success) {
    if (!songs_to_add_.isEmpty())    collection_backend_->AddOrUpdateSongs(songs_to_add_);
    if (!songs_to_remove_.isEmpty()) collection_backend_->DeleteSongs(songs_to_remove_);
  }
  collection_backend_->Close();              // close the per-thread sqlite conn
  songs_to_add_.clear();
  songs_to_remove_.clear();
  cover_files_.clear();
  db_busy_.unlock();
}
```

The new songs are inserted into the per-device sqlite database
(`devices.db`), so the next time you open the iPod's view they show up
without a full re-parse.

### 6.3 `Organize::ProcessSomeFiles` finishes

When both queues are empty:

```cpp
// src/organize/organize.cpp:153
if (!destination_->FinishCopy(...)) { ... log error ... }
if (eject_after_) destination_->Eject();
task_manager_->SetTaskFinished(task_id_);
Q_EMIT Finished(files_with_errors_, log_);
```

`SetTaskFinished` removes the progress task from the task panel. If the user
ticked *Eject when done*, we then call `Eject()` which on macOS issues a
`DADiskUnmount` (equivalent to `diskutil eject`).

---

## 7. The annotated end-to-end picture (FLAC → ALAC)

```text
USER ACTION                  CODE PATH                                          THREAD
──────────────────────────   ────────────────────────────────────────────────   ───────────
Plug in iPod
                             diskarbitrationd mounts /Volumes/IPOD
                             DiskAddedCallback                                  DA cb thread
                              ↓ Q_EMIT DeviceAdded(serial)
                             DeviceManager::PhysicalDeviceAdded                 GUI
                              ↓ MakeUrlFromLocalPath ⇒ IsIpod ⇒ "ipod://..."
                              ↓ schema match → GPodDevice instance

Open iPod
                             GPodDevice::Init / ConnectAsync                    GUI / loader
                              ↓ itdb_parse(/Volumes/IPOD)
                             LoadFinished sets db_, wakes db_wait_cond_         GUI

Right-click → Copy to device
                             CollectionView::CopyToDevice                        GUI
                              ↓ OrganizeDialog::accept
                              ↓ Organize::Start ⇒ moveToThread                  GUI
                                                                                 ↓
                             Organize::ProcessSomeFiles                          Organize thread
                              ↓ destination_->StartCopy
                              ↓ GPodDevice::Start
                              ↓   wait db_wait_cond_, lock db_busy_
                              ↓ GetSupportedFiletypes → {MP4, MPEG, ALAC}
                              ↓ CheckTranscode(FLAC) → ALAC
                              ↓ Transcoder::AddJob + Start                       Organize thread
                                                                                 ↓
                             GStreamer pipeline FLAC→ALAC                        gst thread
                              ↓ (filesrc … alacenc … filesink)
                              ↓ EOS → Transcoder::JobFinished
                             FileTranscoded                                      Organize thread
                              ↓ tasks_pending_ << task (now has .m4a)

                             Organize::ProcessSomeFiles (2nd pass for this Task) Organize thread
                              ↓ build CopyJob (source = /tmp/...m4a)
                              ↓ destination_->CopyToStorage(job)
                             GPodDevice::CopyToStorage                           Organize thread
                              ↓ AddTrackToITunesDb (Song→Itdb_Track)
                              ↓   itdb_track_add → itdb_track_set_defaults
                              ↓ itdb_track_set_thumbnails (if cover)
                              ↓ itdb_cp_track_to_ipod
                              ↓   - picks /iPod_Control/Music/F12/AB3D.m4a
                              ↓   - g_file_copy bytes (synchronous, no progress callback)
                              ↓   - sets track->ipod_path, filetype_marker
                              ↓ AddTrackToModel

                             (loop done)                                         Organize thread
                              ↓ destination_->FinishCopy
                              ↓ GPodDevice::WriteDatabase → itdb_write
                              ↓ Finish ⇒ AddOrUpdateSongs, unlock db_busy_
                              ↓ task_manager_->SetTaskFinished
                              ↓ Q_EMIT Finished
                              ↓ thread_->quit
```

---

## 8. The MP3 path (the bugs)

If the source had been an MP3:

* `CheckTranscode(MPEG)` returns `Unknown` (because `MPEG ∈ {MP4, MPEG, ALAC}`).
* No transcoding step.
* We jump straight to `CopyToStorage` with `job.source_` being the original `.mp3`.
* `itdb_cp_track_to_ipod` copies the bytes verbatim to
  `/iPod_Control/Music/Fxx/RAND4.mp3`.
* `Song::ToItdb` populates the `Itdb_Track` — and **leaves `track->filetype`
  empty**.
* `itdb_track_set_defaults` falls into the unknown branch.
* `itdb_write` flushes the wrong `unk126`/`unk144` values to disk.
* The iPod's firmware reads the database, sees the magic fields don't match
  any audio codec it knows how to render in the menu UI, and **silently
  filters the track out** of every Music menu (Artists, Albums, Songs,
  Playlists). The bytes are still on the iPod consuming space; the entry is
  in the iTunesDB; but you can never navigate to it.

---

## 9. Bug #1 — ALAC transcoding hangs forever ("Organizing files 50%")

### 9.1 What the user sees

* Set the iPod's *Preferred format* to **ALAC** in device properties.
* Right-click *any* FLAC (even a 0:00-second test file) → *Copy to device…*.
* The task panel shows **"Organizing files 50%"** and stays there forever.
* The file never lands on the iPod. The transcode never finishes.

If you instead pick *Preferred format = M4A AAC*, the same FLAC encodes and
copies fine. So whatever's broken is specific to the ALAC pipeline.

### 9.2 The 50 % comes from `Organize::SetSongProgress`

```cpp
// src/organize/organize.cpp:351
void Organize::SetSongProgress(const float progress, const bool transcoded) {
  const int max = transcoded ? 50 : 100;
  current_copy_progress_ = (transcoded ? 50 : 0)
                         + qBound(0, static_cast<int>(progress * static_cast<float>(max)), max - 1);
  UpdateProgress();
}
```

When a task is transcoding, progress reports go through `transcode_progress_`
(also clamped to [0..50]) and `current_copy_progress_` stays at zero until
the file moves to the copy phase. The progress bar climbs from 0 → 50 while
transcoding, then jumps to 50 + copy% once `FileTranscoded` fires.

If `FileTranscoded` **never fires**, the progress bar sits at 50 % forever
and `tasks_pending_` never makes it back to `ProcessSomeFiles` to start the
iPod copy — which is exactly what we're seeing.

### 9.3 Why ALAC's GStreamer pipeline never reaches EOS

`Transcoder::StartJob` builds this pipeline:

```text
filesrc → decodebin → audioconvert → audioresample → <codec> → <muxer> → filesink
```

For *Preferred format = ALAC*, `TranscoderPreset` is
`(codec_mimetype=audio/x-alac, muxer_mimetype=audio/mp4)`. The muxer is
forced to `mp4mux` by a short-circuit at the top of
`CreateElementForMimeType`. The codec is chosen by
`CreateElementForMimeType(GST_ELEMENT_FACTORY_TYPE_AUDIO_ENCODER,
"audio/x-alac", …)` — and **here is where it goes wrong**.

Strawberry's macOS dependency bundle ships only two ALAC encoders, both from
the libav plugin (`libgstlibav.dylib`):

```text
$ gst-inspect-1.0 | grep alac
libav:  avenc_alac:    libav ALAC (Apple Lossless Audio Codec) encoder
libav:  avenc_alac_at: libav alac (AudioToolbox) encoder
```

There is no native `alacenc` (that one lives in `gst-plugins-bad`, which
this build doesn't include the way it includes `faac` and `fdkaacenc` for
AAC).

`CreateElementForMimeType` then iterates the GStreamer registry, collects
all candidates that can produce `audio/x-alac` caps, and runs this snippet
*pre-fix*:

```cpp
if (name.startsWith("avmux"_L1) || name.startsWith("avenc"_L1)) {
  rank = -1;  // ffmpeg usually sucks
}
```

Both candidates get hammered down to rank `-1`. The comparator only sorted
on rank, so the tiebreak between the two `-1`-ranked candidates was
**undefined** — whichever `std::sort` happened to put last is what
`suitable_elements_.last()` picks.

On my machine (and likely most macOS installs because the AudioToolbox
plugins register later) this resolves to `avenc_alac_at`. That encoder
delegates to Apple's AudioToolbox `kAudioFormatAppleLossless` encoder, which
on macOS Sequoia has a **broken EOS path** in libav's wrapper: it accepts
input buffers fine but never produces a final output buffer or forwards
EOS. `mp4mux` waits indefinitely for EOS to finalize the `moov` atom. The
pipeline never moves out of PLAYING. `FileTranscoded` is never posted.
`current_copy_progress_` stays at 50.

Verified by reproducing manually:

```bash
$ gst-launch-1.0 audiotestsrc num-buffers=100 ! audioconvert \
                ! avenc_alac    ! mp4mux ! filesink location=/tmp/ok.m4a
... pipeline ended after 5ms, file is 180 KB and valid

$ gst-launch-1.0 audiotestsrc num-buffers=100 ! audioconvert \
                ! avenc_alac_at ! mp4mux ! filesink location=/tmp/bad.m4a
... pipeline never reaches EOS, file grows without bound, no moov atom written
```

For AAC (`M4A AAC`) the registry contains `faac` (rank 256) and `fdkaacenc`
(rank 256) which both have *positive* ranks. After the libav demotion the
chosen encoder is `faac` or `fdkaacenc`, both of which work, which is why
*M4A AAC* succeeds where *ALAC* hangs.

### 9.4 The fix (applied)

The patch in `src/transcoder/transcoder.cpp`:

1. **Stops blanket-demoting libav encoders when they're the only option.**
   Before deciding to set rank to `-1`, scan the candidate list and only
   apply the demotion if a non-libav alternative exists. For ALAC, both
   candidates are libav, so we keep their original (positive) GStreamer
   ranks and pick the best one rather than letting the order be undefined.
2. **Adds an explicit tiebreaker** that prefers names *without* the `_at`
   suffix. Even if AudioToolbox-backed encoders ever come back to bite us
   on equal ranks, the plain libav variant wins.
3. **Adds a final lexicographic tiebreaker** so element selection is
   deterministic and reproducible across runs and machines.

```cpp
// src/transcoder/transcoder.cpp
struct SuitableElement {
  bool operator<(const SuitableElement &other) const {
    if (rank_ != other.rank_) return rank_ < other.rank_;
    const bool this_at  = name_.endsWith("_at"_L1);
    const bool other_at = other.name_.endsWith("_at"_L1);
    if (this_at != other_at) return this_at;  // non-_at sorts greater → picked
    return name_ < other.name_;
  }
};

// inside CreateElementForMimeType, after collecting all candidates:
if (has_native) {
  for (int i = 0; i < suitable_elements_.size(); ++i) {
    const QString &n = suitable_elements_[i].name_;
    if (n.startsWith("avmux"_L1) || n.startsWith("avenc"_L1)) {
      suitable_elements_[i].rank_ = -1;
    }
  }
}
```

With this fix, FLAC → ALAC reliably picks `avenc_alac`, the pipeline EOSes
in milliseconds, `FileTranscoded` fires, `Organize` calls
`CopyToStorage`, and the file lands on the iPod. The 50 % wall is gone.

### 9.5 Bonus: but why does an MP3 copy at all if I picked ALAC?

Separately from the hang, the user's other wording — *"Why do tracks get
copied successfully if the codec is not alac?"* — is the answer from §4.3:

> The *Preferred format* setting only kicks in for **unsupported** input
> formats. MP3 is supported by the iPod itself, so Strawberry hands the
> file to the device as-is. ALAC is your fallback for things the iPod
> *couldn't* play otherwise.

If you want every import re-encoded to ALAC regardless of source, switch
the iPod's transcode mode from *Transcode unsupported files* to *Always
transcode*. That moves `CheckTranscode` into the `Transcode_Always` branch
and every non-ALAC source — including MP3 — goes through `avenc_alac`.

---

## 10. Bug #2 — Why MP3 files vanish from the iPod after copying

### 10.1 The trail

1. `Song::ToItdb` populates `track->title`, `artist`, etc. but **never sets
   `track->filetype`** (the user-readable string libgpod uses to identify
   the codec — `"MPEG audio file"`, `"AAC audio file"`, etc.).

2. `itdb_track_add` is called immediately after `Song::ToItdb`. libgpod's
   `itdb_track_add` calls `itdb_track_set_defaults` (see
   `.idea/strawberry-libgpod/src/itdb_track.c:217`), which dispatches on
   `track->filetype`:

   ```c
   /* itdb_track_set_defaults excerpts, .../itdb_track.c:92, .../itdb_track.c:122 */
   gchar *mp3_desc[] = {"MPEG", "MP3", "mpeg", "mp3", NULL};
   gchar *mp4_desc[] = {"AAC", "MP4", "M4A", "aac", "mp4", "m4a", NULL};

   if (haystack(tr->filetype, mp3_desc)) { tr->unk126 = 0xffff; tr->unk144 = 0x000c; }
   else if (haystack(tr->filetype, mp4_desc)) {
     if (haystack(tr->filetype, audible_subdesc)) { tr->unk126 = 0x0001; tr->unk144 = 0x0029; }
     else                                          { tr->unk126 = 0xffff; tr->unk144 = 0x0033; }
   }
   else if (haystack(tr->filetype, wav_desc)) { tr->unk126 = 0x0000; tr->unk144 = 0x0000; }
   else                                       { tr->unk126 = 0x0000; tr->unk144 = 0x0000; }
   ```

3. `tr->filetype` is `NULL`, so `haystack` returns false for every probe and
   we land in the final `else` branch — the same one libgpod uses for *"this
   is an uncompressed/unknown blob"*. `unk126` ends up `0x0000` and `unk144`
   ends up `0x0000`.

4. `itdb_write` faithfully serialises those wrong magic numbers into the
   binary `iTunesDB`. The bytes get to the iPod, the database knows about
   the track, libgpod and any host-side parser see it just fine.

5. The iPod firmware — which is what builds the on-device "Music"
   menus — uses these magic fields (alongside the file extension) to decide
   what kind of audio file this is and whether to expose it. Audio (MP3/AAC)
   tracks marked as "WAV-like" with `unk144 = 0x0000` get **silently
   dropped** from the menu population pass. The file is on disk, the entry
   is in the database, but the menus never list it.

The reason **ALAC tracks transcoded by Strawberry happen to *also* be
broken** is the same: their `track->filetype` is unset too. But you don't
notice for ALAC because the only ALAC tracks on the iPod are the ones
Strawberry just put there — there's nothing to compare against. And many
ALAC tracks playable from the *Songs* menu because the iPod's M4A/AAC
codepath is sometimes more lenient than the MP3 codepath. The bug is
genuinely identical; MP3 is just the most reliable repro because everyone
already has MP3s on their iPod from iTunes (which sets the field correctly).

### 10.2 The fix (applied)

`Song::ToItdb` in `src/core/song.cpp` now sets `track->filetype` to the
standard iTunes descriptor string and assigns `track->type1`/`track->type2`
per libgpod's documented MP3/AAC convention:

```cpp
// src/core/song.cpp — Song::ToItdb (post-fix)
switch (d->filetype_) {
  case FileType::MPEG:
    track->filetype = strdup("MPEG audio file");
    track->type1 = 1;  // VBR (what iTunes also writes; matches unk126=0xffff, unk144=0x000c)
    track->type2 = 1;
    break;
  case FileType::MP4:                                            // AAC in MP4 container
    track->filetype = strdup("AAC audio file");
    track->type1 = 0; track->type2 = 0;
    break;
  case FileType::ALAC:                                           // ALAC in MP4 container
    track->filetype = strdup("Apple Lossless audio file");
    track->type1 = 0; track->type2 = 0;
    break;
  case FileType::WAV:
    track->filetype = strdup("WAV audio file");
    track->type1 = 0; track->type2 = 0;
    break;
  case FileType::AIFF:
    track->filetype = strdup("AIFF audio file");
    track->type1 = 0; track->type2 = 0;
    break;
  case FileType::FLAC:                                           // iPod can't play but be honest
    track->filetype = strdup("FLAC audio file");
    track->type1 = 0; track->type2 = 0;
    break;
  default:
    // Leave track->filetype NULL — libgpod's unknown-blob defaults apply
    track->type1 = 0; track->type2 = 0;
    break;
}
```

The previous one-liner

```cpp
track->type1 = (d->filetype_ == FileType::MPEG ? 1 : 0);
track->type2 = (d->filetype_ == FileType::MPEG ? 1 : 0);
```

was wrong because per `itdb.h:1398-1399` `type2` for MP3 is `0x01` (correct
above) and `type2` for AAC is `0x00` (the old code also produced `0x00`
which was accidentally correct only for the AAC case).

With both lines fixed:

* Newly copied MP3s appear in the iPod's *Songs*, *Artists*, *Albums*, and
  *Playlists* menus because `itdb_track_set_defaults` now matches
  `mp3_desc` and writes `unk126=0xffff, unk144=0x000c`.
* Newly transcoded ALAC files keep working and pick up
  `unk126=0xffff, unk144=0x0033` (the same magic that iTunes writes for
  MP4-container audio).
* Existing on-iPod tracks that iTunes had previously synced are unaffected,
  since `Song::ToItdb` is only called on tracks Strawberry is *adding*.

### 10.3 How to verify

After applying the patch and rebuilding (`./install-macos.sh build && ./install-macos.sh bundle && ./install-macos.sh install -y`):

1. Connect a freshly-formatted iPod (or one that's been mounted at least
   once by iTunes so its `SysInfo` is initialised).
2. In Strawberry, right-click a couple of MP3s and *Copy to device…*.
3. After "Finished organizing", unmount/eject via Strawberry.
4. Plug the iPod into your Mac without Strawberry running and inspect with:

   ```bash
   /opt/strawberry_macos_x86_64_release/bin/itdb-syslog /Volumes/IPOD | grep -E "Track:|filetype|unk126|unk144"
   ```

   You should now see `filetype = "MPEG audio file"`, `unk126 = 65535
   (0xffff)`, `unk144 = 12 (0x000c)` for the MP3 entries.
5. Disconnect and navigate on the iPod itself — the new tracks should now
   appear in Music → Artists / Albums / Songs.

---

## 11. Bug #3 — Copies succeed but the iPod shows "No Songs"

### 11.1 What the user sees

* `Device/SysInfoExtended` does not exist; `Device/SysInfo` exists but is 0 bytes.
* Strawberry copies a track, the file shows up in Strawberry's iPod view, the
  bytes are physically under `/Volumes/iPod/iPod_Control/Music/F*/`.
* You eject from Strawberry. You disconnect.
* You boot the iPod stand-alone and Music → Songs reads **"No Songs"**,
  Music → Playlists reads **"No Playlists"**.

This isn't related to Bug #2 (`track->filetype`). That bug makes individual
tracks vanish from menus *within an otherwise-working library*. Bug #3 makes
the iPod refuse the **entire** iTunesDB.

### 11.2 The two pieces of data libgpod needs

Two things have to be in `Device/SysInfo` (or `Device/SysInfoExtended`)
for libgpod to produce an iTunesDB the iPod firmware will accept:

**(a) `FirewireGuid`** — the 64-bit hardware ID. Used as the key for the
**hash58** SHA1-MAC that gets written into the 16-byte field at offset
`0x58` in the iTunesDB's `mhbd` header. The iPod recomputes the same MAC
on boot from the file contents + its own GUID and only loads tracks if
the two match.

**(b) `ModelNumStr`** — the iPod model number (e.g. `MC297` for Classic
7G 160GB Black). Used to look up an `Itdb_IpodInfo` row that tells
libgpod which *checksum scheme* this iPod uses. Looking inside
`itdb_device_get_checksum_type`:

```c
// .idea/strawberry-libgpod/src/itdb_device.c
} else {
  const Itdb_IpodInfo *info;
  info = itdb_device_get_ipod_info (device);
  if (info == NULL) {
    return ITDB_CHECKSUM_NONE;
  }
  switch (info->ipod_generation) {
    case ITDB_IPOD_GENERATION_CLASSIC_1:
    case ITDB_IPOD_GENERATION_CLASSIC_2:
    case ITDB_IPOD_GENERATION_CLASSIC_3:
    case ITDB_IPOD_GENERATION_NANO_3:
    case ITDB_IPOD_GENERATION_NANO_4:
      return ITDB_CHECKSUM_HASH58;
    ...
  }
}
```

And `itdb_device_get_ipod_info`:

```c
model_num = itdb_device_get_sysinfo (device, "ModelNumStr");
if (!model_num)
    return &ipod_info_table[0];          // "Invalid", generation = UNKNOWN
info = get_ipod_info_from_model_number (model_num);
if (info != NULL) {
    return info;
} else {
    return &ipod_info_table[1];          // "Unknown", generation = UNKNOWN
}
```

`ITDB_IPOD_GENERATION_UNKNOWN` falls out of every named `case` in the
switch above and hits the default `return ITDB_CHECKSUM_NONE;` at the
bottom of the function.

The downstream effect:

```c
G_GNUC_INTERNAL gboolean itdb_device_write_checksum (...) {
    switch (itdb_device_get_checksum_type (device)) {
    case ITDB_CHECKSUM_NONE:
        return TRUE;                     // ← writes nothing, no error
    case ITDB_CHECKSUM_HASH58:
        return itdb_hash58_write_hash (...);
    ...
}
```

`CHECKSUM_NONE` is a quiet no-op. `itdb_write` returns success, the on-disk
iTunesDB has `hashing_scheme = 0x0000` at offset `0x68` and a zeroed
16-byte hash58 field at offset `0x58`, and the iPod firmware silently
discards every track when it loads the DB. From the user's perspective:
"No Songs", "No Playlists", "No Artists", everywhere.

### 11.3 How I found this (the diagnostic path)

The mistake worth recording: I initially assumed only `FirewireGuid` was
needed, wrote it alone, and the iPod still showed "No Songs". I had to
parse the actual on-disk iTunesDB to see what hadn't been signed:

```bash
$ python3 -c '
import struct
d = open("/Volumes/iPod/iPod_Control/iTunes/iTunesDB","rb").read()
print(f"hashing_scheme @0x68: 0x{struct.unpack_from(\"<H\", d, 0x68)[0]:04x}")
print(f"hash58 @0x58 nonzero bytes: {sum(1 for b in d[0x58:0x68] if b)}")
'
hashing_scheme @0x68: 0x0000     ← libgpod skipped the signature entirely!
hash58 @0x58 nonzero bytes: 0
```

`hashing_scheme = 0` meant libgpod's `get_checksum_type` had returned
`CHECKSUM_NONE` — i.e. the iTunesDB went out unsigned. That ruled out the
`itdb_hash58_write_hash` failure path (which would still set
`hashing_scheme = ITDB_CHECKSUM_HASH58 = 4`) and pointed at
`get_checksum_type` itself. Tracing it backwards through
`get_ipod_info` made it clear that without `ModelNumStr` the model lookup
returns the "Invalid" sentinel, generation is `UNKNOWN`, and the switch
falls into the silent `CHECKSUM_NONE` branch.

After adding `ModelNumStr: MC297` (which maps to
`ITDB_IPOD_GENERATION_CLASSIC_3` → `ITDB_CHECKSUM_HASH58`) the iPod
immediately started accepting the DB and playing tracks.

### 11.4 Why this iPod was in that state

The user had never plugged this iPod into iTunes or Music.app on this
machine — the iPod had only been restored via macOS Finder's "Restore
iPod" function (which sets up the on-iPod firmware but does **not** write
`Device/SysInfoExtended`). The `Device/SysInfo` file existed but was 0
bytes because of an earlier failed libgpod write. Result: no GUID, no
model, `get_checksum_type` returns NONE, iPod silently rejects the DB.

### 11.5 The fix (applied)

`GPodDevice::Init` now calls `EnsureIpodSysInfo()` before `itdb_parse`
runs. The helper writes BOTH `ModelNumStr` and `FirewireGuid`:

```cpp
// src/device/gpoddevice.cpp
void EnsureIpodSysInfo(const QString &mount_point, const QString &unique_id) {

  // bail if SysInfoExtended exists (Apple's authoritative file)
  if (QFile::exists(sysinfo_extended_path) && ...) return;

  // bail if Device/SysInfo already contains FirewireGuid
  if (... && contents.contains("FirewireGuid"_L1, Qt::CaseInsensitive)) return;

  // strip "USB/" prefix from unique_id and validate as hex
  QString serial = unique_id;
  const qsizetype slash = serial.indexOf(u'/');
  if (slash >= 0) serial = serial.mid(slash + 1);
  if (!hex_re.match(serial).hasMatch()) return;   // not an iPod USB serial

  // write the minimal SysInfo. BOTH lines are required.
  const QByteArray contents =
      QStringLiteral("ModelNumStr: MC297\nFirewireGuid: 0x%1\n")
          .arg(serial.toUpper()).toUtf8();
  file.write(contents);
}

bool GPodDevice::Init() {
  InitBackendDirectory(url_.path(), first_time_);
  collection_model_->Init();
  EnsureIpodSysInfo(url_.path(), unique_id_);   // <-- new
  ...
}
```

Why `MC297` specifically? It maps in libgpod's model table to:

```c
{"C297", 160, ITDB_IPOD_MODEL_CLASSIC_BLACK,  ITDB_IPOD_GENERATION_CLASSIC_3, ...}
```

…and `get_ipod_info_from_model_number` strips one leading letter
(`M` → `C297`) before the lookup. CLASSIC_3 in `get_checksum_type` →
`ITDB_CHECKSUM_HASH58`. The actual hash58 calculation is keyed on
`FirewireGuid + SHA1(iTunesDB)` and is **independent of the model
number** — so MC297 also produces correct hashes for any other HASH58
device (Classic 6G/7G, Nano 3G/4G) even though the displayed model name
would be wrong.

Importantly, MC297 is the SAFEST single hardcoded choice because:

* All Classic 6G/7G + Nano 3G/4G use HASH58 (MC297 picks HASH58 — correct).
* HASH58 is the only checksum scheme libgpod can produce **without** an
  iTunes-extracted `HashInfo` file. HASH72 and HASHAB cannot be generated
  without one regardless of what we put here, so MC297 doesn't make
  newer-iPod scenarios worse.
* Cover-art sizes / chapter image dimensions fall back to Classic-3
  defaults — close enough for the devices this fix actually helps.

The fix is intentionally non-destructive: it leaves `SysInfoExtended`
and any user-written `SysInfo` alone, only filling in when the file is
absent or empty.

### 11.6 How to verify

```bash
# Before the fix: empty file or missing FirewireGuid
$ cat /Volumes/iPod/iPod_Control/Device/SysInfo
# (empty)

# After the fix (after reconnecting the iPod with patched Strawberry):
$ cat /Volumes/iPod/iPod_Control/Device/SysInfo
ModelNumStr: MC297
FirewireGuid: 0x000A2700213F49B4

# Then after one copy + eject cycle, the iTunesDB must be signed:
$ python3 -c '
import struct
d = open("/Volumes/iPod/iPod_Control/iTunes/iTunesDB","rb").read()
print(f"hashing_scheme @0x68: 0x{struct.unpack_from(\"<H\", d, 0x68)[0]:04x}")
print(f"hash58 nonzero bytes: {sum(1 for b in d[0x58:0x68] if b)}")
print(f"mhit (tracks): {d.count(b\"mhit\")}")
'
hashing_scheme @0x68: 0x0004   ← was 0x0000 before the fix
hash58 nonzero bytes: 16       ← was 0 before the fix
mhit (tracks): 1
```

Unplug, power-cycle the iPod, navigate to Music → Songs. The copied
track should now be listed AND playable.

### 11.7 Diagnostic procedure if it still doesn't work

If a future user reports "No Songs" after a Strawberry sync, run through
this in order:

```bash
# 1. Is the iPod USB-mounted and does it have a sensible structure?
ls -la /Volumes/<your-ipod>/iPod_Control/Device/

# 2. Does SysInfo have BOTH lines?
cat /Volumes/<your-ipod>/iPod_Control/Device/SysInfo
# expected:
#   ModelNumStr: <some_iPod_model>
#   FirewireGuid: 0x<16-hex-chars>

# 3. Is libgpod actually signing the DB after a sync?
python3 -c '
import struct
d = open("/Volumes/<your-ipod>/iPod_Control/iTunes/iTunesDB","rb").read()
scheme = struct.unpack_from("<H", d, 0x68)[0]
hash58_nz = sum(1 for b in d[0x58:0x68] if b)
print(f"hashing_scheme = 0x{scheme:04x} (3=hash58, 4=hash72, 5=hashAB, 0=NONE)")
print(f"hash58 nonzero = {hash58_nz}   (must be >0 for the iPod to accept the DB)")
print(f"tracks         = {d.count(b\"mhit\")}")
'

# 4. Is Strawberry actually loading the iPod via the ipod:// path?
/Applications/strawberry.app/Contents/MacOS/strawberry --log-levels=*:3 2>&1 \
  | grep -iE "ipod|gpod|itdb|sysinfo|hash"
# expected log lines:
#   DeviceManager: Connecting QUrl("ipod:/Volumes/<your-ipod>")
#   GPodDevice: wrote .../SysInfo with FirewireGuid 0x... (first time only)
```

If step 3 shows `hashing_scheme = 0x0000` after a sync, the model-number
hack didn't kick in — the user probably has a `Device/SysInfo` that
exists, is non-empty, has `FirewireGuid` (so we skip), but does NOT have
`ModelNumStr`. Delete the file and reconnect, or hand-edit it to add the
ModelNumStr line.

### 11.8 Caveats / future work

* The macOS path uses the lister's `USB/<serial>` `unique_id`. Linux and
  Windows use different `unique_id` schemes; on those platforms the
  helper bails at the hex-validation step and you'll still need the
  standard libgpod/`gtkpod` flow (sgutils + libimobiledevice) to
  populate SysInfo. That's fine — those platforms already have working
  paths.
* The model number is hardcoded. If a future iPod model uses HASH58 but
  needs a different chapter-image format, cover art may render at the
  wrong dimensions. Easy follow-up: look up the actual USB PID via
  IOKit in `MacOsDeviceLister`, pass it through `unique_id` or a new
  field, and pick the right ModelNumStr per device.
* iPad / iPhone / iPod Touch / Nano 5G+ use `hash72` or `hashAB`, which
  need a `HashInfo` file extracted from an iTunes-written DB. Those
  devices MUST be initialized by Apple's iTunes/Music.app first; the
  Strawberry fix above is a no-op for them.

---

## 12. TL;DR / cheat sheet

| Question | Short answer |
| -------- | ------------ |
| How does Strawberry know an iPod is plugged in? | `DARegisterDiskAppearedCallback` on macOS; identifies it by the presence of `/iPod_Control` on the mounted volume |
| What turns an `ipod://` URL into a working device? | `DeviceManager::AddDeviceClass<GPodDevice>()` registers the scheme; `GPodLoader::LoadDatabase` calls `itdb_parse` to read the on-iPod database |
| What happens when I right-click "Copy to device" on a FLAC with Preferred format = ALAC? | UI builds an `Organize` worker → `Organize` discovers iPod-supported formats `{MP4, MPEG, ALAC}` → FLAC isn't supported → transcodes via GStreamer to ALAC → re-queues the task → `GPodDevice::CopyToStorage` adds the `Itdb_Track`, sets thumbnails, calls `itdb_cp_track_to_ipod` which copies bytes into `/iPod_Control/Music/F<NN>/RAND4.m4a` → `WriteDatabase` calls `itdb_write` to flush the iTunesDB |
| Why does an MP3 copy succeed when I asked for ALAC? | "Preferred format" only applies to *unsupported* formats. MP3 is natively supported by the iPod, so Strawberry passes it through unchanged. Switch the device's transcode mode to *Always transcode* if you want everything re-encoded. |
| Why does ALAC transcoding hang at "Organizing files 50%"? | The macOS dependency bundle ships only `avenc_alac` and `avenc_alac_at` for `audio/x-alac` — no native `alacenc`. `Transcoder::CreateElementForMimeType` was demoting both to rank `-1` (blanket "avenc/avmux suck" rule) and the comparator only sorted on rank, so `std::sort` non-deterministically picked `avenc_alac_at`. That AudioToolbox-backed encoder doesn't propagate EOS, so `mp4mux` waits forever and `FileTranscoded` is never posted. **Fixed** in `src/transcoder/transcoder.cpp`: only demote `avenc/avmux` when a native alternative exists, and add a tiebreaker that prefers non-`_at` element names. ALAC now consistently picks `avenc_alac` and finishes in milliseconds. |
| Why do MP3s vanish from the iPod's menus after copying? | `Song::ToItdb` didn't set `track->filetype`, so libgpod's `itdb_track_set_defaults` fell through to the "unknown blob" branch and wrote `unk126=0x0000, unk144=0x0000`. The iPod firmware silently filters those out of every Music menu. **Fixed** in `src/core/song.cpp`: set `track->filetype = "MPEG audio file"` (etc.) in `Song::ToItdb`, plus correct `track->type1`/`type2` per libgpod's documented MP3/AAC convention. |
| Why does the iPod say "No Songs" even though Strawberry copied files? | The iPod Classic 6G/7G and Nano 4G enforce a `hash58` MAC over the iTunesDB. libgpod only writes the hash if BOTH `Device/SysInfo` entries are present: `FirewireGuid` (the SHA1 key) AND `ModelNumStr` (so `get_ipod_info` finds an entry with `ITDB_IPOD_GENERATION_CLASSIC_3` → `ITDB_CHECKSUM_HASH58`, instead of returning the "Invalid" sentinel that maps to `CHECKSUM_NONE`). Missing either one → `itdb_write` silently produces a DB with `hashing_scheme = 0x0000` and a zeroed hash → iPod firmware rejects the whole library and the menus show "No Songs". **Fixed** in `src/device/gpoddevice.cpp`: `GPodDevice::Init` now writes `ModelNumStr: MC297` + `FirewireGuid: 0x<USB-serial>` into `Device/SysInfo` (when neither it nor `SysInfoExtended` has FirewireGuid yet) before `itdb_parse` runs. MC297 maps to Classic-3 → HASH58, which is correct for every iPod libgpod can sign without iTunes-side initialization. |
