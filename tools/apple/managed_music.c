/* Update Linnet-managed Apple music entries in a staged libgpod database. */
#define _GNU_SOURCE
#include <gpod/itdb.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "managed_io.h"

static const char *managed_prefix = ":iPod_Control:Music:..:..:Music:";
static const char *managed_video_prefix = ":iPod_Control:Music:..:..:Videos:";

static gchar *decode(const char *encoded) {
    gsize size = 0;
    guchar *raw = g_base64_decode(encoded, &size);
    gchar *value = g_strndup((const char *)raw, size);
    g_free(raw);
    if (!g_utf8_validate(value, -1, NULL)) {
        g_free(value);
        return NULL;
    }
    return value;
}

static unsigned long long number(const char *value) {
    char *end = NULL;
    unsigned long long result = strtoull(value, &end, 10);
    if (!value[0] || !end || *end) {
        fprintf(stderr, "Invalid manifest number: %s\n", value);
        exit(2);
    }
    return result;
}

int main(int argc, char **argv) {
    managed_arguments(&argc, &argv);
    if (argc != 6) {
        fprintf(stderr, "usage: managed_music staging-mount manifest.tsv model guid owned-paths.txt\n");
        return 2;
    }
    /* A shared path alone does not establish ownership. Use the saved list. */
    GHashTable *owned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    {
        FILE *ownership = g_fopen(argv[5], "rb");
        if (!ownership) {
            perror(argv[5]);
            return 2;
        }
        GString *line = g_string_new(NULL);
        while (managed_read_line(ownership, line)) {
            if (!line->len) continue;
            if (line->len > 55 || strpbrk(line->str, "\r\t\\") ||
                !(g_str_has_prefix(line->str, managed_prefix) ||
                  g_str_has_prefix(line->str, managed_video_prefix))) {
                fprintf(stderr, "Invalid saved Apple media ownership path\n");
                return 2;
            }
            g_hash_table_add(owned, g_strdup(line->str));
        }
        g_string_free(line, TRUE);
        fclose(ownership);
    }
    gchar *database = g_build_filename(argv[1], "iPod_Control", "iTunes", "iTunesDB", NULL);
    GError *error = NULL;
    gboolean existing_database = g_file_test(database, G_FILE_TEST_EXISTS);
    Itdb_iTunesDB *db = existing_database ? itdb_parse(argv[1], &error) : itdb_new();
    g_free(database);
    if (!db) {
        fprintf(stderr, "Could not read the Apple database: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }
    if (!existing_database) itdb_set_mountpoint(db, argv[1]);
    itdb_device_set_sysinfo(db->device, "ModelNumStr", argv[3]);
    itdb_device_set_sysinfo(db->device, "FirewireGuid", argv[4]);
    Itdb_Playlist *master = db->playlists ? itdb_playlist_mpl(db) : NULL;
    if (!master) {
        master = itdb_playlist_new("iPod", FALSE);
        itdb_playlist_set_mpl(master);
        itdb_playlist_add(db, master, -1);
    }
    GHashTable *existing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *retained = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *artwork = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    for (GList *item = db->tracks; item; item = item->next) {
        Itdb_Track *track = item->data;
        if (track->artwork_count && !itdb_track_has_thumbnails(track)) {
            fprintf(stderr, "Existing Apple album artwork could not be read; update stopped\n");
            return 1;
        }
        if (itdb_track_has_thumbnails(track)) {
            guint64 *id = g_new(guint64, 1);
            *id = track->dbid;
            g_hash_table_add(artwork, id);
        }
        if (track->ipod_path && (g_str_has_prefix(track->ipod_path, managed_prefix) ||
                                 g_str_has_prefix(track->ipod_path, managed_video_prefix)))
            g_hash_table_insert(existing, g_strdup(track->ipod_path), track);
    }
    FILE *input = g_fopen(argv[2], "rb");
    if (!input) {
        perror(argv[2]);
        itdb_free(db);
        return 2;
    }
    GString *buffer = g_string_new(NULL);
    unsigned count = 0;
    while (managed_read_line(input, buffer)) {
        char *line = buffer->str;
        line[strcspn(line, "\r\n")] = 0;
        gchar **fields = g_strsplit(line, "\t", 0);
        unsigned columns = g_strv_length(fields);
        gboolean movie = columns == 13 && !strcmp(fields[12], "movie");
        if (!((columns == 12 && g_str_has_prefix(fields[0], managed_prefix)) ||
              (movie && g_str_has_prefix(fields[0], managed_video_prefix))) ||
            strlen(fields[0]) > 55) {
            fprintf(stderr, "Invalid manifest line %u\n", count + 1);
            g_strfreev(fields);
            return 2;
        }
        unsigned long long size = number(fields[1]);
        unsigned long long duration = number(fields[2]);
        unsigned long long bitrate = number(fields[3]);
        unsigned long long sample_rate = number(fields[4]);
        unsigned long long track_number = number(fields[5]);
        unsigned long long disc_number = number(fields[6]);
        if (size == 0 || size > G_MAXUINT32 || duration == 0 || duration > G_MAXINT32 ||
            bitrate > G_MAXINT32 || sample_rate == 0 || sample_rate > G_MAXUINT16 ||
            track_number > G_MAXINT32 || disc_number > G_MAXINT32) {
            fprintf(stderr, "Out-of-range metadata at line %u\n", count + 1);
            g_strfreev(fields);
            return 2;
        }
        Itdb_Track *track = g_hash_table_lookup(existing, fields[0]);
        gboolean added = track == NULL;
        if (added) {
            track = itdb_track_new();
            track->ipod_path = g_strdup(fields[0]);
        } else if (g_hash_table_contains(retained, track)) {
            fprintf(stderr, "Duplicate Apple location at line %u\n", count + 1);
            g_strfreev(fields);
            return 2;
        }
        g_hash_table_add(retained, track);
        track->size = (guint32)size;
        track->tracklen = (gint32)duration;
        track->bitrate = (gint32)bitrate;
        track->samplerate = (guint16)sample_rate;
        track->samplerate2 = (gdouble)track->samplerate;
        track->track_nr = (gint32)track_number;
        track->cd_nr = (gint32)disc_number;
        g_free(track->title); track->title = decode(fields[7]);
        g_free(track->artist); track->artist = decode(fields[8]);
        g_free(track->album); track->album = decode(fields[9]);
        g_free(track->albumartist); track->albumartist = decode(fields[10]);
        g_free(track->genre); track->genre = decode(fields[11]);
        if (!track->title || !track->artist || !track->album ||
            !track->albumartist || !track->genre) {
            fprintf(stderr, "Invalid text metadata at line %u\n", count + 1);
            g_strfreev(fields);
            return 2;
        }
        g_free(track->filetype);
        track->filetype = g_strdup(movie ? "MPEG-4 video file" : "MPEG audio file");
        track->filetype_marker = movie ? 0x4d345620 : 0x4d503320;
        track->type2 = 1;
        track->mediatype = movie ? ITDB_MEDIATYPE_MOVIE : ITDB_MEDIATYPE_AUDIO;
        track->movie_flag = movie ? 1 : 0;
        track->transferred = TRUE;
        if (added) {
            itdb_track_add(db, track, -1);
            itdb_playlist_add_track(master, track, -1);
        }
        g_strfreev(fields);
        count++;
    }
    g_string_free(buffer, TRUE);
    fclose(input);
    /* Only recorded Linnet-owned entries leave Apple OS when unselected.
       All other tracks and their metadata and playlists remain intact. */
    for (GList *item = db->tracks; item;) {
        Itdb_Track *track = item->data;
        item = item->next;
        if (track->ipod_path && g_hash_table_contains(owned, track->ipod_path) &&
            !g_hash_table_contains(retained, track)) {
            for (GList *playlist = db->playlists; playlist; playlist = playlist->next) {
                Itdb_Playlist *current = playlist->data;
                while (g_list_find(current->members, track))
                    itdb_playlist_remove_track(current, track);
            }
            g_hash_table_remove(artwork, &track->dbid);
            itdb_track_remove(track);
        }
    }
    g_hash_table_destroy(existing);
    g_hash_table_destroy(retained);
    g_hash_table_destroy(owned);
    error = NULL;
    gboolean ok = itdb_write(db, &error);
    if (!ok) fprintf(stderr, "Apple database write failed: %s\n", error ? error->message : "unknown");
    if (error) g_error_free(error);
    itdb_free(db);
    /* libgpod can report a successful DB write after an artwork write failure.
       Reparse and require retained artwork associations before publication. */
    if (ok && g_hash_table_size(artwork)) {
        error = NULL;
        Itdb_iTunesDB *verified = itdb_parse(argv[1], &error);
        if (verified) {
            for (GList *item = verified->tracks; item; item = item->next) {
                Itdb_Track *track = item->data;
                if (itdb_track_has_thumbnails(track))
                    g_hash_table_remove(artwork, &track->dbid);
            }
            itdb_free(verified);
        }
        if (error) g_error_free(error);
        if (g_hash_table_size(artwork)) {
            fprintf(stderr, "Apple database writer did not preserve existing album artwork\n");
            ok = FALSE;
        }
    }
    g_hash_table_destroy(artwork);
    fprintf(stderr, "updated %u managed media entries\n", count);
    return ok ? 0 : 1;
}
