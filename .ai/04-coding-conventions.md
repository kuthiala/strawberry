# 4. Coding Conventions

> ⚠️ **`.clang-format` has `DisableFormat: true`** in this repo (see [`.clang-format`](../.clang-format)). It documents the *intended* style but **does not auto-reformat**. Do **not** run `clang-format` on existing files — you will produce huge diffs that don't match the surrounding code. Match the style of the file you are editing.

## Language Standard

- **C++17** is the project standard (`CMAKE_CXX_STANDARD 17`, required).
- **C11** for the small amount of C code (C99 on MSVC).
- Objective-C++ (`.mm`) for macOS-only code (Cocoa, Sparkle, native NSStatusItem, etc.).

## Hard Rules (enforced at compile time)

The following Qt defines are set globally in [`CMakeLists.txt`](../CMakeLists.txt):

| Define                                  | What it forbids                                                    |
| --------------------------------------- | ------------------------------------------------------------------ |
| `QT_NO_FOREACH`                         | The `foreach`/`Q_FOREACH` macro — use C++ range-for or `std::as_const` |
| `QT_NO_KEYWORDS`                        | The `signals`, `slots`, `emit` keywords — use `Q_SIGNALS`/`Q_SLOTS`/`Q_EMIT` |
| `QT_NO_CAST_FROM_ASCII`                 | Implicit `const char*` → `QString` — use `u"..."_s` or `QStringLiteral` |
| `QT_NO_CAST_TO_ASCII`                   | Implicit `QString` → `const char*`                                 |
| `QT_NO_CAST_FROM_BYTEARRAY`             | Implicit `QByteArray` conversions                                  |
| `QT_NO_URL_CAST_FROM_STRING`            | Implicit `QString` → `QUrl`                                        |
| `QT_NO_NARROWING_CONVERSIONS_IN_CONNECT`| Narrowing in signal/slot connects                                  |
| `QT_STRICT_ITERATORS`                   | Stronger iterator types                                            |
| `QT_USE_QSTRINGBUILDER`                 | `QString::operator+` returns a `QStringBuilder` proxy              |
| `BOOST_BIND_NO_PLACEHOLDERS`            | Drops `_1, _2, …` from the global namespace                        |

If your code doesn't compile, **read the warning before disabling these** — the convention exists for a reason.

## Naming

| Kind                | Style                              | Example                                         |
| ------------------- | ---------------------------------- | ----------------------------------------------- |
| Class / struct      | `PascalCase`                       | `CollectionBackend`, `SongLoader`               |
| Functions / methods | `PascalCase` for public, `lower_snake_case()` accessor-style getters | `LoadCoverData()`, `playlist_manager()` |
| Slots / signals     | `PascalCase` (e.g. `SongChanged`)  | `Q_SLOT void SongChanged(Song);`                |
| Member variable     | `name_` (trailing underscore)      | `int playcount_;` `SharedPtr<Database> database_;` |
| Static const / k-prefixed constants | `kCamelCase`                        | `static const QStringList kColumns;` `kSettingsGroup` |
| Enums / enum values | `enum class Foo { Bar, Baz }`      | `Song::Source::LocalFile`                       |
| Files               | all lowercase, no separators       | `collectionbackend.cpp`, `albumcoverfetcher.h`  |
| Namespaces          | Generally avoided; `Utilities::`, `logging::`, `mac::`, `mpris::` exist | |

The unusual mix of `PascalCase` method names and `lower_snake_case` accessors comes from Clementine; **match the file's existing style** rather than picking your favourite.

## File Conventions

Every `.h` / `.cpp` file starts with the GPL header. When you create a new file, copy it from a neighbouring file and update the copyright lines. Example header (abridged):

```cpp
/*
 * Strawberry Music Player
 * This file was part of Clementine.            // only if relevant
 * Copyright YYYY, Original Author <email>
 * Copyright YYYY-YYYY, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * …
 */
```

Add your own copyright line for substantial new work — preserve all existing lines.

### Header guards

Old-school `#ifndef NAME_H / #define NAME_H / #endif` (no `#pragma once`). Match the surrounding pattern:

```cpp
#ifndef COLLECTIONBACKEND_H
#define COLLECTIONBACKEND_H
…
#endif  // COLLECTIONBACKEND_H
```

### Includes

The conventional order in this codebase is:

1. `#include "config.h"` (always first if needed)
2. Platform / C headers (`<unistd.h>`, `<windows.h>`)
3. C++ standard library (`<memory>`, `<optional>`)
4. Qt headers (`<QObject>`, `<QString>`, …) — alphabetised within the group
5. Project headers (`"includes/scoped_ptr.h"`, `"core/song.h"`, …)
6. The header for the current `.cpp` (e.g. in `foo.cpp`, include `"foo.h"`)

`.clang-format` keeps include blocks preserved (`IncludeBlocks: Preserve`); **don't re-sort** other people's includes.

### Forward declarations

Prefer forward declarations in headers over `#include` to keep build times sane. The header pattern you'll see throughout the codebase:

```cpp
class Foo;
class Bar;

class MyClass : public QObject {
  Q_OBJECT
  …
};
```

## Indentation / Formatting

- **2 spaces** per indent level, no tabs.
- `ColumnLimit: 0` — long lines are allowed; readability wins. Break only when it makes sense.
- Opening braces on the **same line** for functions / classes / control flow (Stroustrup-style for `else`: it goes on a new line after `}`).
- Space before `(` after `if`, `for`, `while`, `switch`; **no** space after function names.
- Pointers / references: **right-aligned** to the variable (`QString *str`, `const Song &song`). This is Qt convention.

Examples:

```cpp
class CollectionBackend : public QObject {
  Q_OBJECT

 public:                     // access specifiers indented by one
  explicit CollectionBackend(QObject *parent = nullptr);

  void AddDirectory(const QString &path);

 Q_SIGNALS:
  void DirectoryAdded(int id);

 private:
  SharedPtr<Database> database_;
};
```

## Qt Patterns You Will See Everywhere

### String literals

Because of `QT_NO_CAST_FROM_ASCII`, every literal that becomes a `QString` must be wrapped:

```cpp
using namespace Qt::Literals::StringLiterals;   // at the top of most .cpp files

QString s = u"hello"_s;                         // → QString
QStringView v = u"hello";                       // → QStringView
QLatin1String l = "hello"_L1;                   // → QLatin1String

QStringLiteral("Strawberry")                    // legacy alternative
```

For `QByteArray`, `"raw"_ba`. For `QStringList`s, list-initialise with these literals.

### Logging

```cpp
#include "core/logging.h"

qLog(Debug) << "Loaded" << songs.count() << "songs";
qLog(Info)  << "Strawberry" << STRAWBERRY_VERSION_DISPLAY;
qLog(Warning) << "Could not parse" << url;
qLog(Error) << "Database is corrupt";
```

`qLog()` is Strawberry's wrapper around `qCDebug`/`qDebug` with per-category levels (configurable via the `--log-levels` CLI option, see [`core/commandlineoptions.cpp`](../src/core/commandlineoptions.cpp)).

### Smart pointers

```cpp
#include "includes/scoped_ptr.h"
#include "includes/shared_ptr.h"

ScopedPtr<Foo>   foo_(new Foo);
SharedPtr<Bar>   bar_ = make_shared<Bar>(arg);
```

Avoid raw `new`/`delete` for objects with clear ownership. `QObject` children with a `parent` argument are okay — Qt cleans them up.

### Signals & slots

Use **`Q_SLOTS` / `Q_SIGNALS` / `Q_EMIT`** (uppercase):

```cpp
class Player : public QObject {
  Q_OBJECT
 Q_SIGNALS:
  void TrackChanged(const Song &song);
 public Q_SLOTS:
  void Next();
};

…
Q_EMIT TrackChanged(song);
```

Connect with the function-pointer syntax wherever possible (it's checked at compile time):

```cpp
QObject::connect(player, &Player::TrackChanged, this, &MyClass::OnTrackChanged);
```

Avoid the `SIGNAL()` / `SLOT()` macros; they're string-based and slower.

### Iteration without `foreach`

```cpp
// Range-for: BUT make sure container isn't copied if it's a Qt implicit-shared type.
for (const Song &song : std::as_const(songs)) {
  qLog(Debug) << song.title();
}

// Index-based:
const int count = songs.count();
for (int i = 0; i < count; ++i) {
  …
}
```

### Settings

```cpp
#include "core/settings.h"
#include "constants/behavioursettings.h"

Settings s;
s.beginGroup(BehaviourSettings::kSettingsGroup);
const QString lang = s.value(BehaviourSettings::kLanguage).toString();
s.endGroup();
```

Always go through `Settings` + a constants header — never write group/key strings inline.

### Database access

```cpp
#include "core/database.h"
#include "core/sqlquery.h"
#include "core/scopedtransaction.h"

QSqlDatabase db = database_->Connect();
ScopedTransaction t(&db);

SqlQuery q(db);
q.prepare(u"SELECT * FROM songs WHERE id = :id"_s);
q.BindValue(u":id"_s, id);
if (!q.Exec()) {
  qLog(Error) << q.lastError();
  return {};
}
while (q.next()) { … }

t.Commit();
```

Use `SqlQuery` and `ScopedTransaction` (not raw `QSqlQuery`) — they integrate with logging and the project's binding conventions.

### Networking

```cpp
QNetworkRequest req(url);
QNetworkReply *reply = network_->Get(req);
QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
  reply->deleteLater();
  …
});
```

Always remember to `deleteLater()` replies. For JSON, see [`core/jsonbaserequest.h`](../src/core/jsonbaserequest.h) (a base class for JSON-API requesters). For OAuth, see [`core/oauthenticator.h`](../src/core/oauthenticator.h).

## Commit Messages

From [`CONTRIBUTING.md`](../CONTRIBUTING.md):

```
ClassName: Short description (no trailing period)

Optional longer body, explaining what changed and why.

Fixes #1234
```

- First line: `ClassName: short imperative summary`, ≤ 72 chars, no trailing period.
- Blank line.
- Optional body in normal prose.
- `Fixes #N` / `Closes #N` to reference issues.
- For changes that span multiple classes, omit the prefix and write a general summary.

## Translations

User-visible strings must be translatable:

```cpp
const QString message = tr("Could not save %1").arg(filename);
```

Use `tr()` (member function on any `QObject`) or `QObject::tr()` for static contexts. Avoid stitching translated strings with `+`; use `%1`/`%2` placeholders with `.arg()`.

If you add a new user-facing string, regenerate translations only if asked — they normally flow through Crowdin (see [`crowdin.yml`](../crowdin.yml)).

## When in Doubt

Open the closest existing file and **mimic its conventions exactly**. The codebase is large and consistent within each module — copy patterns rather than inventing new ones.