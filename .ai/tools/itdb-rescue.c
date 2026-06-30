/*
 * itdb-rescue: rebuild an iPod's iTunesDB after Strawberry's Bug #5 left
 *              it empty while the audio files were still on disk.
 *
 * --------------------------------------------------------------------------
 *  HOW MATCHING WORKS (READ THIS BEFORE TOUCHING THE CODE)
 * --------------------------------------------------------------------------
 *
 * We need to figure out which Strawberry collection song each on-disk
 * `iPod_Control/Music/F##/<random>.{m4a,mp3,aac}` was copied or transcoded
 * from. The on-iPod files have no useful tags (Strawberry's Transcoder
 * strips them — see Bug #6 in `.ai/10-ipod-sync.md`) and libgpod's
 * filenames are random.
 *
 * The join key is **microsecond-accurate duration**:
 *
 *     duration_us = duration_ts * 1_000_000 / sample_rate
 *
 * This is exact for FLAC, ALAC, and AAC (their stream `time_base` is
 * `1/sample_rate`, so `duration_ts` IS the sample count). MP3's
 * `time_base` is the historical 1/14112000 quirk and there's no useful
 * `duration_ts`, so we fall back to FFmpeg's format-level `duration` in
 * fractional seconds and multiply by 10^6 — still microsecond-accurate
 * because FFmpeg derives it from the frame headers.
 *
 * Two cases this needs to handle:
 *
 *   - **FLAC -> ALAC (lossless transcode)**. Strawberry runs the source
 *     FLAC through GStreamer (`audioconvert -> audioresample -> alacenc
 *     -> mp4mux`) and writes a `.m4a` to the iPod. ALAC is lossless, so
 *     the on-iPod m4a has the **same sample count** as the source FLAC.
 *     duration_us therefore matches exactly.
 *
 *   - **MP3 / AAC -> direct copy (no transcode)**. The iPod natively
 *     plays these, so Strawberry copies the source file byte-for-byte.
 *     Every probe field is identical between source and on-iPod copy.
 *
 * Failure modes the tool handles:
 *
 *   - **Source file moved or unmounted** ("/Volumes/Satellite" not
 *     plugged in): the source probe fails for that song; we leave the
 *     iPod file unmatched and write it as `"Unknown: <basename>"` so
 *     the user can spot the casualties rather than seeing wrong names.
 *
 *   - **Two source files with the exact same microsecond duration**:
 *     extraordinarily rare (microseconds is ~unique per song in any
 *     real library) but we still tiebreak by file-size delta. If both
 *     candidates are within 1% of each other in size delta to the iPod
 *     file, we **refuse to guess** and mark the file `"Unknown"`.
 *
 *   - **MP3 -> MP3 direct copy of a file whose source has been re-tagged
 *     since the sync**: tag-rewrites in many MP3 editors change padding
 *     bytes but not audio content, so sample count is unchanged.
 *     duration_us still matches and the tool recovers metadata cleanly.
 *
 * Why NOT match by content hash? FLAC->ALAC changes bytes; only direct
 * copies are hash-stable. Microseconds works for both paths uniformly.
 *
 * Why NOT match by URL/filename written into the on-iPod file? libgpod
 * assigns random names and Strawberry's Transcoder strips most tags.
 *
 * Why NOT match by `strawberry.db`'s `length` field? It stores
 * milliseconds, which has ~1000x worse resolution than microseconds —
 * fine for fingerprinting unique songs in isolation, but with 3,500+
 * songs collisions become rampant (we observed 64 songs in the rounded-
 * seconds bucket in our test library).
 *
 * --------------------------------------------------------------------------
 *  WHEN TO USE THIS TOOL
 * --------------------------------------------------------------------------
 *
 * Use this ONLY when:
 *
 *   1. `iPod_Control/iTunes/iTunesDB` on the device is the empty 16-20 KB
 *      stub (`/tmp/itdb-deep <mount>` reports `Tracks: 0`), AND
 *   2. `iPod_Control/Music/F##/` is full of audio files, AND
 *   3. Your music collection's source files are mounted and readable
 *      (e.g. `/Volumes/Satellite/Music/...` on the user's machine).
 *
 * See `.ai/tools/README.md` for the full step-by-step including when
 * NOT to use this tool and the wipe-and-re-sync alternative.
 *
 * --------------------------------------------------------------------------
 *  USAGE
 * --------------------------------------------------------------------------
 *
 *   itdb-rescue <ipod-mount> <strawberry-db> [--dry-run] [--cover-dir <d>]
 *
 *   Default cover-cache dir:
 *     $HOME/Library/Application Support/Strawberry/Strawberry/devicealbumcovers
 *
 *   The tool will:
 *     1. itdb_parse the existing iTunesDB (inherits MPL + SysInfo).
 *     2. ffprobe every iPod audio file for codec, sample_rate,
 *        duration_ts (or format duration) -> duration_us.
 *     3. SELECT every collection song from strawberry.db, ffprobe its
 *        source file for the same fields.
 *     4. Match by EXACT duration_us, file-size tiebreak, refuse to guess
 *        when ambiguous.
 *     5. Set track->filetype, type1, type2 based on the on-iPod codec
 *        (see .ai/10-ipod-sync.md §10.4 for why the strings matter).
 *     6. itdb_track_set_thumbnails using the SHA1-named JPEGs in the
 *        Strawberry cover cache (matches CoverUtils::Sha1CoverHash).
 *     7. itdb_write to persist + sign the iTunesDB (hash58 via SysInfo
 *        FirewireGuid).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <glib.h>
#include <sqlite3.h>
#include "itdb.h"

/* ====================================================================== */
/*  Codec & iTunesDB filetype mapping                                     */
/* ====================================================================== */

typedef enum {
    CODEC_UNKNOWN = 0,
    CODEC_AAC,
    CODEC_ALAC,
    CODEC_MP3,
    CODEC_FLAC,  /* source-side only */
    CODEC_WAV,
    CODEC_AIFF
} Codec;

static Codec codec_from_name(const char *name) {
    if (!name)                              return CODEC_UNKNOWN;
    if (strcmp(name, "aac")  == 0)          return CODEC_AAC;
    if (strcmp(name, "alac") == 0)          return CODEC_ALAC;
    if (strcmp(name, "mp3")  == 0)          return CODEC_MP3;
    if (strcmp(name, "flac") == 0)          return CODEC_FLAC;
    if (strcmp(name, "pcm_s16le") == 0 ||
        strcmp(name, "pcm_s24le") == 0)     return CODEC_WAV;
    if (strcmp(name, "pcm_s16be") == 0 ||
        strcmp(name, "pcm_s24be") == 0)     return CODEC_AIFF;
    return CODEC_UNKNOWN;
}

/* iPod firmware hides tracks from Music menus unless `filetype` is the
 * exact English string libgpod's itdb_track_set_defaults() recognises.
 * See .ai/10-ipod-sync.md §10.4. */
static void itunesdb_filetype_for(Codec codec, const char *path,
                                  const char **filetype_out,
                                  int *type1_out, int *type2_out) {
    if (codec == CODEC_UNKNOWN && path) {
        const char *dot = strrchr(path, '.');
        if (dot) {
            if      (strcasecmp(dot, ".mp3")  == 0) codec = CODEC_MP3;
            else if (strcasecmp(dot, ".m4a")  == 0) codec = CODEC_AAC;
            else if (strcasecmp(dot, ".aac")  == 0) codec = CODEC_AAC;
            else if (strcasecmp(dot, ".flac") == 0) codec = CODEC_FLAC;
            else if (strcasecmp(dot, ".wav")  == 0) codec = CODEC_WAV;
            else if (strcasecmp(dot, ".aif")  == 0 ||
                     strcasecmp(dot, ".aiff") == 0) codec = CODEC_AIFF;
        }
    }
    switch (codec) {
        case CODEC_MP3:
            *filetype_out = "MPEG audio file";
            /* libgpod itdb.h: type1=type2=1 for VBR MP3, 0/0 for CBR.
             * 1/1 works for both on the iPod Classic firmware. */
            *type1_out = 1; *type2_out = 1; return;
        case CODEC_ALAC:
            *filetype_out = "Apple Lossless audio file";
            *type1_out = 0; *type2_out = 0; return;
        case CODEC_AAC:
            *filetype_out = "AAC audio file";
            *type1_out = 0; *type2_out = 0; return;
        case CODEC_FLAC:
            *filetype_out = "FLAC audio file";
            *type1_out = 0; *type2_out = 0; return;
        case CODEC_WAV:
            *filetype_out = "WAV audio file";
            *type1_out = 0; *type2_out = 0; return;
        case CODEC_AIFF:
            *filetype_out = "AIFF audio file";
            *type1_out = 0; *type2_out = 0; return;
        case CODEC_UNKNOWN:
        default:
            *filetype_out = "AAC audio file";  /* safe default for .m4a */
            *type1_out = 0; *type2_out = 0; return;
    }
}

/* ====================================================================== */
/*  Types                                                                 */
/* ====================================================================== */

typedef struct {
    char    *path;                /* full filesystem path on the iPod   */
    char    *ipod_path;           /* ":iPod_Control:Music:F00:libgpodXXXX.m4a" */
    int64_t  duration_us;         /* JOIN KEY                            */
    int      samplerate;
    int64_t  size;                /* bytes on disk                       */
    int      bitrate_kbps;
    Codec    codec;
    int      matched_song_idx;    /* -1 = no match                       */
} IpodFile;

typedef struct {
    /* from strawberry.db */
    char *title, *artist, *album, *albumartist, *composer, *genre;
    int   track, disc, year;
    int   length_ms;              /* informational only                  */

    /* from probing source_path */
    char    *source_path;
    int64_t  source_duration_us;  /* JOIN KEY                            */
    int      source_samplerate;
    int64_t  source_size;
    Codec    source_codec;
    int      probe_ok;            /* nonzero iff source_duration_us > 0  */
    int      matched;             /* used during matching                */
} CollSong;

/* ====================================================================== */
/*  Tiny string utilities                                                 */
/* ====================================================================== */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* RFC 3986 percent-decode (in place). Good enough for file:// URLs. */
static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int hi = (s[1] >= '0' && s[1] <= '9') ? s[1] - '0' :
                     (s[1] >= 'a' && s[1] <= 'f') ? s[1] - 'a' + 10 :
                     (s[1] >= 'A' && s[1] <= 'F') ? s[1] - 'A' + 10 : -1;
            int lo = (s[2] >= '0' && s[2] <= '9') ? s[2] - '0' :
                     (s[2] >= 'a' && s[2] <= 'f') ? s[2] - 'a' + 10 :
                     (s[2] >= 'A' && s[2] <= 'F') ? s[2] - 'A' + 10 : -1;
            if (hi >= 0 && lo >= 0) {
                *o++ = (char)((hi << 4) | lo);
                s += 3;
                continue;
            }
        }
        *o++ = *s++;
    }
    *o = 0;
}

static char *file_url_to_path(const char *url) {
    if (!url) return NULL;
    const char *p = NULL;
    if (strncmp(url, "file://", 7) == 0) p = url + 7;
    else if (url[0] == '/')              p = url;
    else return NULL;
    char *out = strdup(p);
    url_decode(out);
    return out;
}

/* ASCII tolower; matches CoverUtils::Sha1CoverHash for ASCII text. */
static char *ascii_lower(const char *s) {
    if (!s) return strdup("");
    size_t L = strlen(s);
    char *r = malloc(L + 1);
    for (size_t i = 0; i < L; i++) {
        unsigned char c = (unsigned char)s[i];
        r[i] = (c < 0x80) ? (char)tolower(c) : (char)c;
    }
    r[L] = 0;
    return r;
}

/* ====================================================================== */
/*  ffprobe wrapper                                                       */
/* ====================================================================== */

/* Returns 1 on success (duration_us > 0), 0 otherwise. */
static int probe_audio(const char *path,
                       int64_t *duration_us_out,
                       int     *samplerate_out,
                       int64_t *size_out,
                       int     *bitrate_kbps_out,
                       Codec   *codec_out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (size_out) *size_out = (int64_t)st.st_size;

    /* Shell-quote the path (single-quote escape). */
    GString *cmd = g_string_new(NULL);
    g_string_append(cmd,
        "ffprobe -v error -select_streams a:0 -show_entries "
        "stream=codec_name,duration_ts,sample_rate"
        ":format=bit_rate,duration "
        "-of default=noprint_wrappers=1 '");
    for (const char *p = path; *p; p++) {
        if (*p == '\'') g_string_append(cmd, "'\\''");
        else g_string_append_c(cmd, *p);
    }
    g_string_append(cmd, "' 2>/dev/null");

    FILE *fp = popen(cmd->str, "r");
    g_string_free(cmd, TRUE);
    if (!fp) return 0;

    int64_t dur_ts = 0;
    int     sr = 0, br_kbps = 0;
    double  fmt_dur_sec = 0.0;
    char    codec_name[64] = {0};

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (!*val || strcmp(val, "N/A") == 0) continue;
        if      (strcmp(key, "duration_ts") == 0) dur_ts = (int64_t)strtoll(val, NULL, 10);
        else if (strcmp(key, "sample_rate") == 0) sr     = atoi(val);
        else if (strcmp(key, "bit_rate")    == 0) br_kbps = atoi(val) / 1000;
        else if (strcmp(key, "duration")    == 0) fmt_dur_sec = atof(val);
        else if (strcmp(key, "codec_name")  == 0) {
            strncpy(codec_name, val, sizeof(codec_name) - 1);
            codec_name[sizeof(codec_name) - 1] = 0;
        }
    }
    pclose(fp);

    Codec   codec = codec_from_name(codec_name);
    int64_t dur_us = 0;

    /* FLAC/ALAC/AAC: duration_ts is in 1/sample_rate units (= sample count).
     * Exact integer arithmetic. */
    if (sr > 0 && dur_ts > 0 &&
        (codec == CODEC_FLAC || codec == CODEC_ALAC || codec == CODEC_AAC)) {
        dur_us = (dur_ts * 1000000LL) / sr;
    }
    /* MP3 / WAV / AIFF / unknown: use format-level duration. */
    else if (fmt_dur_sec > 0.0) {
        dur_us = (int64_t)(fmt_dur_sec * 1000000.0 + 0.5);
    }

    if (duration_us_out)  *duration_us_out  = dur_us;
    if (samplerate_out)   *samplerate_out   = sr;
    if (bitrate_kbps_out) *bitrate_kbps_out = br_kbps;
    if (codec_out)        *codec_out        = codec;
    return dur_us > 0;
}

/* ====================================================================== */
/*  Collect on-disk iPod audio files                                      */
/* ====================================================================== */

static int compare_path(const void *a, const void *b) {
    return strcmp((*(const IpodFile *const *)a)->path,
                  (*(const IpodFile *const *)b)->path);
}

static IpodFile **collect_files(const char *mount, int *n_out) {
    char music_dir[1024];
    snprintf(music_dir, sizeof(music_dir), "%s/iPod_Control/Music", mount);
    DIR *d = opendir(music_dir);
    if (!d) {
        fprintf(stderr, "Cannot open %s: %s\n", music_dir, strerror(errno));
        return NULL;
    }
    size_t cap = 4096, n = 0;
    IpodFile **out = malloc(cap * sizeof(IpodFile *));
    size_t mount_len = strlen(mount);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] != 'F') continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", music_dir, de->d_name);
        DIR *sd = opendir(sub);
        if (!sd) continue;
        struct dirent *fe;
        while ((fe = readdir(sd)) != NULL) {
            const char *name = fe->d_name;
            size_t L = strlen(name);
            if (L < 5) continue;
            const char *ext = name + L - 4;
            if (strcasecmp(ext, ".m4a") != 0 &&
                strcasecmp(ext, ".mp3") != 0 &&
                strcasecmp(ext, ".aac") != 0) continue;
            if (n + 1 >= cap) { cap *= 2; out = realloc(out, cap * sizeof(IpodFile *)); }
            IpodFile *f = calloc(1, sizeof(IpodFile));
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", sub, name);
            f->path = strdup(full);
            const char *rel = f->path + mount_len;
            f->ipod_path = strdup(rel);
            for (char *p = f->ipod_path; *p; p++) if (*p == '/') *p = ':';
            f->matched_song_idx = -1;
            out[n++] = f;
        }
        closedir(sd);
    }
    closedir(d);
    qsort(out, n, sizeof(IpodFile *), compare_path);
    *n_out = (int)n;
    return out;
}

/* ====================================================================== */
/*  Load Strawberry collection                                            */
/* ====================================================================== */

static CollSong *load_collection(const char *db_path, int *n_out) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open(%s) failed: %s\n",
                db_path, sqlite3_errmsg(db));
        return NULL;
    }
    /* source = 2 is Song::Source::Collection. */
    const char *sql =
        "SELECT title, artist, album, albumartist, composer, genre, "
        "       track, disc, year, length, url "
        "FROM songs "
        "WHERE source = 2 "
        "  AND unavailable = 0 "
        "ORDER BY albumartist, album, disc, track, title";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_prepare_v2 failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    size_t cap = 4096, n = 0;
    CollSong *out = calloc(cap, sizeof(CollSong));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n + 1 >= cap) {
            cap *= 2;
            out = realloc(out, cap * sizeof(CollSong));
            memset(out + n, 0, (cap - n) * sizeof(CollSong));
        }
        CollSong *s = &out[n++];
        const char *t;
        t = (const char *)sqlite3_column_text(stmt, 0);  if (t) s->title       = strdup(t);
        t = (const char *)sqlite3_column_text(stmt, 1);  if (t) s->artist      = strdup(t);
        t = (const char *)sqlite3_column_text(stmt, 2);  if (t) s->album       = strdup(t);
        t = (const char *)sqlite3_column_text(stmt, 3);  if (t) s->albumartist = strdup(t);
        t = (const char *)sqlite3_column_text(stmt, 4);  if (t) s->composer    = strdup(t);
        t = (const char *)sqlite3_column_text(stmt, 5);  if (t) s->genre       = strdup(t);
        s->track  = sqlite3_column_int(stmt, 6);
        s->disc   = sqlite3_column_int(stmt, 7);
        s->year   = sqlite3_column_int(stmt, 8);
        long long len_ns = sqlite3_column_int64(stmt, 9);
        s->length_ms = (int)(len_ns / 1000000LL);
        t = (const char *)sqlite3_column_text(stmt, 10);
        if (t) s->source_path = file_url_to_path(t);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    *n_out = (int)n;
    return out;
}

/* ====================================================================== */
/*  Matching: microsecond-exact join + size tiebreak                      */
/* ====================================================================== */

static void match_files_to_songs(IpodFile **files, int nf,
                                 CollSong *songs, int ns) {
    GHashTable *bucket = g_hash_table_new(g_int64_hash, g_int64_equal);
    int64_t *keys = calloc(ns, sizeof(int64_t));

    int songs_with_source = 0, songs_probed = 0;
    for (int i = 0; i < ns; i++) {
        if (songs[i].source_path) songs_with_source++;
        if (!(songs[i].probe_ok && songs[i].source_duration_us > 0)) continue;
        keys[i] = songs[i].source_duration_us;
        GList *gl = g_hash_table_lookup(bucket, &keys[i]);
        gl = g_list_prepend(gl, GINT_TO_POINTER(i));
        g_hash_table_insert(bucket, &keys[i], gl);
        songs_probed++;
    }
    fprintf(stderr,
        "Match table: %d/%d collection songs have a source path; "
        "%d were successfully probed for duration.\n",
        songs_with_source, ns, songs_probed);

    /* Bucket histogram. */
    int single_buckets = 0, multi_buckets = 0, max_bucket = 0;
    {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, bucket);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            int len = g_list_length((GList *)v);
            if (len == 1) single_buckets++;
            else { multi_buckets++; if (len > max_bucket) max_bucket = len; }
        }
    }
    fprintf(stderr,
        "Bucket distribution: %d unique durations, %d collide "
        "(max %d songs share a microsecond-exact duration).\n",
        single_buckets, multi_buckets, max_bucket);

    int matched_count = 0, ambiguous_refused = 0, no_candidate = 0;
    for (int i = 0; i < nf; i++) {
        IpodFile *f = files[i];
        if (f->duration_us <= 0) { no_candidate++; continue; }
        GList *gl = g_hash_table_lookup(bucket, &f->duration_us);
        if (!gl) { no_candidate++; continue; }

        int best_idx = -1, n_unmatched = 0;
        int64_t best_dist = INT64_MAX;
        for (GList *p = gl; p; p = p->next) {
            int idx = GPOINTER_TO_INT(p->data);
            if (songs[idx].matched) continue;
            n_unmatched++;
            int64_t dist = llabs((long long)songs[idx].source_size
                                - (long long)f->size);
            if (dist < best_dist) { best_dist = dist; best_idx = idx; }
        }
        if (best_idx < 0) { no_candidate++; continue; }

        if (n_unmatched > 1) {
            int64_t second_best = INT64_MAX;
            for (GList *p = gl; p; p = p->next) {
                int idx = GPOINTER_TO_INT(p->data);
                if (songs[idx].matched || idx == best_idx) continue;
                int64_t dist = llabs((long long)songs[idx].source_size
                                    - (long long)f->size);
                if (dist < second_best) second_best = dist;
            }
            int64_t denom = best_dist > 1 ? best_dist : 1;
            if (second_best != INT64_MAX &&
                (second_best - best_dist) * 100 < denom) {
                ambiguous_refused++;
                continue;
            }
        }
        songs[best_idx].matched = 1;
        f->matched_song_idx = best_idx;
        matched_count++;
    }

    fprintf(stderr,
        "Matching: matched=%d, no_candidate=%d, ambiguous_refused=%d "
        "(out of %d iPod files).\n",
        matched_count, no_candidate, ambiguous_refused, nf);

    free(keys);
    /* GLists leaked - process exits in a moment. */
    (void)bucket;
}

/* ====================================================================== */
/*  Cover hash (matches Strawberry's CoverUtils::Sha1CoverHash)           */
/* ====================================================================== */

static char *cover_hash_hex(const char *albumartist, const char *artist,
                            const char *album) {
    const char *eff = (albumartist && *albumartist) ? albumartist : artist;
    char *eff_lc   = ascii_lower(eff);
    char *album_lc = ascii_lower(album);
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(ck, (const guchar *)eff_lc,   (gssize)strlen(eff_lc));
    g_checksum_update(ck, (const guchar *)album_lc, (gssize)strlen(album_lc));
    char *out = g_strdup(g_checksum_get_string(ck));
    g_checksum_free(ck);
    free(eff_lc);
    free(album_lc);
    return out;
}

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <ipod-mount> <strawberry-db-path> "
            "[--dry-run] [--cover-dir <dir>]\n", argv[0]);
        return 2;
    }
    const char *mount   = argv[1];
    const char *db_path = argv[2];

    char default_cover_dir[2048];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(default_cover_dir, sizeof(default_cover_dir),
        "%s/Library/Application Support/Strawberry/Strawberry/devicealbumcovers",
        home);
    const char *cover_dir = default_cover_dir;
    int dry_run = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (strcmp(argv[i], "--cover-dir") == 0 && i + 1 < argc) {
            cover_dir = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    /* 1. Parse existing iTunesDB. */
    GError *err = NULL;
    Itdb_iTunesDB *db = itdb_parse(mount, &err);
    if (!db) {
        fprintf(stderr, "itdb_parse(%s) failed: %s\n",
                mount, err ? err->message : "unknown");
        return 1;
    }
    Itdb_Playlist *mpl = itdb_playlist_mpl(db);
    if (!mpl) {
        fprintf(stderr,
            "iTunesDB has no master playlist - cannot continue. "
            "Plug the iPod into Music.app once to initialize it.\n");
        return 1;
    }
    fprintf(stderr,
        "Parsed iTunesDB: %u existing tracks, MPL=\"%s\".\n",
        itdb_tracks_number(db), mpl->name ? mpl->name : "(null)");

    /* Idempotency: skip files already in the DB (matched by ipod_path). */
    GHashTable *existing = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *gl = db->tracks; gl; gl = gl->next) {
        Itdb_Track *t = (Itdb_Track *)gl->data;
        if (t->ipod_path) g_hash_table_insert(existing, g_strdup(t->ipod_path), t);
    }
    fprintf(stderr,
        "(%u tracks already in DB will be skipped to keep the tool "
        "idempotent; delete the iTunesDB first if you want a clean rebuild.)\n",
        (unsigned)g_hash_table_size(existing));

    /* 2. Scan iPod_Control/Music/F##/ */
    int nf = 0;
    IpodFile **files = collect_files(mount, &nf);
    if (!files) return 1;
    fprintf(stderr, "Found %d audio files on the iPod.\n", nf);

    /* 3. Probe each iPod file. */
    fprintf(stderr,
        "Probing iPod files for duration_us (codec + sample count)...\n");
    int probe_failed = 0;
    for (int i = 0; i < nf; i++) {
        IpodFile *f = files[i];
        if (!probe_audio(f->path, &f->duration_us, &f->samplerate,
                         &f->size, &f->bitrate_kbps, &f->codec)) {
            probe_failed++;
        }
        if ((i + 1) % 250 == 0) {
            fprintf(stderr, "  probed %d/%d iPod files\n", i + 1, nf);
        }
    }
    fprintf(stderr,
        "iPod probe done; %d files unreadable (will fall back to filename "
        "as title).\n", probe_failed);

    /* 4. Load Strawberry collection. */
    int ns = 0;
    CollSong *songs = load_collection(db_path, &ns);
    if (!songs) return 1;
    fprintf(stderr, "Loaded %d collection songs from %s\n", ns, db_path);

    /* 5. Probe each source file. */
    fprintf(stderr,
        "Probing source files for duration_us. This is the slow part - "
        "it may take 5-15 minutes for a large library...\n");
    int src_missing = 0, src_probe_ok = 0, src_probe_failed = 0;
    for (int i = 0; i < ns; i++) {
        CollSong *s = &songs[i];
        if (!s->source_path) { src_missing++; continue; }
        struct stat st;
        if (stat(s->source_path, &st) != 0) { src_missing++; continue; }
        s->source_size = (int64_t)st.st_size;
        if (probe_audio(s->source_path, &s->source_duration_us,
                        &s->source_samplerate, NULL, NULL, &s->source_codec)) {
            s->probe_ok = 1;
            src_probe_ok++;
        } else {
            src_probe_failed++;
        }
        if ((i + 1) % 250 == 0) {
            fprintf(stderr, "  probed %d/%d source files\n", i + 1, ns);
        }
    }
    fprintf(stderr,
        "Source probe done: probe_ok=%d, missing=%d, probe_failed=%d "
        "(out of %d collection songs).\n",
        src_probe_ok, src_missing, src_probe_failed, ns);

    if (src_probe_ok < nf / 2) {
        fprintf(stderr,
            "ERROR: Only %d of %d sources are probeable. The match step "
            "would produce mostly garbage. Likely cause: the volume "
            "holding your music collection (where the original "
            "FLAC/MP3 files live) is not mounted. Mount it and re-run.\n",
            src_probe_ok, nf);
        return 1;
    }

    /* 6. Match. */
    match_files_to_songs(files, nf, songs, ns);

    /* 7. Build the iTunesDB. */
    int added = 0, attached_cover = 0, missing_cover = 0;
    int used_filename_fallback = 0;
    for (int i = 0; i < nf; i++) {
        IpodFile *f = files[i];
        if (g_hash_table_contains(existing, f->ipod_path)) continue;

        Itdb_Track *t = itdb_track_new();
        const char *title = NULL, *artist = NULL, *album = NULL;
        const char *albumartist = "", *composer = "", *genre = "";
        int track_nr = 0, disc_nr = 0, year = 0;

        if (f->matched_song_idx >= 0) {
            CollSong *s = &songs[f->matched_song_idx];
            title       = s->title;
            artist      = s->artist;
            album       = s->album;
            albumartist = s->albumartist ? s->albumartist : "";
            composer    = s->composer    ? s->composer    : "";
            genre       = s->genre       ? s->genre       : "";
            track_nr    = s->track > 0 ? s->track : 0;
            disc_nr     = s->disc  > 0 ? s->disc  : 0;
            year        = s->year  > 0 ? s->year  : 0;
        } else {
            used_filename_fallback++;
            const char *bn = strrchr(f->path, '/');
            const char *base = bn ? bn + 1 : f->path;
            char fallback_title[512];
            snprintf(fallback_title, sizeof(fallback_title),
                     "Unknown: %s", base);
            title  = fallback_title;
            artist = "Unknown Artist";
            album  = "Unknown Album (rescue)";
        }

        t->title       = g_strdup(title);
        t->artist      = g_strdup(artist);
        t->album       = g_strdup(album);
        t->albumartist = g_strdup(albumartist);
        t->composer    = g_strdup(composer);
        t->genre       = g_strdup(genre);
        t->track_nr    = track_nr;
        t->cd_nr       = disc_nr;
        t->year        = year;
        t->bitrate     = f->bitrate_kbps;
        t->samplerate  = (guint16)f->samplerate;
        t->tracklen    = (gint32)(f->duration_us / 1000LL);  /* ms */
        t->size        = (gint32)f->size;
        t->ipod_path   = g_strdup(f->ipod_path);
        t->transferred = TRUE;
        t->mediatype   = ITDB_MEDIATYPE_AUDIO;
        t->compilation = 0;

        /* Codec-correct filetype string + type1/type2 magic. */
        const char *ftype = NULL;
        int type1 = 0, type2 = 0;
        itunesdb_filetype_for(f->codec, f->path, &ftype, &type1, &type2);
        t->filetype = g_strdup(ftype);
        t->type1    = type1;
        t->type2    = type2;

        /* Cover art: SHA1(lower(albumartist) + lower(album)).jpg in the
         * device-cover-cache. Only for real matches. */
        if (f->matched_song_idx >= 0 && album && *album) {
            char *hex = cover_hash_hex(albumartist, artist, album);
            char cover_path[4096];
            snprintf(cover_path, sizeof(cover_path), "%s/%s.jpg",
                     cover_dir, hex);
            g_free(hex);
            if (g_file_test(cover_path, G_FILE_TEST_EXISTS) &&
                itdb_track_set_thumbnails(t, cover_path)) {
                t->has_artwork = 1;
                attached_cover++;
            } else {
                missing_cover++;
            }
        }

        itdb_track_add(db, t, -1);
        itdb_playlist_add_track(mpl, t, -1);
        added++;
        if (added % 500 == 0) {
            fprintf(stderr, "  built %d / %d tracks\n", added, nf);
        }
    }

    fprintf(stderr,
        "\nBuild complete:\n"
        "  added=%d (of %d files)\n"
        "  used_filename_fallback=%d (no reliable source match)\n"
        "  attached_cover=%d, missing_cover=%d\n"
        "  DB now has %u tracks, MPL %u members.\n",
        added, nf, used_filename_fallback,
        attached_cover, missing_cover,
        itdb_tracks_number(db), g_list_length(mpl->members));

    if (used_filename_fallback > 0) {
        fprintf(stderr,
            "\nNOTE: %d files were labelled \"Unknown:...\" because we couldn't\n"
            "match them to a Strawberry collection song. They will play but\n"
            "won't have proper title/artist/album. To fix them, locate the\n"
            "originals manually and re-sync the affected albums from Strawberry.\n",
            used_filename_fallback);
    }

    if (dry_run) {
        fprintf(stderr,
            "\nDry run: NOT writing. Re-run without --dry-run to commit.\n");
        return 0;
    }

    GError *werr = NULL;
    if (!itdb_write(db, &werr)) {
        fprintf(stderr, "itdb_write failed: %s\n",
                werr ? werr->message : "unknown");
        return 1;
    }
    fprintf(stderr,
        "\nitdb_write succeeded. iPod should now show %u tracks with %d "
        "album covers.\nEject and reconnect the device to verify.\n",
        itdb_tracks_number(db), attached_cover);

    g_hash_table_destroy(existing);
    return 0;
}