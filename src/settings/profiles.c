#include "profiles.h"

#include "store.h"

#include <string.h>

ProfilesSettings profiles_settings_defaults(void)
{
    return (ProfilesSettings){.delay_ms = PROFILES_LAUNCH_DELAY_DEFAULT, .excluded = NULL};
}

void profiles_settings_clear(ProfilesSettings *settings)
{
    g_clear_pointer(&settings->excluded, g_strfreev);
    settings->delay_ms = PROFILES_LAUNCH_DELAY_DEFAULT;
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
    return store_save(file, error);
}
