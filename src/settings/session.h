#pragma once

#include <glib.h>

gboolean session_settings_load(gboolean *restore_tabs, GError **error);
gboolean session_settings_save(gboolean restore_tabs, GError **error);
