/*
 * Strawberry Music Player
 * This file is part of Strawberry.
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

#include <QtGlobal>
#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>
#include <QStyle>
#include <QFile>
#include <QPalette>
#include <QColor>
#include <QString>

#include "core/logging.h"
#include "core/settings.h"
#include "constants/appearancesettings.h"
#include "claudetheme.h"

using namespace Qt::Literals::StringLiterals;

namespace {

QString CssColor(const QColor &color) {

  if (color.alpha() == 255) return color.name(QColor::HexRgb);
  return u"rgba(%1, %2, %3, %4)"_s.arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());

}

}  // namespace

ClaudeTheme::ClaudeTheme(QObject *parent) : QObject(parent), active_(false), colors_(LightColors()) {

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
    if (active_) Update();
  });
#endif

}

ClaudeTheme *ClaudeTheme::instance() {

  static ClaudeTheme *theme = new ClaudeTheme(qApp);
  return theme;

}

ClaudeTheme::Colors ClaudeTheme::LightColors() {

  Colors c;
  c.dark = false;
  c.window = QColor(0xFAF9F5);
  c.sidebar = QColor(0xF0EEE6);
  c.surface = QColor(0xFFFFFF);
  c.surface_alt = QColor(0xF8F6F1);
  c.raised = QColor(0xFFFFFF);
  c.hover = QColor(0xEFECE3);
  c.selected = QColor(0xF4E1D7);
  c.border = QColor(0xE6E2D8);
  c.border_strong = QColor(0xD3CEC1);
  c.text = QColor(0x1F1E1D);
  c.text_secondary = QColor(0x6F6B63);
  c.text_disabled = QColor(0xADA99F);
  c.accent = QColor(0xC96442);
  c.accent_hover = QColor(0xB5573A);
  c.accent_text = QColor(0xFFFFFF);
  c.tooltip = QColor(0x2B2A27);
  c.tooltip_text = QColor(0xFAF9F5);
  return c;

}

ClaudeTheme::Colors ClaudeTheme::DarkColors() {

  Colors c;
  c.dark = true;
  c.window = QColor(0x262624);
  c.sidebar = QColor(0x1F1E1D);
  c.surface = QColor(0x2C2C2A);
  c.surface_alt = QColor(0x302F2D);
  c.raised = QColor(0x353432);
  c.hover = QColor(0x3A3936);
  c.selected = QColor(0x4D3429);
  c.border = QColor(0x3A3936);
  c.border_strong = QColor(0x4D4B46);
  c.text = QColor(0xF5F4EE);
  c.text_secondary = QColor(0xA8A49A);
  c.text_disabled = QColor(0x6E6B64);
  c.accent = QColor(0xD97757);
  c.accent_hover = QColor(0xE38B6D);
  c.accent_text = QColor(0xFFFFFF);
  c.tooltip = QColor(0xF0EEE6);
  c.tooltip_text = QColor(0x1F1E1D);
  return c;

}

bool ClaudeTheme::IsEnabledInSettings() {

  Settings s;
  s.beginGroup(AppearanceSettings::kSettingsGroup);
  const QString theme = s.value(AppearanceSettings::kTheme, QLatin1String(AppearanceSettings::kDefaultTheme)).toString();
  s.endGroup();

  return theme == QLatin1String(AppearanceSettings::kThemeClaude);

}

bool ClaudeTheme::IsActive() {

  return instance()->active_;

}

const ClaudeTheme::Colors &ClaudeTheme::colors() {

  return instance()->colors_;

}

bool ClaudeTheme::WantDark() {

  Settings s;
  s.beginGroup(AppearanceSettings::kSettingsGroup);
  const bool dark_mode = s.value(AppearanceSettings::kDarkMode, AppearanceSettings::kDefaultDarkMode).toBool();
  s.endGroup();
  if (dark_mode) return true;

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
  return false;
#endif

}

void ClaudeTheme::Apply() {

  ClaudeTheme *theme = instance();

  // Fusion draws everything from the palette and honours the stylesheet everywhere, the native macOS style draws native controls which ignore both.
  if (!QApplication::style() || QApplication::style()->name().compare("fusion"_L1, Qt::CaseInsensitive) != 0) {
    if (!QApplication::setStyle(u"Fusion"_s)) {
      qLog(Error) << "Claude theme: could not set the Fusion style";
    }
  }

  theme->active_ = true;
  theme->Update();

}

void ClaudeTheme::Remove(const QString &style) {

  ClaudeTheme *theme = instance();
  if (!theme->active_) return;

  theme->active_ = false;
  theme->colors_ = LightColors();
  qApp->setStyleSheet(QString());
  if (!style.isEmpty()) QApplication::setStyle(style);
  QApplication::setPalette(QApplication::style() ? QApplication::style()->standardPalette() : QPalette());

}

void ClaudeTheme::Update() {

  colors_ = WantDark() ? DarkColors() : LightColors();

  qLog(Debug) << "Claude theme:" << (colors_.dark ? "dark" : "light");

  QApplication::setPalette(Palette(colors_));
  qApp->setStyleSheet(StyleSheet(colors_));

}

QPalette ClaudeTheme::Palette(const Colors &c) {

  QPalette palette;

  for (const QPalette::ColorGroup group : { QPalette::Active, QPalette::Inactive, QPalette::Disabled }) {
    const bool disabled = group == QPalette::Disabled;
    palette.setColor(group, QPalette::Window, c.window);
    palette.setColor(group, QPalette::WindowText, disabled ? c.text_disabled : c.text);
    palette.setColor(group, QPalette::Base, c.surface);
    palette.setColor(group, QPalette::AlternateBase, c.surface_alt);
    palette.setColor(group, QPalette::Text, disabled ? c.text_disabled : c.text);
    palette.setColor(group, QPalette::PlaceholderText, c.text_disabled);
    palette.setColor(group, QPalette::Button, c.raised);
    palette.setColor(group, QPalette::ButtonText, disabled ? c.text_disabled : c.text);
    palette.setColor(group, QPalette::BrightText, c.accent);
    palette.setColor(group, QPalette::ToolTipBase, c.tooltip);
    palette.setColor(group, QPalette::ToolTipText, c.tooltip_text);
    // Fusion draws bevels from these, keep them close to the surfaces so nothing looks embossed.
    palette.setColor(group, QPalette::Light, c.dark ? c.raised.lighter(115) : QColor(0xFFFFFF));
    palette.setColor(group, QPalette::Midlight, c.hover);
    palette.setColor(group, QPalette::Mid, c.border_strong);
    palette.setColor(group, QPalette::Dark, c.border_strong.darker(115));
    palette.setColor(group, QPalette::Shadow, c.dark ? QColor(0, 0, 0, 160) : QColor(0, 0, 0, 60));
    // Selections are a soft accent tint with the normal text colour, like the Claude apps, the strong accent is used for sliders, progress and default buttons.
    palette.setColor(group, QPalette::Highlight, disabled ? c.hover : c.selected);
    palette.setColor(group, QPalette::HighlightedText, disabled ? c.text_disabled : c.text);
    palette.setColor(group, QPalette::Link, c.accent);
    palette.setColor(group, QPalette::LinkVisited, c.accent_hover);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(group, QPalette::Accent, disabled ? c.text_disabled : c.accent);
#endif
  }

  return palette;

}

QString ClaudeTheme::StyleSheet(const Colors &c) {

  QFile file(u":/style/claude.css"_s);
  if (!file.open(QIODevice::ReadOnly)) {
    qLog(Error) << "Claude theme: could not open the stylesheet" << file.fileName() << file.errorString();
    return QString();
  }
  QString css = QString::fromUtf8(file.readAll());
  file.close();

  const QList<std::pair<QString, QColor>> tokens = {
    { u"window"_s, c.window },
    { u"sidebar"_s, c.sidebar },
    { u"surface"_s, c.surface },
    { u"surface_alt"_s, c.surface_alt },
    { u"raised"_s, c.raised },
    { u"hover"_s, c.hover },
    { u"selected"_s, c.selected },
    { u"border"_s, c.border },
    { u"border_strong"_s, c.border_strong },
    { u"text"_s, c.text },
    { u"text_secondary"_s, c.text_secondary },
    { u"text_disabled"_s, c.text_disabled },
    { u"accent"_s, c.accent },
    { u"accent_hover"_s, c.accent_hover },
    { u"accent_text"_s, c.accent_text },
    { u"tooltip"_s, c.tooltip },
    { u"tooltip_text"_s, c.tooltip_text },
    { u"scrollbar"_s, c.dark ? QColor(255, 255, 255, 56) : QColor(31, 30, 29, 56) },
    { u"scrollbar_hover"_s, c.dark ? QColor(255, 255, 255, 110) : QColor(31, 30, 29, 110) },
  };
  for (const std::pair<QString, QColor> &token : tokens) {
    css.replace(u"{{"_s + token.first + u"}}"_s, CssColor(token.second));
  }
  css.replace("{{scheme}}"_L1, c.dark ? "dark"_L1 : "light"_L1);

  if (css.contains("{{"_L1)) {
    qLog(Warning) << "Claude theme: the stylesheet has unknown placeholders left";
  }

  return css;

}
