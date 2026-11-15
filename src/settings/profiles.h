#pragma once

#include <glib.h>

#define PROFILES_LAUNCH_DELAY_DEFAULT 1000
#define PROFILES_LAUNCH_DELAY_MIN 0
#define PROFILES_LAUNCH_DELAY_MAX 60000
#define PROFILES_RESTART_MINUTES_MAX 1440

typedef enum { PROFILES_RESTART_AT_ONCE, PROFILES_RESTART_SEQUENTIAL } ProfilesRestartMode;

typedef struct {
    int delay_ms;
    char **excluded;
    gboolean auto_rejoin;
    char *place_id;
    char *job_id;
    int restart_minutes;
    ProfilesRestartMode restart_mode;
} ProfilesSettings;

ProfilesSettings profiles_settings_defaults(void);
void profiles_settings_clear(ProfilesSettings *settings);
void profiles_settings_copy(ProfilesSettings *to, const ProfilesSettings *from);
gboolean profiles_settings_load(ProfilesSettings *settings, GError **error);
gboolean profiles_settings_save(const ProfilesSettings *settings, GError **error);
