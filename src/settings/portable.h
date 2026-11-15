#pragma once

#include <glib.h>

gboolean portable_export(const char *path, GError **error);
gboolean portable_import(const char *path, GError **error);
