# 12. Sync Queue (OrganizeQueue)

> Audience: anyone touching "Copy to device…", "Organize files…", `src/organize/`, or the sync pane at the bottom of the Devices tab.

## 12.1 What it does

Every `OrganizeDialog::accept()` builds an `OrganizeQueue::JobRequest` and hands it to the process-wide `OrganizeQueue` (`src/organize/organizequeue.{h,cpp}`, owned by `DeviceManager`, reached with `OrganizeQueue::instance()`). The dialog closes immediately, so the user can queue more music, podcasts or audiobooks while a sync is running.

```
OrganizeDialog::accept()  ──Enqueue(request)──►  OrganizeQueue  (main thread)
                                                  │  jobs_ (FIFO, kept after they end until "Clear finished")
                                                  │
     Audiobook + merge on ──► Preparing ──QtConcurrent──► AudiobookMerger::MergeByBook (ffmpeg)
                                                  │            one preparing job at a time
                                                  ▼
                                               Queued ──Pump()──► Running: new Organize(...)->Start()
                                                                   │ one Running job per destination
                                                                   ▼
                                       Finished / Failed / Cancelled ──► MaybeEject(), Pump()
```

Scheduling rules (`OrganizeQueue::Pump`):
- **Same destination → one at a time**, oldest first. Destinations are compared by `MusicStorage*`; different devices or collection directories run in parallel.
- **Preparing doesn't block.** A job still merging audiobooks keeps its place in `jobs_`, but ready jobs behind it start; when it's ready it goes before jobs queued after it.
- **Eject is deferred.** `Organize` is always constructed with `eject_after = false`; the queue remembers the request and calls `MusicStorage::Eject()` on the main thread once the device has no unfinished jobs.
- **Disconnect cancels waiting jobs.** For `ConnectedDevice` destinations the queue listens to `DeviceCloseFinished` / `destroyed` and cancels that device's Queued / Preparing jobs (starting them would block on `GPodDevice::db_` or write to an unmounted path).

## 12.2 Cancelling

`OrganizeQueue::Cancel(id)`:
- Queued → Cancelled at once.
- Preparing → sets the shared `std::atomic<bool>` the merger polls (it kills ffmpeg), the job ends when the merge returns.
- Running → `Organize::Cancel()` (atomic flag). `ProcessSomeFiles` stops before the next song, drops the pending songs (deleting their transcoded temp files and releasing their `Transcoder` reservations), then finishes **normally**: `FinishCopy` writes the iTunesDB, so everything copied so far is kept. Transcodes already running finish and are dropped.

## 12.3 Progress pane

`DeviceSyncProgressModel` (flat list model) subscribes to the queue's signals: `JobAdded`, `JobSongsChanged` (audiobook chapters replaced by merged books), `JobChanged`, `JobRemoved`, `SongStarted`, `SongFinished`, `SongRetry`. Each job is a bold two-line header row ("Music · Album (12 songs)" / "Copying 3/12 → iPod") followed by its song rows. Songs are matched **by URL within the job**, not by position, because retries reorder processing (see rule 10 in `10-ipod-sync.md`). Song rows still unfinished when their job ends become "Skipped".

`DeviceSyncProgressView` (in `DeviceViewContainer`, below the device tree) shows a summary line, "Cancel job" (selected job) and "Clear finished", plus the same actions on right click.

The signals come from `Organize` on its worker thread; `StartJob` connects them with lambdas that capture the **job id** only, since `Organize` deletes itself when its thread finished and the job can be cleared.

## 12.4 Rules

- Never start an `Organize` yourself; enqueue a `JobRequest`. (`OrganizeDialog` is the only caller; if `OrganizeQueue::instance()` is null, e.g. in tests, it creates one parented to `qApp`.)
- Errors are reported through `OrganizeQueue::JobFinished(id, files_with_errors, log, success)`. `OrganizeDialog` remembers the ids it enqueued and shows `OrganizeErrorDialog` for its own jobs.
- A finished job releases its destination `SharedPtr` and song list, so a finished job doesn't keep a disconnected `GPodDevice` (and its whole iTunesDB) alive.
- Merged audiobooks share one cache file per book (`AudiobookMerger`'s stable cache key), which is why preparing is serialised.
- The old `DeviceSyncProgressModel::StartSync` / per-dialog wiring and `Organize::SyncQueueReady` are gone; don't reintroduce a "replace the whole list" API, it broke as soon as two jobs ran.
