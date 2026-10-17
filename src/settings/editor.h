#pragma once

#include <glib.h>

enum { EDITOR_FONT_SIZE_MIN = 8, EDITOR_FONT_SIZE_MAX = 32 };

typedef struct {
    int font_size;
    gboolean word_wrap;
    int indent_width;
} EditorSettings;

EditorSettings editor_settings_defaults(void);
gboolean editor_settings_load(EditorSettings *settings, GError **error);
gboolean editor_settings_save(const EditorSettings *settings, GError **error);
