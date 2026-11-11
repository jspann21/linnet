/* UTF-8 paths and bounded manifest input on Windows and Unix. */
#ifndef LINNET_MANAGED_IO_H
#define LINNET_MANAGED_IO_H
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

static gboolean managed_read_line(FILE *input, GString *line) {
    g_string_truncate(line, 0);
    int ch;
    while ((ch = fgetc(input)) != EOF) {
        if (ch == '\n') return TRUE;
        if (ch == 0 || line->len >= 1024 * 1024) {
            fprintf(stderr, "Invalid or oversized manifest line\n");
            exit(2);
        }
        g_string_append_c(line, ch);
    }
    if (ferror(input)) {
        fprintf(stderr, "Could not read the manifest\n");
        exit(2);
    }
    return line->len != 0;
}

static void managed_arguments(int *argc, char ***argv) {
#ifdef G_OS_WIN32
    *argv = g_win32_get_command_line();
    *argc = g_strv_length(*argv);
#else
    (void)argc;
    (void)argv;
#endif
}
#endif
