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

#ifndef CRASHREPORTER_H
#define CRASHREPORTER_H

#include "config.h"

#include <QString>

// Lightweight, async-signal-safe crash reporter that writes a brief
// `strawberry-crash-<pid>-<epoch>.log` next to the regular Strawberry log
// files whenever the process receives a fatal signal (SIGSEGV / SIGBUS /
// SIGFPE / SIGILL / SIGABRT) or hits std::terminate.
//
// We do this *in addition to* (not instead of) macOS's native
// `~/Library/Logs/DiagnosticReports/strawberry-*.ips` capture, because the
// .ips files are the authoritative crash reports (full thread state, fully
// symbolicated) but:
//
//   1. They are not obvious to users (hidden under ~/Library, and a
//      different directory from where Strawberry writes its own logs).
//   2. They are only emitted on signals that the kernel treats as fatal;
//      a deliberate abort() from std::terminate may or may not produce one
//      depending on the macOS version.
//   3. They are big (50–100 KB JSON) and contain a lot of OS-level noise
//      that's irrelevant for diagnosing a Strawberry-internal bug.
//
// The companion log we write here is small (a couple of KB), contains the
// Strawberry version, the signal that killed us, a UNIX backtrace from
// `backtrace_symbols_fd`, and points the reader at the matching .ips on
// macOS / `coredumpctl` on Linux. After writing it we restore the default
// signal handler and re-raise so the kernel's crash machinery still fires
// — i.e. the .ips file is still produced and any attached debugger still
// stops at the faulting instruction. We never call non-signal-safe code
// (no QString construction, no g_*, no std::ostream) inside the handler.
namespace CrashReporter {

// Install the signal handlers. Must be called from `main()` *after*
// QCoreApplication::setOrganizationName / setApplicationName (so the
// destination directory resolves to the same place as the rest of
// Strawberry's logs) but *before* any heavy initialization that could
// itself crash. Idempotent — calling it twice is a no-op.
void Init();

// Returns the directory that crash logs are written to. Useful for the
// "where do I find the crash log?" UI hint we surface in the About dialog
// and in `.ai/11-macos-dev-loop.md`.
QString CrashLogDirectory();

// Helper exposed for tests / for code that wants to force-write a crash
// report without actually crashing (e.g. on an unrecoverable error
// detected at the Strawberry layer). Safe to call from non-signal
// contexts; uses QString / Qt I/O.
void WriteSyntheticCrashLog(const QString &reason);

}  // namespace CrashReporter

#endif  // CRASHREPORTER_H