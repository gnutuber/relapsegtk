#include "log.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

char *log_directory(void)
{
    return g_build_filename(g_get_user_state_dir(), "relapsegtk", NULL);
}

char *log_client_name(const char *client)
{
    g_autofree char *basename = client && *client ? g_path_get_basename(client) : NULL;
    if (!basename || !*basename || strcmp(basename, ".") == 0 || strcmp(basename, "/") == 0)
        return g_strdup("app");
    GString *name = g_string_new(NULL);
    for (const char *c = basename; *c; ++c)
        g_string_append_c(name, g_ascii_isalnum(*c) || *c == '.' || *c == '-' || *c == '_'
            ? *c : '_');
    if (name->len == 0)
        g_string_append(name, "app");
    return g_string_free(name, FALSE);
}

char *log_path_for(const char *client, gboolean single)
{
    g_autofree char *directory = log_directory();
    if (single)
        return g_build_filename(directory, "relapsegtk.rlog", NULL);
    g_autofree char *name = log_client_name(client);
    g_autofree char *clients = g_build_filename(directory, "clients", NULL);
    g_autofree char *filename = g_strconcat(name, ".rlog", NULL);
    return g_build_filename(clients, filename, NULL);
}

gboolean log_append(const char *client, gboolean single, const char *level, const char *text,
                    GError **error)
{
    g_autofree char *path = log_path_for(client, single);
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not create the log folder: %s", g_strerror(saved_errno));
        return FALSE;
    }

    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &parts);

    g_autofree char *name = log_client_name(client);
    g_autofree char *clean = g_strdup(text ? text : "");
    for (char *c = clean; *c; ++c)
        if (*c == '\r')
            *c = ' ';
    g_autofree char *line = g_strdup_printf("%s [%s] %s: %s\n", stamp,
                                            level && *level ? level : "print", name, clean);

    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileOutputStream) stream = g_file_append_to(file, G_FILE_CREATE_PRIVATE, NULL, error);
    if (!stream)
        return FALSE;
    if (!g_output_stream_write_all(G_OUTPUT_STREAM(stream), line, strlen(line), NULL, NULL, error))
        return FALSE;
    return g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, error);
}
