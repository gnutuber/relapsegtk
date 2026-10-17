#pragma once

#include <glib.h>

GKeyFile *store_load(gboolean recover_invalid, GError **error);
gboolean store_save(GKeyFile *file, GError **error);
