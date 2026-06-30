# 3. Build, Run, and Test

## TL;DR

```bash
# from the repo root
cmake -S . -B build
cmake --build build --parallel $(nproc)
sudo cmake --install build   # optional
```

To run tests:

```bash
cmake --build build --target run_strawberry_tests
# or equivalently
cd build && ctest -V
```

## Build Requirements

See [`README.md`](../README.md) for a definitive, up-to-date list. As of writing:

**Mandatory:**
- CMake ≥ 3.13
- C++17 compiler (GCC, Clang, MSVC)
- pkg-config or pkgconf
- Boost
- GLib, GObject
- Qt 6 ≥ 6.4 (Core, Concurrent, Gui, Widgets, Network, Sql; D-Bus on UNIX-not-macOS)
- SQLite ≥ 3.9
- GStreamer 1.x (base, audio, app, tag, pbutils; **plus** plugins-base / plugins-good — and optionally bad/ugly/libav for codecs)
- TagLib ≥ 1.12 (prefers ≥ 2.0)
- ICU
- KDSingleApplication ≥ 1.1.0
- ALSA (Linux only)

**Optional features** are auto-detected via `pkg_check_modules`/`find_package`. CMake will print an availability summary; the corresponding `HAVE_*` macros are written into `src/config.h` (generated from [`src/config.h.in`](../src/config.h.in)).

| CMake option / package | Enables                                       |
| ---------------------- | --------------------------------------------- |
| `libpulse`             | `HAVE_PULSE` — PulseAudio device finder       |
| `libchromaprint`       | `HAVE_CHROMAPRINT`, `HAVE_MUSICBRAINZ`        |
| `fftw3`                | `HAVE_GSTFASTSPECTRUM` — fast moodbar         |
| `libebur128`           | `HAVE_EBUR128` — loudness normalization       |
| `libcdio`              | `HAVE_AUDIOCD`                                |
| `libmtp`               | `HAVE_MTP`                                    |
| `libgpod` + gdk-pixbuf | `HAVE_GPOD` — iPod classic support            |
| `libsparsehash`        | `HAVE_STREAMTAGREADER`                        |
| Qt LinguistTools       | `HAVE_TRANSLATIONS`                           |
| `gio-2.0`              | `HAVE_GIO` device backend                     |
| `X11_xcb` + QX11App    | `HAVE_X11_GLOBALSHORTCUTS`                    |
| `Qt6DBus`              | `HAVE_DBUS`, `HAVE_MPRIS2`, `HAVE_KGLOBALACCEL_GLOBALSHORTCUTS`, `HAVE_UDISKS2` |
| `Sparkle` / `qtsparkle`| Software auto-updater on macOS / Windows      |

Look for `optional_component(…)` calls in [`CMakeLists.txt`](../CMakeLists.txt) — they're how features are gated.

## CMake Options You Might Actually Toggle

| Option                       | Default                              | What it does                              |
| ---------------------------- | ------------------------------------ | ----------------------------------------- |
| `-DBUILD_WERROR=ON`          | OFF                                  | Treat warnings as errors                  |
| `-DCMAKE_BUILD_TYPE=Release` | (empty)                              | Disables debug output, defines `NDEBUG`   |
| `-DENABLE_DEBUG_OUTPUT=OFF`  | derived from build type              | Compiles out `qDebug` etc.                |
| `-DUSE_BUNDLE=ON`            | ON for macOS/Windows                 | Bundle dependencies into the install      |
| `-DUSE_RPATH=ON`             | macOS                                | Use rpath for installed binary            |
| `-DINSTALL_TRANSLATIONS=ON`  | OFF                                  | Install `.qm` files (otherwise embedded)  |
| `-DENABLE_WIN32_CONSOLE=ON`  | ON in Debug                          | Keep Win32 console attached even in Release |
| `-DUSE_INSTALL_PREFIX=OFF`   | ON                                   | Stop looking under `CMAKE_INSTALL_PREFIX` for data |

## Adding / Removing Source Files

⚠️ **The top-level [`CMakeLists.txt`](../CMakeLists.txt) lists every `.cpp` and `.h` file explicitly** (in the `set(SOURCES …)` and `set(HEADERS …)` blocks, plus separate Mac/Win sections and `optional_source(...)` calls). When you add a file:

1. Add the `.cpp` to `SOURCES` (or to a guarded `optional_source(...)` if it's behind a feature flag).
2. Add the `.h` to `HEADERS` **if it contains `Q_OBJECT`** (otherwise it's not strictly required, but recommended for consistency).
3. If the file uses a `.ui`, add it to `UI` and the form will be processed by `qt_wrap_ui`.

The CMake helper [`cmake/OptionalSource.cmake`](../cmake/OptionalSource.cmake) wraps the conditional inclusion logic.

## Compiler Flags You Should Be Aware Of

From the top-level `CMakeLists.txt`:

```
-Wall -Wextra -Wpedantic -Wunused -Wshadow -Wundef -Wuninitialized
-Wredundant-decls -Wcast-align -Winit-self -Wmissing-include-dirs
-Wmissing-declarations -Wstrict-overflow=2 -Wunused-parameter
-Wformat=2 -Wdisabled-optimization
$<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
$<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
```

…plus the Qt hardening defines listed in [`01-project-overview.md`](./01-project-overview.md) ("Project Philosophy"). New code should compile clean against all of them.

## Running the App

### Linux

```bash
./build/strawberry          # from build dir
./build/strawberry --help   # CLI options (see core/commandlineoptions.cpp)
```

`KDSingleApplication` means a second invocation just sends commands to the running instance (play, queue, etc.).

### macOS

In Release/Bundle mode CMake builds a `.app` bundle; otherwise the binary is at `build/strawberry.app/Contents/MacOS/strawberry`.

### Windows

`build/strawberry.exe` (with `ENABLE_WIN32_CONSOLE` controlling whether a console window is attached).

## Test Suite

Tests use **GoogleTest** + **GMock** and live in [`tests/src/`](../tests/src/). The CMake target `strawberry_tests` builds and runs every test; `run_strawberry_tests` runs them under ctest.

| Test                                | Purpose                                                  |
| ----------------------------------- | -------------------------------------------------------- |
| `utilities_test.cpp`                | String/file/etc. helpers                                 |
| `concurrentrun_test.cpp`            | The internal `ConcurrentRun` helper                      |
| `mergedproxymodel_test.cpp`         | `MergedProxyModel` (used by Collection)                  |
| `sqlite_test.cpp`                   | Basic SQLite/QtSql sanity                                |
| `tagreader_test.cpp`                | TagLib I/O wrapper                                       |
| `collectionbackend_test.cpp`        | DB schema, migrations, CRUD                              |
| `collectionmodel_test.cpp` (GUI)    | Tree model logic                                         |
| `songplaylistitem_test.cpp`         | `SongPlaylistItem`                                       |
| `organizeformat_test.cpp`           | Filename-format expansion (Organize/Copy)                |
| `playlist_test.cpp` (GUI)           | The `Playlist` class                                     |
| `waveform*_test.cpp` (when enabled) | Waveform pipeline                                        |

Some tests need a GUI environment (`add_test_file(... true)` → links `test_gui_main`). On Linux CI you typically need `xvfb-run` for those.

There is also `lyrics_live_tests`, **not** part of the default test run — it hits real lyrics provider HTTP endpoints and may fail when an API changes:

```bash
cmake --build build --target lyrics_live_tests
./build/tests/lyrics_live_tests
```

### Test infrastructure

- [`tests/src/main.cpp`](../tests/src/main.cpp) — boots a `QApplication` (or `QCoreApplication`) and runs gtest.
- [`tests/src/test_utils.{h,cpp}`](../tests/src/test_utils.h) — common helpers, asynchronous waits, signal spies.
- `mock_*` headers — Google Mock-based mocks for `NetworkAccessManager`, `PlaylistItem`, `SettingsProvider`, `CollectionBackend`.
- `*_env.h` — gtest environment classes that init resources, metatypes, logging.
- `tests/data/` — sample audio files, cue sheets, playlists, test database fixtures.

## When you change something — what to run

| Change                            | At minimum, build & run...                                |
| --------------------------------- | --------------------------------------------------------- |
| A utility in `src/utilities/`     | `utilities_test`                                          |
| Anything DB-related               | `sqlite_test`, `collectionbackend_test`                   |
| Anything song-tag related         | `tagreader_test`                                          |
| Playlist behaviour                | `playlist_test`, `songplaylistitem_test`                  |
| Organize/Copy format strings      | `organizeformat_test`                                     |
| Collection model                  | `collectionmodel_test`                                    |
| Anything else / unsure            | `cmake --build build --target run_strawberry_tests`       |

## Packaging

- **Linux:** [`cmake/Rpm.cmake`](../cmake/Rpm.cmake), [`cmake/Deb.cmake`](../cmake/Deb.cmake), and the [`debian/`](../debian/) directory. Repology badge in the README tracks distros.
- **macOS:** [`cmake/Dmg.cmake`](../cmake/Dmg.cmake).
- **Windows:** built externally via [strawberry-msvc-build-tools](https://github.com/strawberrymusicplayer/strawberry-msvc-build-tools).