#include "style.h"

#include <gtk/gtk.h>

static void parsing_error(GtkCssProvider *provider, GtkCssSection *section,
                          const GError *error, gpointer user_data)
{
    (void)provider;
    (void)section;
    (void)user_data;
    g_error("Invalid bundled stylesheet: %s", error->message);
}

void style_install(GdkDisplay *display)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    g_signal_connect(provider, "parsing-error", G_CALLBACK(parsing_error), NULL);
    gtk_css_provider_load_from_resource(provider, "/org/relapse/RelapseGtk/style.css");

    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    g_object_unref(provider);
}

void style_set_editor_font_size(GdkDisplay *display, int size)
{
    GtkCssProvider *provider = g_object_get_data(G_OBJECT(display), "editor-font-provider");
    if (!provider) {
        provider = gtk_css_provider_new();
        g_signal_connect(provider, "parsing-error", G_CALLBACK(parsing_error), NULL);
        g_object_set_data_full(G_OBJECT(display), "editor-font-provider", provider, g_object_unref);
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_USER + 2);
    }
    g_autofree char *css = g_strdup_printf(
        ".relapse-ui textview.code-editor, .relapse-ui textview.code-editor * { font-size: %dpx; }", size);
    gtk_css_provider_load_from_string(provider, css);
}
