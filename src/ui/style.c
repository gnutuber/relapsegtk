#include "style.h"

#include <gtk/gtk.h>

static gboolean animations_disabled;

gboolean style_animations_disabled(void)
{
    return animations_disabled;
}

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

static void append_stop(GString *css, const char *color, double alpha, const char *position)
{
    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, color)) {
        g_string_append_printf(css, "%s %s", color, position);
        return;
    }
    rgba.alpha = CLAMP(rgba.alpha * alpha, 0.0, 1.0);
    g_autofree char *text = gdk_rgba_to_string(&rgba);
    g_string_append_printf(css, "%s %s", text, position);
}

static gboolean append_image_url(GString *css, const char *path)
{
    if (!path || !*path)
        return FALSE;
    g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri)
        return FALSE;
    g_autofree char *escaped = g_strescape(uri, NULL);
    g_string_append_printf(css, "url(\"%s\")", escaped);
    return TRUE;
}

static void append_animation_override(GString *css, const AppearanceSettings *settings)
{
    if (!settings->animations_disabled)
        return;
    g_string_append(css,
        "* { animation-name: none; transition-duration: 0s; transition-delay: 0s; }\n"
        ".relapse-ui image.app-logo { animation: none; }\n"
        ".relapse-ui button.settings-category,\n"
        ".relapse-ui .settings-group switch,\n"
        ".relapse-ui .settings-group switch slider,\n"
        ".relapse-ui switch,\n"
        ".relapse-ui switch slider,\n"
        ".relapse-ui .autoexec-frame { transition-duration: 0s; transition-delay: 0s; }\n");
}

static char *appearance_build_css(const AppearanceSettings *settings)
{
    GString *css = g_string_new("/* Generated from the appearance settings. */\n");
    g_string_append_printf(css,
        ".relapse-ui, .relapse-ui * { --window-radius: %dpx; }\n",
        CLAMP(settings->radius, 0, APPEARANCE_RADIUS_MAX));

    double strength = CLAMP(settings->opacity, APPEARANCE_OPACITY_MIN, 1.0);
    double background_alpha = CLAMP(1.0 - 0.55 * strength, 0.0, 1.0);
    double editor_alpha = CLAMP(1.0 - 0.30 * strength, 0.0, 1.0);

    const char *search = appearance_color_valid(settings->search_color)
        ? settings->search_color : APPEARANCE_SEARCH_DEFAULT;
    g_string_append_printf(css,
        ".relapse-ui .settings-search {\n"
        "    background-color: alpha(%s, %.3f);\n"
        "    border-color: alpha(%s, %.3f);\n"
        "}\n",
        search, editor_alpha, search, editor_alpha);

    g_autoptr(GString) image = g_string_new(NULL);
    gboolean have_image = settings->image_enabled && append_image_url(image, settings->image_path);
    if (!have_image && !settings->gradient) {
        g_string_append(css, ".relapse-ui, .relapse-ui * { --accent-gradient: none; }\n");
        g_string_append(css, "window.relapse-ui { background-image: none; }\n");
        append_animation_override(css, settings);
        return g_string_free(css, FALSE);
    }

    g_string_append(css, "window.relapse-ui {\n");
    if (have_image) {
        g_string_append_printf(css, "    background-image: %s;\n", image->str);
        g_string_append(css, "    background-color: #1c1d20;\n"
                             "    background-size: cover;\n"
                             "    background-position: center;\n"
                             "    background-repeat: no-repeat;\n");
    } else {
        g_string_append(css, "    background-image: linear-gradient(");
        g_string_append_printf(css, "%ddeg, ", CLAMP(settings->angle, 0, 360));
        append_stop(css, settings->start, 1.0, "0%");
        g_string_append(css, ", ");
        if (settings->middle_enabled) {
            append_stop(css, settings->middle, 1.0, "50%");
            g_string_append(css, ", ");
        }
        append_stop(css, settings->end, 1.0, "100%");
        g_string_append(css, ");\n");
    }
    g_string_append(css, "}\n");

    g_string_append(css, ".relapse-ui, .relapse-ui * {\n    --relapse-background: alpha(#1c1d20, ");
    g_string_append_printf(css, "%.3f);\n    --relapse-editor: alpha(#131417, %.3f);\n}\n",
                           background_alpha, editor_alpha);

    double visibility = CLAMP(settings->background_strength, 0.0, 1.0);
    if (visibility < 0.999) {
        g_autofree char *veil = g_strdup_printf("inset 0 0 0 100000px alpha(#1c1d20, %.3f)", 1.0 - visibility);
        g_string_append_printf(css,
            "window.relapse-ui { box-shadow: %s, 0 8px 24px rgba(0, 0, 0, 0.3); }\n"
            "window.relapse-ui.maximized, window.relapse-ui.fullscreen { box-shadow: %s; }\n",
            veil, veil);
    }

    if (!settings->cover_titlebar) {
        const char *color = appearance_color_valid(settings->titlebar_color)
            ? settings->titlebar_color : APPEARANCE_TITLEBAR_DEFAULT;
        g_string_append_printf(css,
            ".relapse-ui .titlebar-wrap { background-color: alpha(%s, %.3f); }\n"
            ".relapse-ui .titlebar-wrap headerbar { background-color: transparent; }\n",
            color, CLAMP(settings->titlebar_opacity, 0.0, 1.0));
    }

    g_string_append_printf(css,
        ".relapse-ui .output-pane { background: alpha(#131417, %.3f); }\n"
        ".relapse-ui .output-header { background: alpha(#1c1d20, %.3f); }\n"
        ".relapse-ui .editor-output-split > separator { background: alpha(#1c1d20, %.3f); }\n"
        ".relapse-ui popover contents { background-color: #1c1d20; }\n",
        editor_alpha, background_alpha, background_alpha);

    g_string_append(css, ".relapse-ui, .relapse-ui * { --accent-gradient: linear-gradient(90deg, ");
    append_stop(css, settings->start, 1.0, "0%");
    g_string_append(css, ", ");
    append_stop(css, settings->end, 1.0, "100%");
    g_string_append(css, "); }\n");
    append_animation_override(css, settings);
    return g_string_free(css, FALSE);
}

void style_set_appearance(GdkDisplay *display, const AppearanceSettings *settings)
{
    if (!display)
        display = gdk_display_get_default();
    if (!display)
        return;
    animations_disabled = settings->animations_disabled;
    GtkCssProvider *provider = g_object_get_data(G_OBJECT(display), "appearance-provider");
    if (!provider) {
        provider = gtk_css_provider_new();
        g_signal_connect(provider, "parsing-error", G_CALLBACK(parsing_error), NULL);
        g_object_set_data_full(G_OBJECT(display), "appearance-provider", provider, g_object_unref);
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_USER + 3);
    }
    g_autofree char *css = appearance_build_css(settings);
    gtk_css_provider_load_from_string(provider, css);
}
