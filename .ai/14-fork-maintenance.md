# 14. Fork Maintenance (syncing with upstream)

## 14.1 Layout

- `upstream` = `git@github.com:strawberrymusicplayer/strawberry.git`, `origin` = `git@github.com:kuthiala/strawberry.git`.
- `master` = upstream `master` + a **linear stack of fork commits** on top. No merge commits: rebasing keeps "what does the fork change" answerable with `git log upstream/master..master`.
- Fork commit subjects follow upstream's style (`ClassName: Short explanation`, body explains why). One topic per commit; fixes to an earlier fork commit go in as `fixup!` commits and are squashed with `git rebase --autosquash`.

The stack, oldest first (as of the 2026-10 sync onto upstream 1.2.31):

| Area | Commits |
|------|---------|
| macOS tooling | `install-macos.sh` helper; later "Build with upstream's macdeploytool deploy target" |
| Docs | `docs/iPod-copy-flow.md`, `.ai/` reference, `docs/INSTALL-macOS.md`, the bug diary, the reorganised `.ai/` + `.ai/tools` scripts |
| iPod sync | `Song::ToItdb` filetype string; Transcoder encoder tiebreak; SysInfo seeding; full error paths + cover-trace; per-song unit of work; exponential-backoff retries; CrashReporter; throttled CommitCopy + memory pressure relief + cover fingerprints; transcoder output reservations (Bug #13); iPod playlist management; podcasts + audiobooks |
| Sync UI | Device sync progress pane; OrganizeQueue (queued jobs, cancel, multi-job pane) |
| Look | StyleSheetLoader fixes; BlockAnalyzer palette fix; Claude theme |

## 14.2 Syncing with upstream

```bash
git fetch upstream && git fetch origin
git tag -f backup/master-pre-upstream-sync master        # cheap undo
git switch -c sync/upstream-<version> master
git rebase upstream/master                                # resolve conflict by conflict, see 14.3
./install-macos.sh clean && ./install-macos.sh build      # may need a deps refresh first, see 11 §11.0
# run the app / tests, then
git switch master && git reset --hard sync/upstream-<version>
git push --force-with-lease origin master                 # history was rewritten
```

Before resolving a conflict in a fork fix, check whether upstream fixed the same thing: `git log <merge-base>..upstream/master -- <file>`. Prefer upstream's version and keep only what the fork adds; say so in the fork commit's message.

## 14.3 What the 2026-10 sync (575 upstream commits) ran into

- **Upstream fixed two fork bugs on its own**: Bug #5 (write the iTunesDB after partial failures, `6ca0c9698`) and Bug #14 (`RemoveTrackFromITunesDb`, `37a73e802`). Kept upstream's code.
- **`CMakeLists.txt`**: `optional_source(HAVE_X SOURCES … HEADERS …)` became `if(HAVE_X) list(APPEND SOURCES …) list(APPEND HEADERS …) endif()`. Fork additions to optional blocks have to be redone in the new form.
- **`Organize`**: new `original_roots` constructor argument, `Finished(…, bool success)`, `UniqueFilenames` (case-insensitive unique names), Organize deletes itself when its thread finished. Fork code must not touch an `Organize` after `Finished`.
- **`Transcoder::GetFile`**: upstream fixed chained `.arg()` substituting into filenames containing `%1` (`4a0c7c06f`); the fork's Bug #13 rewrite of the same function had to adopt the multi-arg `.arg()`.
- **macOS deps**: upstream requires `uchardet` and switched to `macdeploytool`; the deps tarball had to be refreshed and the patched libgpod rebuilt (11 §11.0, §11.7).
- Mechanical resolutions were checked by building; one silent mistake (a line duplicated by a scripted resolution) was caught only by re-reading the resolved function. Re-read every resolved hunk.

## 14.4 Verifying UI changes without screen recording permission

macOS doesn't let a terminal session capture other apps' windows. To look at the real main window, temporarily add a hook that resizes the `MainWindow`, waits a few seconds, `grab()`s it (and each `FancyTabWidget` tab) to PNG and quits, then run the unbundled build with `QT_QPA_PLATFORM=offscreen`. Skip `mac::EnableFullScreen` in that mode (it needs a native window). The app uses the real profile: back up `~/Library/Application Support/Strawberry/Strawberry/strawberry.db` and `defaults export org.strawberrymusicplayer.Strawberry` first and restore both afterwards, since a newer build migrates the database schema. Never commit the hook.
