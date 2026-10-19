#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Standalone iTunesDB parser (no libgpod dependency). Reads the raw binary
# iTunesDB and dumps the grouping-relevant fields of each mhit (track) so we
# can see exactly which field differs across the tracks of a split audiobook.
#
# Offsets/format taken directly from libgpod's itdb_itunesdb.c get_mhit() and
# extract_mhod_string().

import struct
import sys

# mhit numeric fields: name -> (offset, size)
MHIT_NUM = {
    "id":            (16, 4),
    "compilation":   (30, 1),
    "track_nr":      (44, 4),
    "tracks":        (48, 4),
    "year":          (52, 4),
    "cd_nr":         (92, 4),
    "cds":           (96, 4),
    "dbid":          (112, 8),
    "skip_shuffle":  (165, 1),
    "remember_pos":  (166, 1),
    "flag4":         (167, 1),
    "dbid2":         (168, 8),
    "mark_unplayed": (178, 1),
    "mediatype":     (208, 4),
}

# mhod string type id -> name
MHOD_STR = {
    1: "title", 3: "album", 4: "artist", 5: "genre", 8: "comment",
    9: "category", 12: "composer", 13: "grouping", 14: "description",
    16: "podcastrss", 18: "subtitle", 22: "albumartist",
    23: "sort_artist", 27: "sort_title", 28: "sort_album",
    29: "sort_albumartist", 30: "sort_composer",
}

TEXT_FIELDS = ["title", "album", "artist", "albumartist", "sort_album",
               "sort_albumartist", "sort_artist", "sort_title", "composer",
               "grouping", "genre", "subtitle", "description", "podcastrss"]
NUM_FIELDS = ["mediatype", "compilation", "cd_nr", "cds", "track_nr", "tracks",
              "year", "flag4", "skip_shuffle", "remember_pos", "mark_unplayed",
              "dbid", "dbid2"]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def read_num(b, o, size):
    if size == 1:
        return b[o]
    if size == 4:
        return struct.unpack_from("<I", b, o)[0]
    if size == 8:
        return struct.unpack_from("<Q", b, o)[0]


def extract_mhod_string(b, seek):
    # seek points at the data area (mhod_seek + header_length)
    string_type = u32(b, seek)
    length = u32(b, seek + 4)
    raw = b[seek + 16: seek + 16 + length]
    if string_type != 0x02:
        try:
            return raw.decode("utf-16-le", "replace")
        except Exception:
            return repr(raw)
    try:
        return raw.decode("utf-8", "replace")
    except Exception:
        return repr(raw)


def parse(path):
    with open(path, "rb") as f:
        b = f.read()

    tracks = []
    seek = 0
    n = len(b)
    while True:
        i = b.find(b"mhit", seek)
        if i < 0:
            break
        header_len = u32(b, i + 4)
        if header_len < 0x9c or i + header_len > n:
            seek = i + 4
            continue
        mhod_nums = u32(b, i + 12)

        rec = {}
        for name, (off, size) in MHIT_NUM.items():
            if off + size <= header_len:
                rec[name] = read_num(b, i + off, size)

        # walk mhods
        s = i + header_len
        for _ in range(mhod_nums):
            if s + 12 > n or b[s:s + 4] != b"mhod":
                break
            mhod_len = u32(b, s + 8)
            mtype = u32(b, s + 12)
            hdr = u32(b, s + 4)
            if mtype in MHOD_STR and mhod_len > hdr:
                rec[MHOD_STR[mtype]] = extract_mhod_string(b, s + hdr)
            if mhod_len <= 0:
                break
            s += mhod_len
        tracks.append(rec)
        seek = i + header_len
    return tracks


def matches(rec, needle):
    needle = needle.lower()
    for k in ("album", "title", "artist", "albumartist"):
        v = rec.get(k)
        if v and needle in v.lower():
            return True
    return False


def main(argv):
    if len(argv) < 2:
        print("Usage: parse_itunesdb.py <iTunesDB path> [match-substring]")
        return 2
    path = argv[1]
    needle = argv[2] if len(argv) > 2 else None

    tracks = parse(path)
    print("Parsed %d track(s) from %s\n" % (len(tracks), path))

    sel = [t for t in tracks if (needle is None or matches(t, needle))]
    if not sel:
        print("No tracks matched %r." % needle)
        print("Albums present:")
        for a in sorted(set(t.get("album", "") or "" for t in tracks)):
            print("   ", repr(a))
        return 0

    for idx, rec in enumerate(sel):
        print("=== track %d ===" % idx)
        for k in TEXT_FIELDS:
            if k in rec:
                print("   %-18s = %r" % (k, rec[k]))
        for k in NUM_FIELDS:
            if k in rec:
                print("   %-18s = %r" % (k, rec[k]))
        print()

    # Highlight which fields DIFFER across the selected tracks.
    print("--- fields that DIFFER across the %d matched tracks ---" % len(sel))
    allkeys = TEXT_FIELDS + NUM_FIELDS
    for k in allkeys:
        vals = [rec.get(k) for rec in sel]
        if len(set(vals)) > 1:
            print("   %-18s : %r" % (k, vals))
    print("--- (fields NOT listed above are identical across all matched tracks) ---")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
