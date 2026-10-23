#include "launcher.h"
#include "store.h"

gboolean launcher_settings_load(char **path, GError **error)
{
    *path = NULL;
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file) return FALSE;
    *path = g_key_file_get_string(file, "Launcher", "path", NULL);
    return TRUE;
}

gboolean launcher_settings_save(const char *path, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file) return FALSE;
    g_key_file_set_string(file, "Launcher", "path", path ? path : "");
    return store_save(file, error);
}
