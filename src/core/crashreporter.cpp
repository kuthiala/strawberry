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
#include "version.h"

#include "crashreporter.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#include <QtGlobal>

#ifdef Q_OS_UNIX
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#ifdef HAVE_BACKTRACE
#  include <execinfo.h>
#endif

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QString>
#include <QSysInfo>

#include "core/logging.h"
#include "core/standardpaths.h"

using namespace Qt::Literals::StringLiterals;

namespace {

// The crash handler must be async-signal-safe. That means: no malloc, no
// QString, no fprintf to a FILE*, no Qt anything, and — crucially — no
// `localtime_r`/`gmtime_r` (they are *not* on POSIX's async-signal-safe
// list because they touch the tzdata cache).
//
// To produce an ISO-8601 human-readable filename like
// `strawberry-crash-2026-06-30T18-25-56-12345.log` we therefore split the
// work:
//
//   • At Init() time we call `QDateTime::currentDateTime()` (safe — we're
//     in a normal Qt context), compute the epoch at process-start, and
//     stash both:
//       - the epoch (`sInitEpoch`) — a signed integer, safely readable
//         from the signal handler
//       - a fully-formed prefix through the DATE portion of the process
//         start-day, `sCrashLogPrefixThroughDate`, which ends in
//         "/strawberry-crash-YYYY-MM-DDT".
//
//   • At crash time the handler adds:
//       - the seconds-since-init offset (via `time(nullptr) -
//         sInitEpoch`, both async-signal-safe)
//       - re-derived HH-MM-SS by simple divmod on the day-of-init epoch
//         + offset
//     …to synthesise the full timestamp.
//
// The pre-computed date is only correct *for the calendar day the
// process started on*. If the process runs for >24 h (unlikely for
// Strawberry, but possible on a media-server-style long-run), the
// filename's date field can drift by up to 23 h from the actual crash
// wall-clock. That's an acceptable trade — the epoch-derived HH-MM-SS
// portion is still monotonically correct, and the file's `mtime`
// (updated by `write(2)`) gives the true wall-clock time to anyone who
// cares. In practice the .ips filename in
// `~/Library/Logs/DiagnosticReports/` is the authoritative wall-clock
// source anyway (§11.16), and it always uses the current day.
//
// All static buffers are zeroed at process start; `Init()` populates
// them once. `sInitialized` is checked first in the handler so a signal
// that arrives before Init() finishes is forwarded to the default handler
// without touching uninitialised memory.

constexpr size_t kPathBufLen = 1024;
// prefix "…/strawberry-crash-YYYY-MM-DDT" fits comfortably in 800 bytes
// even for deeply-nested $HOME paths (macOS FullDiskAccess sandboxed
// homes are ~260 chars max).
constexpr size_t kPrefixBufLen = 800;
constexpr size_t kPidBufLen = 32;

char sCrashLogPrefixThroughDate[kPrefixBufLen] = {0};  // "…/strawberry-crash-YYYY-MM-DDT"
char sPidBuf[kPidBufLen] = {0};                        // "12345"
std::atomic<bool> sInitialized{false};

// The epoch (seconds) at which the process started AND at which the
// pre-formatted date in sCrashLogPrefixThroughDate is valid. We
// (time(nullptr) - sInitEpoch) at crash time to derive HH-MM-SS.
std::atomic<time_t> sInitEpoch{0};
// Seconds-since-local-midnight of the process-start moment, so we can
// compose HH-MM-SS at crash time by adding
// (time(nullptr) - sInitEpoch) and taking modulo 86400.
std::atomic<int> sInitSecondsSinceMidnight{0};

// Header banner ASCII for the crash log. Written once per crash. Static
// rather than constexpr so we have a `const char *` we can pass to
// write(2) without needing to recompute its length each call.
const char kCrashBanner[] =
    "================ Strawberry crash log ================\n";
const char kStrawberryVersion[] =
    "Strawberry version: " STRAWBERRY_VERSION_DISPLAY "\n";
const char kBacktraceBanner[] =
    "--- Backtrace ---\n";
const char kFooterBanner[] =
    "--- End of crash log; re-raising signal so the OS can take its own snapshot. ---\n";

// Async-signal-safe int-to-decimal-string. Writes at most `buf_len-1`
// digits into `buf` and nul-terminates. Returns the number of characters
// written (excluding the NUL).
size_t SafeUintToString(unsigned long long value, char *buf, size_t buf_len) {

  if (buf_len == 0) return 0;

  if (value == 0) {
    if (buf_len < 2) {
      buf[0] = '\0';
      return 0;
    }
    buf[0] = '0';
    buf[1] = '\0';
    return 1;
  }

  char tmp[32];
  size_t n = 0;
  while (value > 0 && n < sizeof(tmp)) {
    tmp[n++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }

  if (n + 1 > buf_len) n = buf_len - 1;
  for (size_t i = 0; i < n; ++i) {
    buf[i] = tmp[n - 1 - i];
  }
  buf[n] = '\0';

  return n;

}

// Async-signal-safe zero-padded decimal formatter. Writes EXACTLY
// `width` characters (no NUL) starting at `buf`. Caller ensures there's
// room. Values greater than `10^width` are truncated to the last
// `width` decimal digits (an acceptable failure mode for HH/MM/SS which
// are always < 100).
void SafeZeroPad(unsigned int value, char *buf, size_t width) {
  for (size_t i = width; i > 0; --i) {
    buf[i - 1] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
}

// Async-signal-safe `write` wrapper that ignores short writes / EINTR.
void SafeWrite(int fd, const char *s, size_t n) {
  if (fd < 0 || s == nullptr || n == 0) return;
  ssize_t written = 0;
  while (n > 0) {
    written = write(fd, s, n);
    if (written < 0) {
      if (errno == EINTR) continue;
      return;
    }
    s += written;
    n -= static_cast<size_t>(written);
  }
}

void SafeWriteCStr(int fd, const char *s) {
  if (s == nullptr) return;
  SafeWrite(fd, s, std::strlen(s));
}

// Build the destination filename for *this* crash and open it. Returns
// a valid fd on success, -1 on failure. Async-signal-safe.
//
// Filename shape (per the June 2026 spec change requested by
// aniruddhkuthiala): the DATE + TIME portion comes FIRST and the PID
// comes AFTER — this lets `ls -l` in the crash directory sort logs
// chronologically without needing `-t`. Format is:
//
//     strawberry-crash-YYYY-MM-DDTHH-MM-SS-<pid>.log
//
// (Colons `:` are omitted from the time portion because they are
// forbidden in filenames on FAT/exFAT-mounted external volumes and
// interpreted as path separators by some shells' completion.)
int OpenCrashLogFd(time_t now_epoch) {

#ifdef Q_OS_UNIX
  if (sCrashLogPrefixThroughDate[0] == '\0') return -1;

  char path[kPathBufLen];

  // 1. prefix through date, e.g. "/…/strawberry-crash-2026-06-30T"
  size_t pos = 0;
  const size_t prefix_len = std::strlen(sCrashLogPrefixThroughDate);
  if (prefix_len >= sizeof(path)) return -1;
  std::memcpy(path, sCrashLogPrefixThroughDate, prefix_len);
  pos = prefix_len;

  // 2. HH-MM-SS derived from init-time seconds-since-midnight + elapsed
  //    seconds. Wrapped mod 86400 so a run that crosses midnight still
  //    yields sane HH values (see the caveat in the top-of-file comment
  //    about the DATE portion drifting past 24h runs).
  const time_t init_epoch = sInitEpoch.load(std::memory_order_acquire);
  const int init_secs_of_day =
      sInitSecondsSinceMidnight.load(std::memory_order_acquire);
  long elapsed = static_cast<long>(now_epoch) - static_cast<long>(init_epoch);
  if (elapsed < 0) elapsed = 0;
  long secs_of_day = (static_cast<long>(init_secs_of_day) + elapsed) % 86400L;
  if (secs_of_day < 0) secs_of_day += 86400L;
  const unsigned int hh =
      static_cast<unsigned int>(secs_of_day / 3600L);
  const unsigned int mm =
      static_cast<unsigned int>((secs_of_day / 60L) % 60L);
  const unsigned int ss = static_cast<unsigned int>(secs_of_day % 60L);

  // Need room for "HH-MM-SS-<pid>.log" = 8 + 1 + pidlen + 4.
  const size_t pid_len = std::strlen(sPidBuf);
  const size_t tail_len = 8 + 1 + pid_len + 4;
  if (pos + tail_len + 1 >= sizeof(path)) return -1;

  SafeZeroPad(hh, path + pos, 2); pos += 2;
  path[pos++] = '-';
  SafeZeroPad(mm, path + pos, 2); pos += 2;
  path[pos++] = '-';
  SafeZeroPad(ss, path + pos, 2); pos += 2;
  path[pos++] = '-';

  std::memcpy(path + pos, sPidBuf, pid_len);
  pos += pid_len;

  std::memcpy(path + pos, ".log", 4);
  pos += 4;
  path[pos] = '\0';

  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#else
  Q_UNUSED(now_epoch)
  return -1;
#endif
}

// Translate a signal number to a short human-readable name. Returns a
// pointer to a static string; never NULL.
const char *SignalName(int signum) {
  switch (signum) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)\n";
    case SIGABRT: return "SIGABRT (aborted)\n";
    case SIGFPE:  return "SIGFPE (arithmetic error)\n";
    case SIGILL:  return "SIGILL (illegal instruction)\n";
    case SIGBUS:  return "SIGBUS (bus error)\n";
    case SIGPIPE: return "SIGPIPE (broken pipe)\n";
    default:      return "(unknown signal)\n";
  }
}

// The fatal-signal handler itself. See the comment block at the top of
// this file for the async-signal-safety contract.
extern "C" void HandleFatalSignal(int signum) {

  if (!sInitialized.load(std::memory_order_acquire)) {
    signal(signum, SIG_DFL);
    raise(signum);
    return;
  }

  const time_t now_epoch = time(nullptr);
  const int fd = OpenCrashLogFd(now_epoch);
  if (fd >= 0) {
    SafeWriteCStr(fd, kCrashBanner);
    SafeWriteCStr(fd, kStrawberryVersion);

    SafeWriteCStr(fd, "Process ID: ");
    SafeWriteCStr(fd, sPidBuf);
    SafeWriteCStr(fd, "\n");

    // We include the raw epoch too — the filename encodes ISO datetime
    // (which is easier to read at a glance) but downstream log
    // aggregators may prefer the numeric form.
    SafeWriteCStr(fd, "Epoch: ");
    char epoch_buf[32];
    SafeUintToString(static_cast<unsigned long long>(now_epoch),
                     epoch_buf, sizeof(epoch_buf));
    SafeWriteCStr(fd, epoch_buf);
    SafeWriteCStr(fd, "\n");

    SafeWriteCStr(fd, "Signal: ");
    SafeWriteCStr(fd, SignalName(signum));

#ifdef HAVE_BACKTRACE
    SafeWriteCStr(fd, kBacktraceBanner);
    void *callstack[128];
    const int n_frames = backtrace(callstack, 128);
    backtrace_symbols_fd(callstack, n_frames, fd);
#else
    SafeWriteCStr(fd, "(backtrace() not available on this platform)\n");
#endif

    SafeWriteCStr(fd, kFooterBanner);

    (void)fsync(fd);
    (void)close(fd);
  }

  signal(signum, SIG_DFL);
  raise(signum);

}

void HandleTerminate() {
  if (sInitialized.load(std::memory_order_acquire)) {
    HandleFatalSignal(SIGABRT);
  }
  std::abort();
}

// Delete strawberry-crash-*.log files whose mtime is older than the
// retention window. Not async-signal-safe; called from Init() at
// process start, well before any handlers are installed.
//
// The retention window (180 days by default) is chosen to be long
// enough to survive "user runs a big sync, doesn't notice the crash
// for weeks, comes back to debug" but short enough that a machine with
// a chronically-crashing build doesn't accumulate gigabytes over years.
// The pruner is best-effort: any file it can't read or delete is
// silently skipped so a bad file can't block startup.
void PruneOldCrashLogs(const QString &dir, int max_age_days) {

  if (max_age_days <= 0) return;

  QDir d(dir);
  if (!d.exists()) return;

  // Match both the current ISO-datetime schema AND the previous
  // `<pid>-<epoch>` schema, so upgrading users don't leak old-format
  // logs forever. QFileInfoList lets us key on mtime (portable) rather
  // than parse the filename.
  const QStringList patterns{u"strawberry-crash-*.log"_s};
  const QFileInfoList entries = d.entryInfoList(patterns, QDir::Files, QDir::NoSort);

  const QDateTime cutoff = QDateTime::currentDateTime().addDays(-max_age_days);
  int pruned = 0;
  qint64 pruned_bytes = 0;
  for (const QFileInfo &fi : entries) {
    if (fi.lastModified() >= cutoff) continue;
    const qint64 sz = fi.size();
    if (QFile::remove(fi.absoluteFilePath())) {
      ++pruned;
      pruned_bytes += sz;
    }
  }

  if (pruned > 0) {
    qLog(Info) << "CrashReporter: pruned" << pruned
               << "crash log(s) older than" << max_age_days << "days ("
               << pruned_bytes << "bytes reclaimed) from" << dir;
  }

}

}  // namespace

namespace CrashReporter {

QString CrashLogDirectory() {

  // We park our crash logs alongside the regular Strawberry log file
  // (`strawberry-stdout.txt`) so users only have to remember *one*
  // location. On macOS that's `~/Library/Logs/Strawberry/`; on Linux
  // it's `~/.local/share/Strawberry/Strawberry/` (StandardPaths returns
  // AppDataLocation with the org subdir).
  //
  // We intentionally do NOT use a system-wide directory: a crash logger
  // that needs root to write its output is no use to anyone.
#ifdef Q_OS_MACOS
  // `~/Library/Logs/<org>/<app>/` matches what `open -a Strawberry` uses
  // for stdout redirection (see `.ai/11-macos-dev-loop.md §11.6`).
  const QString home = QDir::homePath();
  return home + u"/Library/Logs/Strawberry"_s;
#else
  return StandardPaths::WritableLocation(StandardPaths::StandardLocation::AppLocalDataLocation)
      + u"/crashlogs"_s;
#endif

}

void Init() {

  if (sInitialized.load(std::memory_order_acquire)) return;

  // Resolve and create the destination directory. This *can* fail (no
  // disk space, $HOME unwritable, etc.); if it does we just don't install
  // the handlers — the OS-native crash capture (macOS .ips, Linux core
  // dump) still works.
  const QString dir = CrashLogDirectory();
  if (!QDir().mkpath(dir)) {
    qLog(Warning) << "CrashReporter: could not create" << dir
                  << "- per-strawberry crash logs disabled. The OS-native"
                  << "crash report path still works (see .ai/11-macos-dev-loop.md).";
    return;
  }

  // Prune anything older than 6 months so runaway crash loops don't fill
  // the disk. 180 days is the fixed retention window; see the
  // PruneOldCrashLogs() comment for the rationale.
  constexpr int kRetentionDays = 180;
  PruneOldCrashLogs(dir, kRetentionDays);

  // Pre-format the "prefix through date" string that the signal handler
  // will use as the filename base. Format: "…/strawberry-crash-YYYY-MM-DDT".
  const QDateTime now_local = QDateTime::currentDateTime();
  const QString date_part = now_local.date().toString(u"yyyy-MM-dd"_s);
  const QString prefix_qstr = dir + u"/strawberry-crash-"_s + date_part + u"T"_s;
  const QByteArray prefix_utf8 = QFile::encodeName(prefix_qstr);
  if (static_cast<size_t>(prefix_utf8.size()) + 1 > sizeof(sCrashLogPrefixThroughDate)) {
    qLog(Warning) << "CrashReporter: log path too long (" << prefix_utf8.size()
                  << "bytes) - disabling per-strawberry crash logs.";
    return;
  }
  std::memcpy(sCrashLogPrefixThroughDate, prefix_utf8.constData(),
              static_cast<size_t>(prefix_utf8.size()));
  sCrashLogPrefixThroughDate[prefix_utf8.size()] = '\0';

  // Stash the init epoch + seconds-since-midnight so the handler can
  // compute HH-MM-SS without calling localtime_r (which is NOT on the
  // POSIX async-signal-safe list — see the top-of-file comment block).
  const QTime now_time = now_local.time();
  const int secs_of_day = now_time.hour() * 3600
                        + now_time.minute() * 60
                        + now_time.second();
  sInitEpoch.store(static_cast<time_t>(now_local.toSecsSinceEpoch()),
                   std::memory_order_release);
  sInitSecondsSinceMidnight.store(secs_of_day, std::memory_order_release);

  // Stash the PID once, so the handler doesn't need to format it.
#ifdef Q_OS_UNIX
  SafeUintToString(static_cast<unsigned long long>(getpid()),
                   sPidBuf, sizeof(sPidBuf));
#else
  std::strncpy(sPidBuf, "0", sizeof(sPidBuf) - 1);
#endif

  // Publish the buffers before installing handlers, so a racing fatal
  // signal sees them populated rather than half-written.
  sInitialized.store(true, std::memory_order_release);

#ifdef Q_OS_UNIX
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = &HandleFatalSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  // Don't install a handler for SIGPIPE; we want SIGPIPE writes to
  // continue producing EPIPE errors that the Qt/glib network code
  // already handles cleanly, not a process death.
  for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
    if (sigaction(sig, &sa, nullptr) != 0) {
      qLog(Warning) << "CrashReporter: failed to install handler for signal"
                    << sig << "- crash log for that signal may be missing.";
    }
  }
#endif

  std::set_terminate(&HandleTerminate);

  qLog(Info) << "CrashReporter: writing crash logs to" << dir
             << "(filename pattern: strawberry-crash-<ISO-datetime>-<pid>.log,"
             << "auto-prune >" << kRetentionDays << "days).";

}

void WriteSyntheticCrashLog(const QString &reason) {

  const QString dir = CrashLogDirectory();
  if (!QDir().mkpath(dir)) return;

  const QDateTime now_local = QDateTime::currentDateTime();
  // Match the new signal-handler filename schema so `ls` sorting works
  // consistently for both real and synthetic entries.
  const QString iso = now_local.toString(u"yyyy-MM-ddTHH-mm-ss"_s);
#ifdef Q_OS_UNIX
  const qint64 pid = static_cast<qint64>(getpid());
#else
  const qint64 pid = 0;
#endif
  const QString path = dir
      + u"/strawberry-crash-"_s + iso
      + u"-"_s + QString::number(pid)
      + u".log"_s;

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    qLog(Warning) << "CrashReporter: could not write synthetic crash log to" << path;
    return;
  }

  f.write(QByteArrayLiteral(
      "================ Strawberry synthetic crash log ================\n"));
  f.write(QByteArrayLiteral("Strawberry version: " STRAWBERRY_VERSION_DISPLAY "\n"));
  f.write("Process ID: ");
  f.write(QByteArray::number(pid));
  f.write("\n");
  f.write("ISO datetime: ");
  f.write(iso.toUtf8());
  f.write("\n");
  f.write("Epoch: ");
  f.write(QByteArray::number(now_local.toSecsSinceEpoch()));
  f.write("\n");
  f.write("Reason: ");
  f.write(reason.toUtf8());
  f.write("\n");
  f.write("OS: ");
  f.write(QSysInfo::prettyProductName().toUtf8());
  f.write("\n");

  f.close();

  qLog(Warning) << "CrashReporter: wrote synthetic crash log to" << path
                << "- reason:" << reason;

}

}  // namespace CrashReporter