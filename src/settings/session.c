#include "session.h"
#include "store.h"

gboolean session_settings_load(gboolean *restore_tabs, GError **error)
{
    *restore_tabs = TRUE;
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file)
        return FALSE;
    g_autoptr(GError) failure = NULL;
    gboolean value = g_key_file_get_boolean(file, "Session", "restore-tabs", &failure);
    if (!failure)
        *restore_tabs = value;
    return TRUE;
}

gboolean session_settings_save(gboolean restore_tabs, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file)
        return FALSE;
    g_key_file_set_boolean(file, "Session", "restore-tabs", restore_tabs);
    return store_save(file, error);
}
