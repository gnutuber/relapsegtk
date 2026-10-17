#include "editor.h"

#include "store.h"

EditorSettings editor_settings_defaults(void)
{
    return (EditorSettings){.font_size = 13, .word_wrap = FALSE, .indent_width = 4};
}

static gboolean valid_font_size(int size)
{
    return size >= EDITOR_FONT_SIZE_MIN && size <= EDITOR_FONT_SIZE_MAX;
}

static gboolean valid_indent_width(int width)
{
    return width == 2 || width == 4 || width == 8;
}

gboolean editor_settings_load(EditorSettings *settings, GError **error)
{
    *settings = editor_settings_defaults();
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file)
        return FALSE;
    int size = g_key_file_get_integer(file, "Editor", "font-size", NULL);
    int indent = g_key_file_get_integer(file, "Editor", "indent-width", NULL);
    if (valid_font_size(size))
        settings->font_size = size;
    if (valid_indent_width(indent))
        settings->indent_width = indent;
    settings->word_wrap = g_key_file_get_boolean(file, "Editor", "word-wrap", NULL);
    return TRUE;
}

gboolean editor_settings_save(const EditorSettings *settings, GError **error)
{
    if (!valid_font_size(settings->font_size) || !valid_indent_width(settings->indent_width)) {
        g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                            "Invalid editor settings");
        return FALSE;
    }
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file)
        return FALSE;
    g_key_file_set_integer(file, "Editor", "font-size", settings->font_size);
    g_key_file_set_boolean(file, "Editor", "word-wrap", settings->word_wrap);
    g_key_file_set_integer(file, "Editor", "indent-width", settings->indent_width);
    return store_save(file, error);
}
