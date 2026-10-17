#include "store.h"

#include <errno.h>
#include <glib/gstdio.h>

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "relapsegtk", "settings.ini", NULL);
}

GKeyFile *store_load(gboolean recover_invalid, GError **error)
{
    g_autofree char *path = settings_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_autoptr(GError) failure = NULL;
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, &failure)) {
        if (g_error_matches(failure, G_FILE_ERROR, G_FILE_ERROR_NOENT) ||
            (recover_invalid && failure->domain == G_KEY_FILE_ERROR))
            return g_key_file_new();
        g_propagate_error(error, g_steal_pointer(&failure));
        return NULL;
    }
    return g_steal_pointer(&file);
}

gboolean store_save(GKeyFile *file, GError **error)
{
    g_autofree char *path = settings_path();
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not create settings directory: %s", g_strerror(saved_errno));
        return FALSE;
    }
    gsize size;
    g_autofree char *contents = g_key_file_to_data(file, &size, NULL);
    return g_file_set_contents_full(path, contents, size,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}
