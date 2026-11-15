#include "profiles.h"

#include "store.h"

#include <string.h>

static char *trimmed(const char *text)
{
    if (!text) return NULL;
    g_autofree char *copy = g_strdup(text);
    char *value = g_strstrip(copy);
    return *value ? g_strdup(value) : NULL;
}

ProfilesSettings profiles_settings_defaults(void)
{
    return (ProfilesSettings){
        .delay_ms = PROFILES_LAUNCH_DELAY_DEFAULT,
        .excluded = NULL,
        .auto_rejoin = FALSE,
        .place_id = NULL,
        .job_id = NULL,
        .restart_minutes = 0,
        .restart_mode = PROFILES_RESTART_AT_ONCE,
    };
}

void profiles_settings_clear(ProfilesSettings *settings)
{
    g_clear_pointer(&settings->excluded, g_strfreev);
    g_clear_pointer(&settings->place_id, g_free);
    g_clear_pointer(&settings->job_id, g_free);
    *settings = profiles_settings_defaults();
}

void profiles_settings_copy(ProfilesSettings *to, const ProfilesSettings *from)
{
    *to = profiles_settings_defaults();
    to->delay_ms = from->delay_ms;
    to->excluded = from->excluded ? g_strdupv(from->excluded) : g_new0(char *, 1);
    to->auto_rejoin = from->auto_rejoin;
    to->place_id = g_strdup(from->place_id);
    to->job_id = g_strdup(from->job_id);
    to->restart_minutes = from->restart_minutes;
    to->restart_mode = from->restart_mode;
}

gboolean profiles_settings_load(ProfilesSettings *settings, GError **error)
{
    *settings = profiles_settings_defaults();
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file)
        return FALSE;
    g_key_file_set_list_separator(file, ';');
    g_autoptr(GError) invalid = NULL;
    int delay = g_key_file_get_integer(file, "Profiles", "launch-delay", &invalid);
    if (invalid)
        g_clear_error(&invalid);
    else
        settings->delay_ms = CLAMP(delay, PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX);
    settings->excluded = g_key_file_get_string_list(file, "Profiles", "launch-all-exclude", NULL, NULL);
    if (!settings->excluded)
        settings->excluded = g_new0(char *, 1);
    gboolean rejoin = g_key_file_get_boolean(file, "Profiles", "auto-rejoin", &invalid);
    if (invalid)
        g_clear_error(&invalid);
    else
        settings->auto_rejoin = rejoin;
    settings->place_id = trimmed(g_key_file_get_string(file, "Profiles", "rejoin-place-id", NULL));
    settings->job_id = trimmed(g_key_file_get_string(file, "Profiles", "rejoin-job-id", NULL));
    int minutes = g_key_file_get_integer(file, "Profiles", "restart-minutes", &invalid);
    if (invalid)
        g_clear_error(&invalid);
    else
        settings->restart_minutes = CLAMP(minutes, 0, PROFILES_RESTART_MINUTES_MAX);
    char *mode = g_key_file_get_string(file, "Profiles", "restart-mode", NULL);
    settings->restart_mode = g_strcmp0(mode, "sequential") == 0
        ? PROFILES_RESTART_SEQUENTIAL : PROFILES_RESTART_AT_ONCE;
    g_free(mode);
    return TRUE;
}

gboolean profiles_settings_save(const ProfilesSettings *settings, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file)
        return FALSE;
    g_key_file_set_list_separator(file, ';');
    int delay = CLAMP(settings->delay_ms, PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX);
    g_key_file_set_integer(file, "Profiles", "launch-delay", delay);
    g_key_file_set_string_list(file, "Profiles", "launch-all-exclude",
        (const char *const *)settings->excluded,
        settings->excluded ? g_strv_length(settings->excluded) : 0);
    g_key_file_set_boolean(file, "Profiles", "auto-rejoin", settings->auto_rejoin);
    g_autofree char *place = trimmed(settings->place_id);
    g_autofree char *job = trimmed(settings->job_id);
    g_key_file_set_string(file, "Profiles", "rejoin-place-id", place ? place : "");
    g_key_file_set_string(file, "Profiles", "rejoin-job-id", job ? job : "");
    g_key_file_set_integer(file, "Profiles", "restart-minutes",
        CLAMP(settings->restart_minutes, 0, PROFILES_RESTART_MINUTES_MAX));
    g_key_file_set_string(file, "Profiles", "restart-mode",
        settings->restart_mode == PROFILES_RESTART_SEQUENTIAL ? "sequential" : "at-once");
    return store_save(file, error);
}
