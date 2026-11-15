#pragma once

#include <glib.h>

#define APPEARANCE_ANGLE_DEFAULT 160
#define APPEARANCE_RADIUS_DEFAULT 6
#define APPEARANCE_RADIUS_MAX 24
#define APPEARANCE_OPACITY_MIN 0.0
#define APPEARANCE_OPACITY_DEFAULT 1.0
#define APPEARANCE_BACKGROUND_STRENGTH_DEFAULT 1.0
#define APPEARANCE_TITLEBAR_DEFAULT "#1c1d20"
#define APPEARANCE_TITLEBAR_OPACITY_DEFAULT 1.0
#define APPEARANCE_SEARCH_DEFAULT "#1c1d20"
#define APPEARANCE_START_DEFAULT "#3d4048"
#define APPEARANCE_MIDDLE_DEFAULT "#1c1d20"
#define APPEARANCE_END_DEFAULT "#131417"

typedef struct {
    gboolean gradient;
    gint angle;
    char *start;
    char *middle;
    char *end;
    gboolean middle_enabled;
    gdouble opacity;
    gint radius;
    char *image_path;
    gdouble background_strength;
    gboolean image_enabled;
    char *search_color;
    gboolean cover_titlebar;
    char *titlebar_color;
    gdouble titlebar_opacity;
    gboolean animations_disabled;
} AppearanceSettings;

AppearanceSettings appearance_settings_defaults(void);
gboolean appearance_settings_load(AppearanceSettings *settings, GError **error);
gboolean appearance_settings_save(const AppearanceSettings *settings, GError **error);
void appearance_settings_copy(AppearanceSettings *dest, const AppearanceSettings *src);
void appearance_settings_clear(AppearanceSettings *settings);

gboolean appearance_color_valid(const char *color);
