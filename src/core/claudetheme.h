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

#ifndef CLAUDETHEME_H
#define CLAUDETHEME_H

#include "config.h"

#include <QObject>
#include <QColor>
#include <QString>
#include <QPalette>

// The "Claude" look: warm ivory / charcoal surfaces, a terracotta accent, rounded controls and flat, quiet chrome.
//
// The theme is applied application-wide, in three layers:
//  1. The Fusion style, since the native macOS style ignores most of the palette and stylesheet.
//  2. A QPalette built from the design tokens below, so every widget, including custom painted ones, gets the colours.
//  3. data/style/claude.css, an application stylesheet with {{token}} placeholders, filled in from the same tokens.
// Widgets painting themselves (FancyTabBar / FancyTabWidget sidebar) ask IsActive() and colors() to paint flat.
//
// Light or dark follows the system colour scheme, or is dark when the "Dark mode" appearance setting is on, and switches live when the system scheme changes.
class ClaudeTheme : public QObject {
  Q_OBJECT

 public:
  // Design tokens, the same names are used as {{placeholders}} in claude.css.
  struct Colors {
    bool dark = false;
    QColor window;          // Main background.
    QColor sidebar;         // Sidebar and tool bars, a step darker than the window.
    QColor surface;         // Lists, inputs and cards.
    QColor surface_alt;     // Alternating rows.
    QColor raised;          // Buttons.
    QColor hover;           // Hovered rows and buttons.
    QColor selected;        // Selected rows, the accent mixed into the surface.
    QColor border;          // Hairlines.
    QColor border_strong;   // Input and button outlines.
    QColor text;
    QColor text_secondary;
    QColor text_disabled;
    QColor accent;
    QColor accent_hover;
    QColor accent_text;     // Text on the accent.
    QColor tooltip;
    QColor tooltip_text;
  };

  static Colors LightColors();
  static Colors DarkColors();

  // True when the theme is chosen in the appearance settings, which it is by default.
  static bool IsEnabledInSettings();
  // True when the theme is applied.
  static bool IsActive();
  // The tokens of the applied scheme, the light tokens when the theme isn't active.
  static const Colors &colors();

  // Applies the theme, or re-applies it, for example after the dark mode setting changed. Must be called after the QApplication is created.
  static void Apply();
  // Removes the theme and goes back to the given style with its own palette.
  static void Remove(const QString &style);

  static QPalette Palette(const Colors &colors);
  static QString StyleSheet(const Colors &colors);

 private:
  explicit ClaudeTheme(QObject *parent = nullptr);
  static ClaudeTheme *instance();
  static bool WantDark();
  void Update();

 private:
  bool active_;
  Colors colors_;
};

#endif  // CLAUDETHEME_H
