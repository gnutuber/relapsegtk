#include "editor.h"

#include <gtksourceview/gtksource.h>

void editor_apply_settings(GtkWidget *editor, const EditorSettings *settings)
{
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(editor),
        settings->word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(editor), settings->indent_width);
    gtk_source_view_set_indent_width(GTK_SOURCE_VIEW(editor), settings->indent_width);
}

static gboolean editor_materialize_scheme(const char *dir)
{
    g_autoptr(GBytes) bytes = g_resources_lookup_data("/org/relapse/RelapseGtk/styles/relapse.xml",
        G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (!bytes)
        return FALSE;
    gsize size = 0;
    const char *data = g_bytes_get_data(bytes, &size);
    g_autofree char *path = g_build_filename(dir, "relapse.xml", NULL);
    return g_file_set_contents(path, data, (gssize)size, NULL);
}

GtkWidget *editor_new(const EditorSettings *settings)
{
    gtk_source_init();
    GtkSourceBuffer *buffer = gtk_source_buffer_new(NULL);

    GtkSourceLanguageManager *languages = gtk_source_language_manager_new();
    GtkSourceLanguage *language = gtk_source_language_manager_get_language(languages, "lua");
    if (!language) {
        const char *language_paths[] = {"resource:///org/gnome/gtksourceview/language-specs", NULL};
        gtk_source_language_manager_set_search_path(languages, language_paths);
        language = gtk_source_language_manager_get_language(languages, "lua");
    }
    if (!language)
        g_error("GtkSourceView's bundled Lua language is unavailable");
    gtk_source_buffer_set_language(buffer, language);
    g_object_set_data_full(G_OBJECT(buffer), "languages", languages, g_object_unref);

    GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_new();
#if GTK_SOURCE_CHECK_VERSION(5, 16, 0)
    const char *style_paths[] = {"resource:///org/relapse/RelapseGtk/styles", NULL};
    gtk_source_style_scheme_manager_set_search_path(schemes, style_paths);
#endif
    GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(schemes, "relapse");
    if (!scheme) {
        g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "relapsegtk", NULL);
        if (g_mkdir_with_parents(dir, 0700) == 0 && editor_materialize_scheme(dir)) {
            gtk_source_style_scheme_manager_append_search_path(schemes, dir);
            scheme = gtk_source_style_scheme_manager_get_scheme(schemes, "relapse");
        }
    }
    if (!scheme)
        g_error("The bundled editor color scheme is unavailable");
    gtk_source_buffer_set_style_scheme(buffer, scheme);
    g_object_set_data_full(G_OBJECT(buffer), "styles", schemes, g_object_unref);
    gtk_source_buffer_set_highlight_syntax(buffer, TRUE);
    gtk_source_buffer_set_highlight_matching_brackets(buffer, TRUE);
    gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(buffer), TRUE);

    GtkWidget *editor = gtk_source_view_new_with_buffer(buffer);
    gtk_widget_add_css_class(editor, "code-editor");
    g_object_unref(buffer);
    GtkSourceView *view = GTK_SOURCE_VIEW(editor);
    gtk_source_view_set_show_line_numbers(view, TRUE);
    gtk_source_view_set_highlight_current_line(view, TRUE);
    gtk_source_view_set_auto_indent(view, TRUE);
    editor_apply_settings(editor, settings);
    gtk_source_view_set_insert_spaces_instead_of_tabs(view, TRUE);
    gtk_source_view_set_indent_on_tab(view, TRUE);
    gtk_source_view_set_smart_backspace(view, TRUE);
    gtk_source_view_set_smart_home_end(view, GTK_SOURCE_SMART_HOME_END_BEFORE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 10);
    return editor;
}
