# AI Reference for this Strawberry Fork

Start here. Read only the file(s) the task needs: the table below routes you, the landmines list is the short version of everything this fork learned the hard way.

**Fork:** `origin` = `kuthiala/strawberry`, `upstream` = `strawberrymusicplayer/strawberry`. The fork is upstream `master` plus a linear stack of fork commits (iPod sync hardening, sync queue, Claude theme, macOS tooling, these docs); see [`14-fork-maintenance.md`](./14-fork-maintenance.md).
**Platform this fork is developed on:** macOS (Intel, x86_64), built with `./install-macos.sh` against the upstream deps tarball in `/opt/strawberry_macos_<arch>_release`.
Windows builds (with iPod sync, unlike upstream's) use `install-windows.cmd` / `./install-windows.sh` in MSYS2 UCRT64, see [`15-windows.md`](./15-windows.md).

## Where to look

| Task | Read |
|------|------|
| What Strawberry is, repo layout | [`01-project-overview.md`](./01-project-overview.md) |
| How the app is wired (Application, threads, MainWindow) | [`02-architecture.md`](./02-architecture.md) |
| Building / tests (generic) | [`03-build-and-test.md`](./03-build-and-test.md) |
| Code style, Qt idioms, commit message format | [`04-coding-conventions.md`](./04-coding-conventions.md) |
| Finding the module for a feature | [`05-module-guide.md`](./05-module-guide.md), then [`08-quick-reference.md`](./08-quick-reference.md) |
| Database schema, `Song` | [`06-data-model.md`](./06-data-model.md) |
| Adding a provider / setting / file / test | [`07-common-workflows.md`](./07-common-workflows.md) |
| Terms | [`09-glossary.md`](./09-glossary.md) |
| **Anything iPod** (copy, artwork, playlists, podcasts, audiobooks) | [`10-ipod-sync.md`](./10-ipod-sync.md) — rules + bug index. Full bug write-ups: [`history/ipod-bug-diary.md`](./history/ipod-bug-diary.md) (open only for a matching symptom) |
| **macOS build / install / crash logs / libgpod fork** | [`11-macos-dev-loop.md`](./11-macos-dev-loop.md) (§11.0 first) |
| **Copy-to-device queue, sync pane, cancelling** | [`12-sync-queue.md`](./12-sync-queue.md) |
| **Colours, stylesheet, sidebar look, Appearance settings** | [`13-claude-theme.md`](./13-claude-theme.md) |
| **Pulling upstream, rebasing, commit layout** | [`14-fork-maintenance.md`](./14-fork-maintenance.md) |
| **Windows build, `WindowsDeviceLister`, iPod on Windows** | [`15-windows.md`](./15-windows.md) |
| iPod diagnostics without Strawberry | [`tools/`](./tools/) |

`CLAUDE.md` in the repo root is upstream's generic guide; this directory is the fork-specific layer on top.

## Facts that are easy to get wrong

- Qt 6 (≥ 6.4; the macOS deps ship 6.12), C++17, CMake ≥ 3.25. **Every source file is listed in the top-level `CMakeLists.txt`** (`SOURCES`/`HEADERS`, optional features in `if(HAVE_X) list(APPEND …)` blocks; the old `optional_source()` helper no longer exists). New `.ui`/resources: `.ui` files go in the UI list, resources in `data/data.qrc`.
- Code style: `Q_SIGNALS`/`Q_SLOTS`/`Q_EMIT`, no `foreach`, `u"…"_s` / `"…"_L1` literals, comments are long single lines (one sentence per line). Commit subject `ClassName: Short explanation`.
- DB schema is version **30** (`data/schema/schema.sql`, migrations `schema-N.sql`).
- `QString("%1/%2").arg(a).arg(b)` is a bug when `a` can contain `%1`: use one multi-arg `.arg(a, b)`.

## Landmines (short version of past incidents)

1. **App crashes within ~100 ms of launch on macOS** → not your code. The bundle references `/opt/...` libs (duplicate QtCore). Run `./install-macos.sh bundle` then `install -y`. Check: `otool -L /Applications/strawberry.app/Contents/MacOS/strawberry | grep /opt/strawberry_macos` must print nothing. [`11 §11.15`](./11-macos-dev-loop.md#1115-post-mortem-the-duplicate-qtcore-crash-read-this-first-if-the-app-wont-launch)
2. **After pulling upstream, configure fails on a missing package** (e.g. `uchardet`) → the deps tarball is stale: `./install-macos.sh clean && ./install-macos.sh --refresh-deps -y`. The script rebuilds the patched libgpod after every extraction (the tarball ships the stock one). [`11 §11.0`](./11-macos-dev-loop.md#110-what-changed-with-upstream-1231-2026-10)
3. **libgpod changes don't reach the app** → the install audit compares the bundle with `/opt/.../lib/libgpod.dylib` (Bug #12). `install-macos.sh` builds `.idea/strawberry-libgpod` (or a clone of `kuthiala/strawberry-libgpod`) into `/opt` whenever its commit or diff changes; run `./install-macos.sh libgpod` to force it. Uncommitted libgpod edits count too. Verify the deployed lib: `strings /Applications/strawberry.app/Contents/Frameworks/libgpod.dylib | grep -c g_try_realloc` > 0.
4. **Source fix "doesn't work"** → check the installed binary is newer than your edit (`ls -la /Applications/strawberry.app/Contents/MacOS/strawberry`); `build` alone doesn't update `/Applications`.
5. **iPod: never write the iTunesDB per song**, never `itdb_track_remove` directly, always write the DB at the end even after failures, keep SysInfo seeded. All 13 rules: [`10 §10.0`](./10-ipod-sync.md#100-rules-each-one-was-learned-from-a-bug--dont-break-them).
6. **Never start an `Organize` directly** — enqueue an `OrganizeQueue::JobRequest`. Two Organizes on one iPod deadlock one thread on `db_busy_`, and eject-after unmounts the device under the other. [`12`](./12-sync-queue.md)
7. **Theme:** no `QSlider` rules and no plain-`QWidget` backgrounds in `claude.css`, no SVGs in stylesheets (no SVG plugin in the macOS deps), `%palette-` roles must be replaced longest-first. [`13 §13.3`](./13-claude-theme.md#133-donts-each-breaks-something)
8. **Crash triage:** newest Strawberry crash log `ls -t ~/Library/Logs/Strawberry/strawberry-crash-*.log | head -1` (short), macOS report `~/Library/Logs/DiagnosticReports/strawberry-*.ips` (authoritative, look at `ktriageinfo`). [`11 §11.16`](./11-macos-dev-loop.md#1116-crash-logs)
9. **Windows iPod IDs carry the FirewireGuid**: `WindowsDeviceLister` IDs are `Win/<USB serial>` because `GPodDevice::EnsureIpodSysInfo` reads the GUID from the unique ID, and iPods are mounted at `E:` without a trailing slash because `Song::InitFromItdb` appends `/iPod_Control/...`. Changing either breaks iPod Classic syncs. [`15 §15.2`](./15-windows.md#152-pieces-that-exist-only-for-windows)
10. **Upstream may already have fixed your bug** — when rebasing, upstream independently fixed iPod Bugs #5 and #14; their versions were kept. Check `git log upstream/master -- <file>` before re-applying a fork fix. [`14`](./14-fork-maintenance.md)

## Keep this directory current

When you change architecture, add a module, change the build or find a new failure mode: update the matching file here, add a landmine line above if it cost real time, and put long investigations in `history/` rather than in the reference files.
