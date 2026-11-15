#include "appearance.h"

#include "store.h"

#include <string.h>

gboolean appearance_color_valid(const char *color)
{
    if (!color || color[0] != '#' || strlen(color) != 7)
        return FALSE;
    for (const char *c = color + 1; *c; ++c)
        if (!g_ascii_isxdigit(*c))
            return FALSE;
    return TRUE;
}

AppearanceSettings appearance_settings_defaults(void)
{
    return (AppearanceSettings){
        .gradient = TRUE,
        .angle = APPEARANCE_ANGLE_DEFAULT,
        .start = g_strdup(APPEARANCE_START_DEFAULT),
        .middle = g_strdup(APPEARANCE_MIDDLE_DEFAULT),
        .end = g_strdup(APPEARANCE_END_DEFAULT),
        .middle_enabled = TRUE,
        .opacity = APPEARANCE_OPACITY_DEFAULT,
        .radius = APPEARANCE_RADIUS_DEFAULT,
        .background_strength = APPEARANCE_BACKGROUND_STRENGTH_DEFAULT,
        .image_enabled = TRUE,
        .search_color = g_strdup(APPEARANCE_SEARCH_DEFAULT),
        .cover_titlebar = TRUE,
        .titlebar_color = g_strdup(APPEARANCE_TITLEBAR_DEFAULT),
        .titlebar_opacity = APPEARANCE_TITLEBAR_OPACITY_DEFAULT,
        .animations_disabled = FALSE,
    };
}

void appearance_settings_clear(AppearanceSettings *settings)
{
    if (!settings)
        return;
    g_free(settings->start);
    g_free(settings->middle);
    g_free(settings->end);
    g_free(settings->image_path);
    g_free(settings->titlebar_color);
    g_free(settings->search_color);
    *settings = (AppearanceSettings){0};
}

void appearance_settings_copy(AppearanceSettings *dest, const AppearanceSettings *src)
{
    if (!dest || !src || dest == src)
        return;
    appearance_settings_clear(dest);
    *dest = (AppearanceSettings){
        .gradient = src->gradient,
        .angle = CLAMP(src->angle, 0, 360),
        .start = g_strdup(appearance_color_valid(src->start) ? src->start : APPEARANCE_START_DEFAULT),
        .middle = g_strdup(appearance_color_valid(src->middle) ? src->middle : APPEARANCE_MIDDLE_DEFAULT),
        .end = g_strdup(appearance_color_valid(src->end) ? src->end : APPEARANCE_END_DEFAULT),
        .middle_enabled = src->middle_enabled,
        .opacity = CLAMP(src->opacity, APPEARANCE_OPACITY_MIN, 1.0),
        .radius = CLAMP(src->radius, 0, APPEARANCE_RADIUS_MAX),
        .image_path = src->image_path && *src->image_path ? g_strdup(src->image_path) : NULL,
        .background_strength = CLAMP(src->background_strength, 0.0, 1.0),
        .image_enabled = src->image_enabled,
        .search_color = g_strdup(appearance_color_valid(src->search_color)
            ? src->search_color : APPEARANCE_SEARCH_DEFAULT),
        .cover_titlebar = src->cover_titlebar,
        .titlebar_color = g_strdup(appearance_color_valid(src->titlebar_color)
            ? src->titlebar_color : APPEARANCE_TITLEBAR_DEFAULT),
        .titlebar_opacity = CLAMP(src->titlebar_opacity, 0.0, 1.0),
        .animations_disabled = src->animations_disabled,
    };
}

static char *read_color(GKeyFile *file, const char *key, const char *fallback)
{
    g_autofree char *value = g_key_file_get_string(file, "Appearance", key, NULL);
    return g_strdup(appearance_color_valid(value) ? value : fallback);
}

gboolean appearance_settings_load(AppearanceSettings *settings, GError **error)
{
    *settings = appearance_settings_defaults();
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file)
        return FALSE;

    if (g_key_file_has_key(file, "Appearance", "gradient", NULL))
        settings->gradient = g_key_file_get_boolean(file, "Appearance", "gradient", NULL);
    if (g_key_file_has_key(file, "Appearance", "middle-enabled", NULL))
        settings->middle_enabled = g_key_file_get_boolean(file, "Appearance", "middle-enabled", NULL);
    if (g_key_file_has_key(file, "Appearance", "angle", NULL))
        settings->angle = CLAMP((int)g_key_file_get_integer(file, "Appearance", "angle", NULL), 0, 360);
    if (g_key_file_has_key(file, "Appearance", "radius", NULL))
        settings->radius = CLAMP((int)g_key_file_get_integer(file, "Appearance", "radius", NULL),
                                 0, APPEARANCE_RADIUS_MAX);
    g_autoptr(GError) failure = NULL;
    double opacity = g_key_file_get_double(file, "Appearance", "opacity", &failure);
    if (!failure)
        settings->opacity = CLAMP(opacity, APPEARANCE_OPACITY_MIN, 1.0);
    g_clear_error(&failure);

    g_free(settings->start);
    g_free(settings->middle);
    g_free(settings->end);
    settings->image_path = g_key_file_get_string(file, "Appearance", "image", NULL);
    g_free(settings->titlebar_color);
    settings->titlebar_color = read_color(file, "titlebar-color", APPEARANCE_TITLEBAR_DEFAULT);
    g_free(settings->search_color);
    settings->search_color = read_color(file, "search-color", APPEARANCE_SEARCH_DEFAULT);
    settings->start = read_color(file, "start", APPEARANCE_START_DEFAULT);
    settings->middle = read_color(file, "middle", APPEARANCE_MIDDLE_DEFAULT);
    settings->end = read_color(file, "end", APPEARANCE_END_DEFAULT);

    double strength = g_key_file_get_double(file, "Appearance", "background-strength", &failure);
    if (!failure)
        settings->background_strength = CLAMP(strength, 0.0, 1.0);
    g_clear_error(&failure);
    double titlebar = g_key_file_get_double(file, "Appearance", "titlebar-opacity", &failure);
    if (!failure)
        settings->titlebar_opacity = CLAMP(titlebar, 0.0, 1.0);
    g_clear_error(&failure);
    if (g_key_file_has_key(file, "Appearance", "cover-titlebar", NULL))
        settings->cover_titlebar = g_key_file_get_boolean(file, "Appearance", "cover-titlebar", NULL);
    if (g_key_file_has_key(file, "Appearance", "image-enabled", NULL))
        settings->image_enabled = g_key_file_get_boolean(file, "Appearance", "image-enabled", NULL);
    if (g_key_file_has_key(file, "Appearance", "disable-animations", NULL))
        settings->animations_disabled = g_key_file_get_boolean(file, "Appearance",
                                                               "disable-animations", NULL);
    return TRUE;
}

gboolean appearance_settings_save(const AppearanceSettings *settings, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file)
        return FALSE;
    g_key_file_set_boolean(file, "Appearance", "gradient", settings->gradient);
    g_key_file_set_boolean(file, "Appearance", "middle-enabled", settings->middle_enabled);
    g_key_file_set_integer(file, "Appearance", "angle", CLAMP(settings->angle, 0, 360));
    g_key_file_set_integer(file, "Appearance", "radius", CLAMP(settings->radius, 0, APPEARANCE_RADIUS_MAX));
    g_key_file_set_double(file, "Appearance", "opacity",
                          CLAMP(settings->opacity, APPEARANCE_OPACITY_MIN, 1.0));
    g_key_file_set_string(file, "Appearance", "start",
                          appearance_color_valid(settings->start) ? settings->start : APPEARANCE_START_DEFAULT);
    g_key_file_set_string(file, "Appearance", "middle",
                          appearance_color_valid(settings->middle) ? settings->middle : APPEARANCE_MIDDLE_DEFAULT);
    g_key_file_set_string(file, "Appearance", "end",
                          appearance_color_valid(settings->end) ? settings->end : APPEARANCE_END_DEFAULT);
    if (settings->image_path && *settings->image_path)
        g_key_file_set_string(file, "Appearance", "image", settings->image_path);
    else
        g_key_file_remove_key(file, "Appearance", "image", NULL);
    g_key_file_set_boolean(file, "Appearance", "image-enabled", settings->image_enabled);
    g_key_file_set_double(file, "Appearance", "background-strength",
                          CLAMP(settings->background_strength, 0.0, 1.0));
    g_key_file_set_string(file, "Appearance", "search-color",
                          appearance_color_valid(settings->search_color)
                              ? settings->search_color : APPEARANCE_SEARCH_DEFAULT);
    g_key_file_set_boolean(file, "Appearance", "cover-titlebar", settings->cover_titlebar);
    g_key_file_set_string(file, "Appearance", "titlebar-color",
                          appearance_color_valid(settings->titlebar_color)
                              ? settings->titlebar_color : APPEARANCE_TITLEBAR_DEFAULT);
    g_key_file_set_double(file, "Appearance", "titlebar-opacity",
                          CLAMP(settings->titlebar_opacity, 0.0, 1.0));
    g_key_file_set_boolean(file, "Appearance", "disable-animations", settings->animations_disabled);
    return store_save(file, error);
}
