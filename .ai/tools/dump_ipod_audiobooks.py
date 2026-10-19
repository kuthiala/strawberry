#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# dump_ipod_audiobooks.py
#
# Diagnostic tool: parse an iPod's iTunesDB using libgpod and dump the raw
# Itdb_Track fields that could influence how the iPod firmware groups tracks
# into audiobooks. Use this to compare the tracks of a single audiobook that
# the iPod wrongly split into several entries, and find the ACTUAL differing
# field (instead of guessing).
#
# Requirements: the `gpod` Python bindings (same libgpod Strawberry uses).
#   - Debian/Ubuntu: apt-get install python3-gpod  (or python-gpod)
#   - It must be the same libgpod that wrote the DB.
#
# Usage:
#   python dump_ipod_audiobooks.py /path/to/ipod/mountpoint
#   python dump_ipod_audiobooks.py /path/to/ipod/mountpoint --all   # every track
#   python dump_ipod_audiobooks.py /path/to/ipod/mountpoint --album "Book Name"
#
# The mountpoint is the root of the iPod filesystem (the folder that contains
# the "iPod_Control" directory).

from __future__ import print_function

import sys

try:
    import gpod
except ImportError:
    sys.stderr.write(
        "ERROR: the 'gpod' Python module is not available.\n"
        "Install libgpod's python bindings (e.g. 'apt-get install python3-gpod')\n"
        "and run this with the matching python interpreter.\n")
    sys.exit(2)


# ITDB_MEDIATYPE_AUDIOBOOK is (1 << 3) == 8. Prefer the library constant if
# present, fall back to the literal so the script still runs on odd builds.
AUDIOBOOK = getattr(gpod, "ITDB_MEDIATYPE_AUDIOBOOK", 1 << 3)

# The fields the iPod firmware is known (or plausibly suspected) to use when
# grouping/labelling items in the Music menus. We dump ALL of them verbatim so
# we can diff the split tracks and see which one actually differs. Add/remove
# freely -- every name here is a real Itdb_Track member.
FIELDS = [
    "title", "artist", "album", "albumartist", "sort_artist", "sort_album",
    "sort_albumartist", "sort_title", "composer", "grouping", "genre",
    "comment", "category", "description", "subtitle",
    "track_nr", "tracks", "cd_nr", "cds",
    "compilation", "mediatype", "flag4", "mark_unplayed",
    "skip_when_shuffling", "remember_playback_position", "movie_flag",
    "lyrics_flag", "filetype", "dbid", "dbid2", "year",
]


def get(track, name):
    try:
        value = getattr(track, name)
    except AttributeError:
        return "<no such field>"
    # libgpod returns ints for numeric fields and str/bytes for text fields.
    if isinstance(value, bytes):
        try:
            return value.decode("utf-8", "replace")
        except Exception:
            return repr(value)
    return value


def dump_track(index, track):
    print("  [track %d]" % index)
    for name in FIELDS:
        print("      %-28s = %r" % (name, get(track, name)))
    print("")


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("Usage: %s <ipod-mountpoint> [--all|--album NAME]\n" % argv[0])
        return 2

    mountpoint = argv[1]
    show_all = "--all" in argv
    album_filter = None
    if "--album" in argv:
        i = argv.index("--album")
        if i + 1 < len(argv):
            album_filter = argv[i + 1]

    itdb = gpod.itdb_parse(mountpoint, None)
    if not itdb:
        sys.stderr.write("ERROR: could not parse an iTunesDB at %r.\n"
                         "Point me at the iPod's root mount folder (the one\n"
                         "containing 'iPod_Control').\n" % mountpoint)
        return 1

    tracks = gpod.sw_get_tracks(itdb)
    total = gpod.sw_get_list_len(tracks)
    print("Parsed iTunesDB at %s -- %d tracks total\n" % (mountpoint, total))

    shown = 0
    for n in range(total):
        track = gpod.sw_get_track(tracks, n)

        if album_filter is not None:
            album = get(track, "album")
            if album != album_filter:
                continue
        elif not show_all:
            # Default: only audiobook-mediatype tracks.
            mt = get(track, "mediatype")
            if not (isinstance(mt, int) and (mt & AUDIOBOOK)):
                continue

        dump_track(n, track)
        shown += 1

    if shown == 0:
        print("No matching tracks found.")
        if not show_all and album_filter is None:
            print("(No tracks had the AUDIOBOOK mediatype bit set. Re-run with")
            print(" '--album \"Your Book Name\"' or '--all' to inspect them anyway.)")
    else:
        print("Dumped %d track(s). Compare the fields above across the tracks of")
        print("the split audiobook and look for the one that DIFFERS -- that is the")
        print("field the iPod is splitting on.")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
