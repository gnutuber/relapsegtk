#include "opener.h"

#include <string.h>
#include "store.h"

static const char *mode_keys[] = {"editor", "folder", "system", "command", "nothing"};

OpenerSettings opener_settings_defaults(void)
{
    return (OpenerSettings){.mode = OPENER_NOTHING, .command = NULL};
}

void opener_settings_clear(OpenerSettings *settings)
{
    g_clear_pointer(&settings->command, g_free);
    settings->mode = OPENER_NOTHING;
}

static OpenerMode mode_from_key(const char *key)
{
    for (guint i = 0; i < G_N_ELEMENTS(mode_keys); ++i)
        if (g_strcmp0(key, mode_keys[i]) == 0)
            return (OpenerMode)i;
    return OPENER_NOTHING;
}

gboolean opener_settings_load(OpenerSettings *settings, GError **error)
{
    *settings = opener_settings_defaults();
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file)
        return FALSE;
    g_autofree char *key = g_key_file_get_string(file, "Scripts", "open-with", NULL);
    settings->mode = mode_from_key(key);
    settings->command = g_key_file_get_string(file, "Scripts", "open-command", NULL);
    return TRUE;
}

gboolean opener_settings_save(const OpenerSettings *settings, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file)
        return FALSE;
    guint mode = settings->mode < G_N_ELEMENTS(mode_keys) ? settings->mode : OPENER_NOTHING;
    g_key_file_set_string(file, "Scripts", "open-with", mode_keys[mode]);
    g_key_file_set_string(file, "Scripts", "open-command", settings->command ? settings->command : "");
    return store_save(file, error);
}

static gboolean launch_default(GFile *file, GError **error)
{
    g_autofree char *uri = g_file_get_uri(file);
    return g_app_info_launch_default_for_uri(uri, NULL, error);
}

static gboolean launch_command(const char *command, const char *path, GError **error)
{
    g_autofree char *quoted = g_shell_quote(path);
    g_auto(GStrv) parts = g_strsplit(command, "%s", -1);
    g_autofree char *expanded = parts[1]
        ? g_strjoinv(quoted, parts)
        : g_strconcat(command, " ", quoted, NULL);
    int argc = 0;
    g_auto(GStrv) argv = NULL;
    if (!g_shell_parse_argv(expanded, &argc, &argv, error))
        return FALSE;
    if (argc == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The external editor command is empty");
        return FALSE;
    }
    g_autoptr(GSubprocess) process = g_subprocess_newv((const char *const *)argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
    return process != NULL;
}

gboolean opener_open(GFile *file, const OpenerSettings *settings, GError **error)
{
    if (!file) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No script selected");
        return FALSE;
    }
    switch (settings->mode) {
        case OPENER_FOLDER: {
            g_autofree char *path = g_file_get_path(file);
            if (!path) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                    "Only local scripts can be shown in a file manager");
                return FALSE;
            }
            g_autofree char *directory = g_path_get_dirname(path);
            g_autoptr(GFile) folder = g_file_new_for_path(directory);
            return launch_default(folder, error);
        }
        case OPENER_SYSTEM:
            return launch_default(file, error);
        case OPENER_COMMAND: {
            g_autofree char *path = g_file_get_path(file);
            if (!path) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                    "Only local scripts can be opened externally");
                return FALSE;
            }
            if (!settings->command || !*settings->command) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    "Set an external editor command in Settings");
                return FALSE;
            }
            return launch_command(settings->command, path, error);
        }
        case OPENER_NOTHING:
            return TRUE;
        case OPENER_EDITOR:
        default:
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Scripts open in the editor");
            return FALSE;
    }
}
