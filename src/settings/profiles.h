#pragma once

#include <glib.h>

#define PROFILES_LAUNCH_DELAY_DEFAULT 1000
#define PROFILES_LAUNCH_DELAY_MIN 0
#define PROFILES_LAUNCH_DELAY_MAX 60000

typedef struct {
    int delay_ms;
    char **excluded;
} ProfilesSettings;

ProfilesSettings profiles_settings_defaults(void);
void profiles_settings_clear(ProfilesSettings *settings);
gboolean profiles_settings_load(ProfilesSettings *settings, GError **error);
gboolean profiles_settings_save(const ProfilesSettings *settings, GError **error);
