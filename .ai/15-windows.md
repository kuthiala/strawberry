# 15. Windows Build and iPod Support

Upstream's Windows builds (MSVC and MXE MinGW, see `.github/workflows/build.yaml`) configure with `-DENABLE_GPOD=OFF -DENABLE_GIO=OFF -DENABLE_MTP=OFF -DENABLE_AUDIOCD=OFF`, and upstream's MSVC dependency bundle has no libgpod, libplist or gdk-pixbuf.
This fork builds on Windows **with iPod sync** through MSYS2 UCRT64 and [`install-windows.sh`](../install-windows.sh).
User-facing guide: [`docs/INSTALL-Windows.md`](../docs/INSTALL-Windows.md).

## 15.1 Why MSYS2 and not upstream's MSVC deps

- MSYS2 UCRT64 packages every Strawberry dependency, the full GStreamer plugin set of the official installer (including `gstspotify` from `gst-plugins-rs`), plus libplist and gdk-pixbuf for libgpod.
- libgpod has always been a GCC/MinGW library on Windows; it doesn't build with MSVC without porting.
- Upstream already compiles Strawberry with MinGW GCC (`build-windows-mingw`, `-DBUILD_WERROR=ON`), so the C++ side is exercised on that toolchain.
- Missing from MSYS2 and built by the script into `/opt/strawberry-install/<MSYSTEM>/prefix` (inside the MSYS2 root, no spaces for pkg-config): the fork's patched libgpod (`kuthiala/strawberry-libgpod`) and **getopt-win** 2.42.0 (same version and SHA-256 as upstream's MXE/MSVC builds).
- Not built: qtsparkle (the updater would offer upstream installers, which lack the fork's changes).

## 15.2 Pieces that exist only for Windows

| What | Where | Why |
|------|-------|-----|
| `WindowsDeviceLister` | `src/device/windowsdevicelister.{h,cpp}` | GLib's Windows volume monitor reports no `GVolume`s (`GioLister::DeviceInfo::is_suitable()` needs one) and no hot-plug events, so `GioLister` sees nothing. This lister polls `GetLogicalDrives()` every 2 s, lists `DRIVE_REMOVABLE` volumes and `DRIVE_FIXED` ones on USB/SD/1394 buses, and ejects with lock + dismount + `IOCTL_STORAGE_EJECT_MEDIA` + `CM_Request_Device_EjectW` on the removable ancestor ("Safely Remove Hardware"). |
| Device IDs `Win/<USB serial>` | `WindowsDeviceLister::MakeUniqueId()` | `GPodDevice::EnsureIpodSysInfo()` takes the text after the first `/` of the unique ID as the iPod's FirewireGuid (rule 1 of [`10 §10.0`](./10-ipod-sync.md#100-rules-each-one-was-learned-from-a-bug--dont-break-them)). The serial comes from `IOCTL_STORAGE_QUERY_PROPERTY`; an iPod reports its 16-hex FirewireGuid there. Without a serial the ID falls back to `Win/volume-<volume serial>`. |
| iPod mount path `E:` (no slash) | `WindowsDeviceLister::MountPath()` | `Song::InitFromItdb()` builds `prefix + "/iPod_Control/..."`, so a trailing slash produced `E://iPod_Control/...` and broke path matching. Plain drives keep `E:/`, since bare `E:` means the current directory on E. |
| `CrashReporter` POSIX guards | `src/core/crashreporter.cpp` | `write`/`fsync`/`close`/`ssize_t`/`SIGBUS`/`SIGPIPE` don't exist on Windows. The signal-handler log file is Unix-only there (`OpenCrashLogFd()` returns -1), synthetic logs and `std::set_terminate` still work. |
| libgpod MinGW fixes | `kuthiala/strawberry-libgpod` commit "Fix building with MinGW for Windows" | `struct stat` passed to `g_stat()` (an error with GCC 14+), `#ifdef WIN32` which GCC doesn't define with `-std=c99` (so `sys/statvfs.h` and `sync()` were used), local variables named `_timezone`/`tzname` clashing with CRT macros, and the deprecated UCRT `timezone` variable. |

## 15.3 What `install-windows.sh` does

`deps` (pacman packages, getopt-win, libgpod) → `configure` (refuses to continue unless `config.h` has `HAVE_GPOD`) → `build` → `bundle` → `install`.

`bundle` builds `build-windows/dist/` from scratch every time, in the official installer's layout: `strawberry.exe` with `platforms/ styles/ tls/ sqldrivers/ imageformats/` (Qt), `gstreamer-plugins/` and `gio-modules/` (where `GstStartup::SetEnvironment()` looks when `USE_BUNDLE` is on), a `qt.conf` (otherwise Qt derives its prefix from `Qt6Core.dll` as if it were in `<prefix>/bin`) and Qt's translations.
DLL dependencies are resolved with `ntldd -R` until nothing new turns up; `libssl`/`libcrypto`/`libsoup` are copied explicitly because they're loaded with `LoadLibrary`.
The audit then fails the bundle if any shipped `.exe`/`.dll` needs a DLL that is neither in `dist/` nor part of Windows, and if `dist/libgpod.dll` differs from the one just built (the Windows version of macOS Bug #12).

`install` copies `dist/` to `%LOCALAPPDATA%\Programs\Strawberry` (no admin rights needed) after deleting the old copy, and makes a Start Menu shortcut.
Settings live in `HKCU\Software\Strawberry`, the collection database and caches in `%LOCALAPPDATA%\Strawberry`; `uninstall` asks before removing them.

`install-windows.cmd` is the one-step entry point from cmd/PowerShell: installs MSYS2 if missing (winget, else the official installer fetched with `curl.exe` and run unattended), runs `pacman -Syuu` twice on every run (from outside MSYS2, because a core update closes every MSYS2 process; it also stops a stale package database from causing 404s), then starts the script in UCRT64, whose default command does deps, build, bundle and install.
libgpod comes from `.idea/strawberry-libgpod` when it exists, `--libgpod-src`, or a clone of `kuthiala/strawberry-libgpod` that is fetched on every run, and is rebuilt when its commit changes, the same rules as on macOS.

## 15.4 How this was verified, and what wasn't

Developed on macOS without a Windows machine.
Verified by cross-compiling with Homebrew's `mingw-w64` (GCC 16, UCRT, same as MSYS2) against the real MSYS2 UCRT64 packages extracted into a sysroot:

- The patched libgpod builds and links into `libgpod.dll` with gdk-pixbuf, importing only MSYS2/UCRT DLLs; it still builds on macOS.
- `windowsdevicelister.cpp`, `crashreporter.cpp`, `devicemanager.cpp`, `main.cpp`, the iPod sources (`gpoddevice`, `gpodloader`, `gpodplaylistmanager`, `audiobookmerger`), `organize`/`organizequeue`, `transcoder`, `song.cpp` compile with the project's warning flags and a Windows `config.h`.
- The ntldd parsing and DLL classification in the script were unit-tested on sample output.

**Not yet run on Windows**: the full CMake configure/build in MSYS2, the bundle, and an actual iPod connected to a Windows PC (detection, sync, eject).
The first Windows run should check, in this order: `./install-windows.sh doctor`; that the CMake summary lists "Devices: iPod classic support"; that the app starts from the Start Menu on a machine without MSYS2 in PATH; that a connected iPod appears in Devices with a `Win/<16 hex digits>` ID (log line "Device added"); a copy to the iPod; eject; and that the iPod shows the copied tracks with artwork.
Update this section with what that run finds.

## 15.5 Known gaps

- **Audio CD** stays off, as upstream: libcdio's Windows device names (`D:`) don't fit the `cdda://<path>` URLs `CddaDevice`/`CddaSongLoader` build, and it was untestable here.
- **MTP** stays off: MSYS2 has no libmtp, and libmtp on Windows would need the device's driver replaced with WinUSB.
- **No auto-updater, no NSIS installer, no file associations** (the official installer registers them via `dist/windows/strawberry.nsi.in`; its file list names the MSVC/MXE DLLs, not MSYS2's).
