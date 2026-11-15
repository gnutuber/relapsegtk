#pragma once

#include <glib.h>

gboolean raknet_settings_load(gboolean *enabled);
gboolean raknet_settings_save(gboolean enabled, GError **error);
