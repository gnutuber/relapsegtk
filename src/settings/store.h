#pragma once

#include <glib.h>

char *store_settings_path(void);
GKeyFile *store_load(gboolean recover_invalid, GError **error);
gboolean store_save(GKeyFile *file, GError **error);
