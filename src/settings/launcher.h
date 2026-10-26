#pragma once

#include <glib.h>

gboolean launcher_settings_load(char **path, GError **error);
gboolean launcher_settings_save(const char *path, GError **error);
char *launcher_find(void);
