# Installing Strawberry on Windows

[`install-windows.sh`](../install-windows.sh) builds this fork of Strawberry from source and installs it for the current user, **with iPod sync**, which the official Windows builds don't have.
It needs Windows 10 or 11 (x64) and about 3 GB of disk space for MSYS2 and the build.

## Quick start

From cmd or PowerShell, in the repository folder:

```bat
install-windows.cmd
```

This installs [MSYS2](https://www.msys2.org/) with winget if it isn't there, then runs the whole build: the first run downloads about 1.5 GB of packages and compiles for 15 to 30 minutes.
When it finishes, start **Strawberry** from the Start Menu.

Already in an MSYS2 **UCRT64** shell? Run the script directly:

```bash
./install-windows.sh            # same as: deps, build, install
```

## Commands

| Command | What it does |
|---------|--------------|
| `deps` | Installs the MSYS2 packages, then builds getopt-win and the fork's patched libgpod |
| `libgpod` | Rebuilds only libgpod |
| `configure` | Runs CMake; stops if iPod support didn't get enabled |
| `build` | Configures if needed, then compiles |
| `bundle` | Collects the app and every DLL and plugin it needs in `build-windows\dist`, then checks nothing is missing |
| `install` | Bundles, then copies it to the install folder and adds a Start Menu shortcut |
| `uninstall` | Removes the installed app and shortcuts, and asks whether to remove your settings and collection |
| `purge` | Uninstalls, removes settings, collection, the build folder and the libgpod/getopt-win builds |
| `clean` | Removes the build folder |
| `status`, `doctor` | Show what's installed / check the environment |
| `all` | `deps`, `build`, `install` (the default) |

Options: `--prefix DIR` (install folder, default `%LOCALAPPDATA%\Programs\Strawberry`), `--build-dir DIR`, `--build-type Debug` (also shows a console window), `--jobs N`, `--refresh-deps` (pull and rebuild libgpod; with `install-windows.cmd` it also updates MSYS2 itself), `--libgpod-src DIR` (use your own libgpod checkout), `--desktop-shortcut`, `-y`, `-v`.

After changing code, `./install-windows.sh build && ./install-windows.sh install -y` rebuilds incrementally and reinstalls.

## What you get

Everything the official Windows installer ships (Tidal, Qobuz, Subsonic, Spotify, Plex, Jellyfin, Discord Rich Presence, scrobbling, moodbar, waveform, EBU R 128 loudness, Chromaprint fingerprinting and tag fetching, the same GStreamer plugins), plus:

- **iPod Classic / Nano / Mini sync**: copy music, podcasts and audiobooks with artwork, manage iPod playlists, and eject. Connect the iPod in disk mode; it appears under **Devices**.
- The fork's sync queue, sync progress pane and Claude theme.

Not included: Audio CD playback and MTP devices (both also off in the official Windows builds), the built-in updater, and file associations.

## Where things go

| What | Where |
|------|-------|
| The app | `%LOCALAPPDATA%\Programs\Strawberry` |
| Settings | Registry, `HKCU\Software\Strawberry` |
| Collection database, caches, logs | `%LOCALAPPDATA%\Strawberry` |
| MSYS2 | `C:\msys64` |
| libgpod and getopt-win builds | `C:\msys64\opt\strawberry-install` |

The installed app doesn't need MSYS2 to run: you can copy `%LOCALAPPDATA%\Programs\Strawberry` to another PC.

## Troubleshooting

- **pacman reports 404 errors**: the package database is old. Run `install-windows.cmd --refresh-deps`.
- **"Run this from an MSYS2 UCRT64 shell"**: you opened the MSYS or MINGW64 shell. Use the "MSYS2 UCRT64" shortcut, or `install-windows.cmd`.
- **The iPod doesn't show up**: make sure it's in disk mode and has a drive letter in Explorer. Run a Debug build (`--build-type Debug`) and look for `Device added Win/...` in the console.
- **The iPod shows "No Music" after a sync**: the device ID didn't contain the iPod's serial number, which the iTunesDB signature needs. The console shows `cannot derive FirewireGuid from unique_id`; please report it with that line.

Developer notes (how it works, what has been verified): [`.ai/15-windows.md`](../.ai/15-windows.md).
