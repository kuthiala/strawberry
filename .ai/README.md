# AI Reference Documentation for Strawberry Music Player

This directory contains comprehensive documentation designed for AI assistants (and human contributors) working in the Strawberry codebase. Use these documents to quickly understand the project structure, conventions, and where to look for specific functionality.

## How to Use This Directory

When you are tasked with making changes in this repository, start by reading the documents most relevant to the task:

| If your task involves...                                  | Start with...                                              |
| --------------------------------------------------------- | ---------------------------------------------------------- |
| Understanding the project at a high level                 | [`01-project-overview.md`](./01-project-overview.md)       |
| Understanding how the application is wired together       | [`02-architecture.md`](./02-architecture.md)               |
| Building, running, or testing the project                 | [`03-build-and-test.md`](./03-build-and-test.md)           |
| Following coding style and Qt conventions                 | [`04-coding-conventions.md`](./04-coding-conventions.md)   |
| Finding a specific module / area of the codebase          | [`05-module-guide.md`](./05-module-guide.md)               |
| Database schema, songs, and migrations                    | [`06-data-model.md`](./06-data-model.md)                   |
| Performing common development tasks (adding providers...) | [`07-common-workflows.md`](./07-common-workflows.md)       |
| Quick keyword/file lookup                                 | [`08-quick-reference.md`](./08-quick-reference.md)         |
| Glossary / domain terms                                   | [`09-glossary.md`](./09-glossary.md)                       |
| **Anything iPod-sync related** (libgpod, transcoder, cover art, "No Music.") | [`10-ipod-sync.md`](./10-ipod-sync.md) |
| **macOS-specific build / install / debug workflow**       | [`11-macos-dev-loop.md`](./11-macos-dev-loop.md)           |

## TL;DR for AI Agents

- **Project:** Strawberry is a **Qt 6 / C++17** desktop **music player and collection manager**, forked from Clementine in 2018.
- **Build system:** **CMake** (>= 3.13). Top-level [`CMakeLists.txt`](../CMakeLists.txt) lists every source file explicitly — when you add a `.cpp`/`.h`, you usually need to add it there too.
- **Entry point:** [`src/main.cpp`](../src/main.cpp) → constructs an [`Application`](../src/core/application.h) → constructs [`MainWindow`](../src/core/mainwindow.h).
- **Code style:** Heavy use of Qt; **no `foreach`, no `signals`/`slots` keywords** (use `Q_SIGNALS`/`Q_SLOTS`/`Q_EMIT`), **no implicit `QString` ↔ `const char*` casts** (use `u"..."_s` / `"..."_L1`).
- **Tests:** GoogleTest, in [`tests/`](../tests/), opt-in target `run_strawberry_tests`.
- **Domain core:** The [`Song`](../src/core/song.h) class is the central data type; the SQLite **schema is in [`data/schema/schema.sql`](../data/schema/schema.sql)** (current version: **23**); migrations live next to it as `schema-NN.sql`.
- **Modules are organised by feature**, each in its own subdirectory under `src/` (e.g. `collection/`, `playlist/`, `engine/`, `covermanager/`, `lyrics/`).
- **🛑 macOS-specific landmine:** after `./install-macos.sh clean`, after a first build, or after any deps/CMakeLists link-list change, you **must** run `./install-macos.sh bundle` *before* `install -y`. Skipping it produces a `build/strawberry.app` that crashes within 100 ms on launch with duplicate-QtCore errors — and the crash has nothing to do with whatever C++ change you just made. Full triage at [`11-macos-dev-loop.md §11.15`](./11-macos-dev-loop.md#1115-post-mortem-the-duplicate-qtcore-crash-read-this-first-if-the-app-wont-launch). 2-second verification: `otool -L /Applications/strawberry.app/Contents/MacOS/strawberry | grep /opt/strawberry_macos` must be empty.

## Important: Keep This Directory Up To Date

If you make significant architectural changes (new module, new provider type, change to the schema, change to build system), please update the relevant document(s) in this directory so future AI sessions stay accurate.