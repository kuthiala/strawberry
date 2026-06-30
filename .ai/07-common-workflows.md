# 7. Common Workflows

Step-by-step recipes for changes you'll commonly need to make in this codebase. Each recipe lists every file you'll likely touch and the rough order of operations.

> Always confirm by **looking at how a sibling does it** before inventing. Patterns are remarkably consistent within Strawberry.

---

## Add a New Source File

When you create a new `.cpp`/`.h`:

1. Create the files under the appropriate `src/<module>/` directory using the GPL header from a neighbour (see [`04-coding-conventions.md`](./04-coding-conventions.md)).
2. Open the top-level [`CMakeLists.txt`](../CMakeLists.txt) and **add the `.cpp` to `SOURCES`** (in the right module's block, alphabetical order within the block).
3. If your header contains `Q_OBJECT`, **add the `.h` to `HEADERS`** so `automoc` picks it up.
4. If the file has a `.ui` form, add it to the `UI` set.
5. Configure & build:

   ```bash
   cmake -S . -B build && cmake --build build
   ```

6. Run the relevant tests (see [`03-build-and-test.md`](./03-build-and-test.md)).

> ⚠️ If you forget step 2/3, the linker error will be `undefined reference to vtable for YourClass` or "no MOC for class derived from QObject".

---

## Add a New User Setting

Goal: a new toggle/option in the Preferences dialog, persisted to `QSettings`.

1. **Decide where it goes.** Pick the existing settings page (e.g. `settings/behavioursettingspage.{h,cpp,ui}`) it logically belongs to, or — for a major feature — create a new page (steps marked **★** below cover this).
2. **Add a constant for the key** in [`src/constants/`](../src/constants/). Open or create `constants/<name>settings.h` and add:

   ```cpp
   namespace BehaviourSettings {
     inline constexpr char kSettingsGroup[] = "Behaviour";
     inline constexpr char kMyNewToggle[] = "my_new_toggle";
   }
   ```

3. **Edit the `.ui` form** in Qt Designer (or by hand) to add the widget.
4. **Wire load/save in the settings page** (`Load()` and `Save()` methods, or whatever the page calls them). Use the constants:

   ```cpp
   Settings s;
   s.beginGroup(BehaviourSettings::kSettingsGroup);
   ui_->my_new_toggle->setChecked(s.value(BehaviourSettings::kMyNewToggle, /*default=*/true).toBool());
   s.endGroup();
   ```

5. **Consume the setting** wherever it applies — again via `Settings` + the constants header. Connect to `Application` or another controller to react to changes if needed.
6. **★ New page only:**
   - Subclass `SettingsPage` ([`settings/settingspage.h`](../src/settings/settingspage.h)).
   - Register it in [`settings/settingsdialog.cpp`](../src/settings/settingsdialog.cpp) (`AddPage(...)`).
   - Add icon entries in [`data/icons.qrc`](../data/icons.qrc) if needed.
   - Add the source files to `CMakeLists.txt`.

---

## Add a New Cover Art Provider

Pattern model: [`covermanager/lastfmcoverprovider.{h,cpp}`](../src/covermanager/lastfmcoverprovider.h).

1. Subclass either [`CoverProvider`](../src/covermanager/coverprovider.h) (for arbitrary providers) or [`JsonCoverProvider`](../src/covermanager/jsoncoverprovider.h) (for JSON-API ones).
2. Override `StartSearch(const QString &artist, const QString &album, const QString &title, int id)` → kick off the HTTP request.
3. Override `CancelSearch(int id)`.
4. Emit `SearchFinished(int id, const CoverProviderSearchResults &results)` (or `SearchResults` followed by `SearchFinished` for streaming results).
5. Register the provider in [`covermanager/coverproviders.cpp`](../src/covermanager/coverproviders.cpp) (look for the constructor that adds existing providers).
6. Add the source files to `CMakeLists.txt`.
7. If the provider needs configuration (API key, auth):
   - Add settings constants in [`constants/coverssettings.h`](../src/constants/coverssettings.h).
   - Extend [`settings/coverssettingspage.{cpp,ui}`](../src/settings/coverssettingspage.cpp).
8. **Test it** — the existing live-test file [`tests/src/lyricsproviders_live_test.cpp`](../tests/src/lyricsproviders_live_test.cpp) is the closest template for live testing.

---

## Add a New Lyrics Provider

Pattern model: [`lyrics/lrcliblyricsprovider.{h,cpp}`](../src/lyrics/lrcliblyricsprovider.h) for JSON, or [`lyrics/azlyricscomlyricsprovider.{h,cpp}`](../src/lyrics/azlyricscomlyricsprovider.h) for HTML scraping.

1. Subclass:
   - [`JsonLyricsProvider`](../src/lyrics/jsonlyricsprovider.h) for JSON APIs,
   - [`HtmlLyricsProvider`](../src/lyrics/htmllyricsprovider.h) for HTML scrapers, or
   - [`LyricsProvider`](../src/lyrics/lyricsprovider.h) directly for anything else.
2. Override `StartSearch()` and `CancelSearch()`.
3. Emit `SearchFinished(int id, const LyricsSearchResults &results)`.
4. Register in [`lyrics/lyricsproviders.cpp`](../src/lyrics/lyricsproviders.cpp).
5. Add the source files to `CMakeLists.txt`.
6. If config is needed: extend [`constants/lyricssettings.h`](../src/constants/lyricssettings.h) and [`settings/lyricssettingspage`](../src/settings/lyricssettingspage.cpp).
7. Add a live test case to [`tests/src/lyricsproviders_live_test.cpp`](../tests/src/lyricsproviders_live_test.cpp).

---

## Add a New Radio Service

Pattern model: [`radios/somafmservice.{h,cpp}`](../src/radios/somafmservice.h).

1. Subclass [`RadioService`](../src/radios/radioservice.h).
2. Implement `name()`, `url_scheme()`, and the fetch logic (typically populating a list of `RadioChannel`).
3. Register it in [`radios/radioservices.cpp`](../src/radios/radioservices.cpp).
4. If the URLs need a custom `UrlHandler`, also create that and register it in `Application` via `url_handlers()`.
5. Add the source files to `CMakeLists.txt`.
6. Optional: settings constants in `constants/<name>settings.h`, settings UI in `settings/radiosettingspage.*`.

---

## Add a New Streaming Service

(Much bigger task — only outlined here. Use Subsonic as the canonical example because its API is open.)

1. Create `src/<svc>/` with:
   - `<svc>service.{h,cpp}` — subclasses [`StreamingService`](../src/streaming/streamingservice.h).
   - `<svc>baserequest.{h,cpp}` — HTTP shared boilerplate.
   - `<svc>request.{h,cpp}` — catalogue + search.
   - `<svc>favoriterequest.{h,cpp}` — favourites/playlists.
   - `<svc>streamurlrequest.{h,cpp}` — resolve track ID → stream URL.
   - `<svc>urlhandler.{h,cpp}` — `<svc>://track-id` URL scheme.
2. Add **service mirror tables** to `data/schema/`:
   - In a new `schema-NN.sql` migration plus the baseline `schema.sql`, add
     `<svc>_artists_songs`, `<svc>_albums_songs`, `<svc>_songs` tables with the same column shape as `songs` (see [`06-data-model.md`](./06-data-model.md)).
3. Add `Source::<Svc>` to the `Song::Source` enum in [`core/song.h`](../src/core/song.h). Update `Song::kSourceCount` and any `switch` statements in `song.cpp`.
4. Register the service in [`streaming/streamingservices.cpp`](../src/streaming/streamingservices.cpp).
5. Add settings constants (`constants/<svc>settings.h`) and a settings page (`settings/<svc>settingspage.{h,cpp,ui}`).
6. Add cover & lyrics providers if applicable, in `covermanager/` and `lyrics/`.
7. **All new sources to `CMakeLists.txt`**.

---

## Add a New Playlist Format

1. Subclass [`ParserBase`](../src/playlistparsers/parserbase.h) (or [`XMLParser`](../src/playlistparsers/xmlparser.h) for XML).
2. Implement `name()`, `file_extensions()`, `mime_type()`, `Load()`, and `Save()`.
3. Register the parser in [`playlistparsers/playlistparser.cpp`](../src/playlistparsers/playlistparser.cpp) (look at the constructor — every parser is added there).
4. Add source files to `CMakeLists.txt`.
5. Add a unit test fixture under [`tests/data/playlists/`](../tests/data/) and a test in [`tests/src/playlist_test.cpp`](../tests/src/playlist_test.cpp).

---

## Add a New Scrobbler

Pattern model: [`scrobbler/listenbrainzscrobbler.{h,cpp}`](../src/scrobbler/listenbrainzscrobbler.h).

1. Subclass [`ScrobblerService`](../src/scrobbler/scrobblerservice.h).
2. Implement auth + the network calls.
3. Use [`ScrobblerCache`](../src/scrobbler/scrobblercache.h) for offline queuing.
4. Register in [`scrobbler/audioscrobbler.{h,cpp}`](../src/scrobbler/audioscrobbler.h).
5. Add settings constants in [`constants/scrobblersettings.h`](../src/constants/scrobblersettings.h) and extend [`settings/scrobblersettingspage.{cpp,ui}`](../src/settings/scrobblersettingspage.cpp).
6. Add source files to `CMakeLists.txt`.

---

## Add a New Audio Analyzer (visualisation)

Pattern model: [`analyzer/blockanalyzer.{h,cpp}`](../src/analyzer/blockanalyzer.h).

1. Subclass [`AnalyzerBase`](../src/analyzer/analyzerbase.h) — typically `Analyzer::Base` (templated on FFT data type).
2. Implement `analyze(QPainter &painter, const Scope &scope, bool new_frame)`.
3. Provide a static `name()` returning a translatable label.
4. Add the new class to the dropdown in [`analyzer/analyzercontainer.cpp`](../src/analyzer/analyzercontainer.cpp) (look for the existing `AddAnalyzer<…>()` calls).
5. Add source files to `CMakeLists.txt`.

---

## Add a New Database Column

See the checklist in [`06-data-model.md`](./06-data-model.md) ("When you add a new field to `songs`"). Briefly:

1. Bump schema version: new `data/schema/schema-NN.sql` migration + update `schema.sql` baseline + update `schema_version` insert + add to [`data/data.qrc`](../data/data.qrc).
2. Add column to **all** parallel `*_songs` tables.
3. Update `Song::kColumns`, `kColumnSpec`, `kBindSpec`, `kUpdateSpec` in `song.cpp`.
4. Add getter/setter/member to `Song` (via `SongData` `QSharedData` subclass).
5. Update `Song::InitFromQuery()` and `Song::BindToQuery()`.
6. If searchable, add to the right `k*SearchColumns` list.
7. Update `tagreader/tagreadertaglib.cpp` to read/write the tag if it lives in files.
8. Update `dialogs/edittagdialog.{cpp,ui}` if user-editable.
9. Update tests (`collectionbackend_test.cpp`, `tagreader_test.cpp`).

---

## Add a New Translatable String

Just call `tr()` or `QObject::tr()`:

```cpp
QMessageBox::information(this, tr("Strawberry"), tr("Done!"));
```

Strings flow into the `.ts` files automatically (via `lupdate` in the translation tooling). **You normally do not commit new `.qm`/`.ts` files** — translations are managed via Crowdin (see [`crowdin.yml`](../crowdin.yml)).

---

## Add a New Test

1. Create `tests/src/<feature>_test.cpp`. Copy structure from [`tests/src/utilities_test.cpp`](../tests/src/utilities_test.cpp) for non-GUI, or [`tests/src/playlist_test.cpp`](../tests/src/playlist_test.cpp) for GUI.
2. Standard prologue:

   ```cpp
   #include "gtest_include.h"
   #include "test_utils.h"
   #include "metatypes_env.h"

   TEST(MyThing, DoesAThing) {
     EXPECT_EQ(1, 1);
   }
   ```

3. Register the test in [`tests/CMakeLists.txt`](../tests/CMakeLists.txt) with:

   ```cmake
   add_test_file(src/myfeature_test.cpp false)   # true if it needs GUI
   ```

4. Run with `cmake --build build --target run_strawberry_tests`.

---

## Add an Icon or Other Resource

1. Drop the file into `data/icons/<size>/...` or another appropriate location under [`data/`](../data/).
2. Register it in [`data/icons.qrc`](../data/icons.qrc) or [`data/data.qrc`](../data/data.qrc):

   ```xml
   <file alias="icons/22x22/foo.png">icons/22x22/foo.png</file>
   ```

3. Load via `IconLoader::Load(u"foo"_s)` ([`core/iconloader.h`](../src/core/iconloader.h)).

---

## Add a Command-line Option

1. Edit [`src/core/commandlineoptions.{h,cpp}`](../src/core/commandlineoptions.h):
   - Add the option to the enum and `getopt_long` struct array.
   - Add a member to hold the parsed value.
   - Parse it in `CommandlineOptions::Parse()`.
   - Add to `Serialize()` / deserialize so it can be relayed to the running instance via `KDSingleApplication`.
2. Consume it where appropriate (often in `main.cpp` or `MainWindow::CommandlineOptionsReceived`).

---

## Where to Look for Existing Patterns

| You want to…                                  | Open this as a template                              |
| --------------------------------------------- | ---------------------------------------------------- |
| Subclass a JSON-API requester                 | `core/jsonbaserequest.{h,cpp}` consumers (any service) |
| Do OAuth                                      | `core/oauthenticator.{h,cpp}` + `scrobbler/listenbrainzscrobbler.cpp` |
| Show a custom non-modal dialog                | `dialogs/console.{h,cpp,ui}`                         |
| Implement a `QAbstractItemModel`              | `collection/collectionmodel.{h,cpp}`                 |
| Use a worker `QThread`                        | `collection/collectionwatcher.{h,cpp}` (look at `Application::MoveToNewThread()`) |
| Persist a struct as JSON                      | `radios/radiobackend.{h,cpp}`                        |
| Add a column to a tree view                   | `playlist/playlist.cpp` (`Playlist::Column_*` enums) |
| Wire a signal across threads                  | `Q_OBJECT` + `Q_SIGNALS` + queued connections — see `collection/collectionwatcher.{h,cpp}` |
| Add a global keyboard shortcut                | `globalshortcuts/globalshortcutsbackend*.{h,cpp}`   |
| Read/write tags                               | `tagreader/tagreaderclient.{h,cpp}` callers (everywhere) |