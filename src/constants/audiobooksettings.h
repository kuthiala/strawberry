/*
 * Strawberry Music Player
 * Copyright 2024, Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef AUDIOBOOKSETTINGS_H
#define AUDIOBOOKSETTINGS_H

namespace AudiobookSettings {

constexpr char kSettingsGroup[] = "Audiobook";

// Master toggle: when true, multiple chapter files sharing the same book are
// combined into a single chaptered .m4b before syncing to a device. When false,
// chapters are synced individually (legacy behaviour).
constexpr char kMergeEnabled[] = "merge_enabled";
constexpr bool kMergeEnabledDefault = true;

// When true, always re-encode to AAC even if a lossless stream-copy is possible.
// Normally false so AAC/ALAC-in-MP4 inputs are combined losslessly.
constexpr char kForceReencode[] = "force_reencode";
constexpr bool kForceReencodeDefault = false;

// AAC bitrate used when the book must be (or is forced to be) re-encoded.
// A plain ffmpeg bitrate string such as "128k", "192k", "256k".
constexpr char kAacBitrate[] = "aac_bitrate";
constexpr char kAacBitrateDefault[] = "256k";

}  // namespace AudiobookSettings

#endif  // AUDIOBOOKSETTINGS_H
