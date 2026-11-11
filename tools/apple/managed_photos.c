/* Build a staged Apple Photo Database from selected image sources. */
#define _GNU_SOURCE
#include <gpod/itdb.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "managed_io.h"

int main(int argc, char **argv) {
    managed_arguments(&argc, &argv);
    if (argc != 5) {
        fprintf(stderr, "usage: managed_photos staging-mount image-list model guid\n");
        return 2;
    }
    Itdb_PhotoDB *db = itdb_photodb_create(argv[1]);
    if (!db) {
        fprintf(stderr, "Could not initialize the Apple Photo Database\n");
        return 1;
    }
    itdb_device_set_sysinfo(db->device, "ModelNumStr", argv[3]);
    itdb_device_set_sysinfo(db->device, "FirewireGuid", argv[4]);
    FILE *input = g_fopen(argv[2], "rb");
    if (!input) {
        perror(argv[2]);
        return 2;
    }
    GString *buffer = g_string_new(NULL);
    unsigned count = 0;
    while (managed_read_line(input, buffer)) {
        char *line = buffer->str;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        GError *error = NULL;
        Itdb_Artwork *photo = itdb_photodb_add_photo(db, line, -1, 0, &error);
        if (!photo) {
            fprintf(stderr, "Could not prepare photo %u: %s\n", count + 1,
                    error ? error->message : "unknown");
            if (error) g_error_free(error);
            return 1;
        }
        count++;
    }
    g_string_free(buffer, TRUE);
    fclose(input);
    GError *error = NULL;
    gboolean ok = itdb_photodb_write(db, &error);
    if (!ok) fprintf(stderr, "Photo Database write failed: %s\n", error ? error->message : "unknown");
    if (error) g_error_free(error);
    itdb_photodb_free(db);
    fprintf(stderr, "prepared %u photos\n", count);
    return ok ? 0 : 1;
}
