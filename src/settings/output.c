#include "output.h"
#include "store.h"

gboolean output_settings_load(gboolean *enabled, GError **error)
{
    *enabled = FALSE;
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file) return FALSE;
    g_autoptr(GError) failure = NULL;
    gboolean value = g_key_file_get_boolean(file, "Output", "redirect", &failure);
    if (!failure) *enabled = value;
    return TRUE;
}

gboolean output_settings_save(gboolean enabled, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file) return FALSE;
    g_key_file_set_boolean(file, "Output", "redirect", enabled);
    return store_save(file, error);
}
