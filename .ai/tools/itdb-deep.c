/*
 * itdb-deep: read-only diagnostic for an iPod's iTunesDB.
 *
 * Prints track count, playlist count, and per-playlist info (member count,
 * whether it is the MPL, etc.). Use this to confirm Bug #5 symptoms
 * BEFORE attempting any recovery, and to verify success AFTER.
 *
 * Disk-level fingerprint of Bug #5 (.ai/10-ipod-sync.md §10.8):
 *     Tracks: 0
 *     Playlists: 1
 *     Playlist 0: "<user's iPod name>"
 *       is_mpl: 1
 *       Members in list: 0
 * combined with iPod_Control/Music/F##/ being full of .m4a files
 * (find /Volumes/iPod/iPod_Control/Music -name '*.m4a' | wc -l > 0).
 *
 * Build & run: see .ai/tools/README.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include "itdb.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ipod-mount>\n", argv[0]);
        return 2;
    }

    GError *error = NULL;
    Itdb_iTunesDB *db = itdb_parse(argv[1], &error);
    if (!db) {
        fprintf(stderr, "itdb_parse(%s) failed: %s\n",
                argv[1], error ? error->message : "unknown");
        return 1;
    }

    printf("=== DB Info ===\n");
    printf("Tracks: %u\n", itdb_tracks_number(db));
    printf("Playlists: %u\n", itdb_playlists_number(db));
    printf("DB version: 0x%x\n", db->version);
    printf("DB id: 0x%llx\n", (unsigned long long)db->id);
    printf("Filename: %s\n", db->filename ? db->filename : "(null)");
    printf("Mountpoint: %s\n", itdb_get_mountpoint(db));

    int pl_i = 0;
    for (GList *pls = db->playlists; pls != NULL; pls = pls->next, pl_i++) {
        Itdb_Playlist *pl = (Itdb_Playlist*)pls->data;
        printf("\nPlaylist %d: %s\n", pl_i, pl->name ? pl->name : "(null)");
        printf("  is_mpl: %d\n", itdb_playlist_is_mpl(pl));
        printf("  type: %u\n", pl->type);
        printf("  flag1: %u flag2: %u flag3: %u\n",
               pl->flag1, pl->flag2, pl->flag3);
        printf("  num: %u\n", pl->num);
        printf("  Members in list: %u\n", g_list_length(pl->members));
        printf("  itdb_playlist_tracks_number: %u\n",
               itdb_playlist_tracks_number(pl));
    }

    /* Note: when itdb_parse runs against a Bug #5'ed iPod whose ArtworkDB
     * still references orphan dbids, libgpod will print one line per orphan
     * to stderr:
     *     "Could not find corresponding track (dbid: ...) for artwork entry."
     * This is informational, not an error. The orphans are cleaned up the
     * next time the iTunesDB is written. */

    itdb_free(db);
    return 0;
}