#pragma once
#include <glib.h>

gboolean output_settings_load(gboolean *enabled, GError **error);
gboolean output_settings_save(gboolean enabled, GError **error);
