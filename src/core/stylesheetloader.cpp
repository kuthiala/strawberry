/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2026, Jonas Kvinge <jonas@jkvinge.net>
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

#include <cmath>
#include <algorithm>
#include <memory>

#include <QtGlobal>
#include <QObject>
#include <QWidget>
#include <QTimer>
#include <QIODevice>
#include <QTextStream>
#include <QFile>
#include <QString>
#include <QPalette>
#include <QColor>
#include <QEvent>
#include <QList>

#include "includes/shared_ptr.h"
#include "logging.h"
#include "utilities/colorutils.h"
#include "stylesheetloader.h"

using namespace Qt::Literals::StringLiterals;

namespace {
// Color difference between alternating rows and normal rows, for light and dark palettes.
#ifdef Q_OS_MACOS
// A smaller difference is hard to see on macOS in light mode.
constexpr float kAlternateBaseColorDifferenceLight = 0.07F;
constexpr float kAlternateBaseColorDifferenceDark = 0.017F;
// The alternate base on macOS is the same colour as the base in light mode, and an opaque grey in dark mode.
// Use the text colour instead, which has the most contrast to the base, so the alternating rows need the least opacity.
constexpr bool kUseTextColorForAlternateBase = true;
#else
constexpr float kAlternateBaseColorDifferenceLight = 0.025F;
constexpr float kAlternateBaseColorDifferenceDark = 0.025F;
constexpr bool kUseTextColorForAlternateBase = false;
#endif
}  // namespace

using std::make_shared;

StyleSheetLoader::StyleSheetLoader(QObject *parent) : QObject(parent), updating_(false) {}

void StyleSheetLoader::SetStyleSheet(QWidget *widget, const QString &filename) {

  // Load the file
  QFile file(filename);
  if (!file.open(QIODevice::ReadOnly)) {
    qLog(Error) << "Could not open stylesheet file" << filename << "for reading:" << file.errorString();
    return;
  }
  QTextStream stream(&file);
  QString stylesheet;
  Q_FOREVER {
    QString line = stream.readLine();
    stylesheet.append(line);
    if (stream.atEnd()) break;
  }
  file.close();

  SharedPtr<StyleSheetData> styledata = make_shared<StyleSheetData>();
  styledata->filename_ = filename;
  styledata->stylesheet_template_ = stylesheet;
  styledata_.insert(widget, styledata);

  widget->installEventFilter(this);
  UpdateStyleSheet(widget, styledata);

}

void StyleSheetLoader::UpdateStyleSheet(QWidget *widget, SharedPtr<StyleSheetData> styledata) {

  QString stylesheet = styledata->stylesheet_template_;

  // Replace %palette-role with actual colours
  const QPalette palette = widget->palette();

  // The alternate base is drawn on top of the base, so the visible difference is the alpha multiplied by the color difference between the colours.
  // Calculate the alpha giving the same visible difference for all palettes, but never more opaque than the palette colour.
  const QColor color_base = palette.color(QPalette::Base);
  const float target_color_difference = Utilities::IsPaletteDark(palette, QPalette::Base, QPalette::Text) ? kAlternateBaseColorDifferenceDark : kAlternateBaseColorDifferenceLight;
  QColor color_altbase = palette.color(QPalette::AlternateBase);
  float color_difference = Utilities::ColorDifference(color_altbase, color_base);
  // Some palettes use an alternate base too close to the base to be visible, use the text colour instead.
  if (kUseTextColorForAlternateBase || color_difference < target_color_difference) {
    const QColor color_text = palette.color(QPalette::Text);
    const float text_color_difference = Utilities::ColorDifference(color_text, color_base);
    if (text_color_difference > color_difference) {
      color_altbase = color_text;
      color_difference = text_color_difference;
    }
  }
  // If the colors are the same, the alternating rows can't be seen at any opacity, so don't draw them.
  const float altbase_alpha = color_difference > 0.0F ? std::min(color_altbase.alphaF(), target_color_difference / color_difference) : 0.0F;
  const int altbase_alpha_percent = static_cast<int>(std::lround(altbase_alpha * 100.0F));
  stylesheet.replace("%palette-alternate-base"_L1, QStringLiteral("rgba(%1,%2,%3,%4%)").arg(color_altbase.red()).arg(color_altbase.green()).arg(color_altbase.blue()).arg(altbase_alpha_percent));

  // Longer names first, a name which is the start of another name would otherwise replace the start of it, "%palette-Highlight" turning "%palette-HighlightedText" into "#314f78edText".
  static const QList<std::pair<QString, QPalette::ColorRole>> color_roles = []() {
    QList<std::pair<QString, QPalette::ColorRole>> roles = {
      { u"Window"_s, QPalette::Window },
      { u"Background"_s, QPalette::Window },
      { u"WindowText"_s, QPalette::WindowText },
      { u"Base"_s, QPalette::Base },
      { u"AlternateBase"_s, QPalette::AlternateBase },
      { u"ToolTipBase"_s, QPalette::ToolTipBase },
      { u"ToolTipText"_s, QPalette::ToolTipText },
      { u"Text"_s, QPalette::Text },
      { u"Button"_s, QPalette::Button },
      { u"ButtonText"_s, QPalette::ButtonText },
      { u"BrightText"_s, QPalette::BrightText },
      { u"Light"_s, QPalette::Light },
      { u"Midlight"_s, QPalette::Midlight },
      { u"Dark"_s, QPalette::Dark },
      { u"Mid"_s, QPalette::Mid },
      { u"Shadow"_s, QPalette::Shadow },
      { u"Highlight"_s, QPalette::Highlight },
      { u"HighlightedText"_s, QPalette::HighlightedText },
      { u"Link"_s, QPalette::Link },
      { u"LinkVisited"_s, QPalette::LinkVisited },
    };
    std::stable_sort(roles.begin(), roles.end(), [](const std::pair<QString, QPalette::ColorRole> &a, const std::pair<QString, QPalette::ColorRole> &b) { return a.first.length() > b.first.length(); });
    return roles;
  }();
  for (const std::pair<QString, QPalette::ColorRole> &color_role : color_roles) {
    ReplaceColor(&stylesheet, color_role.first, palette, color_role.second);
  }

#ifdef Q_OS_MACOS
  stylesheet.replace(QLatin1String("macos"), QLatin1String("*"));
#endif

  // With an application stylesheet (the Claude theme), setting this stylesheet repolishes the widgets, which sets their palettes and sends PaletteChange again.
  // Applying the same stylesheet again would repeat that forever, so only apply it when the colours changed.
  if (widget->styleSheet() == stylesheet) return;

  updating_ = true;
  widget->setStyleSheet(stylesheet);
  updating_ = false;

}

void StyleSheetLoader::ReplaceColor(QString *css, const QString &name, const QPalette &palette, const QPalette::ColorRole role) {

  css->replace(u"%palette-"_s + name + u"-lighter"_s, palette.color(role).lighter().name(), Qt::CaseInsensitive);
  css->replace(u"%palette-"_s + name + u"-darker"_s, palette.color(role).darker().name(), Qt::CaseInsensitive);
  css->replace(u"%palette-"_s + name, palette.color(role).name(), Qt::CaseInsensitive);

}

bool StyleSheetLoader::eventFilter(QObject *obj, QEvent *event) {

  if (event->type() == QEvent::PaletteChange && !updating_) {
    QWidget *widget = qobject_cast<QWidget*>(obj);
    if (widget && styledata_.contains(widget)) {
      UpdateStyleSheet(widget, styledata_[widget]);
    }
  }

  return false;

}
