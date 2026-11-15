#pragma once

#include <gtk/gtk.h>
#include "settings/editor.h"
#include "settings/opener.h"
#include "settings/appearance.h"
#include "settings/profiles.h"

typedef void (*SettingsChangedFunc)(const EditorSettings *settings, gpointer user_data);
typedef void (*RestoreTabsChangedFunc)(gboolean enabled, gpointer user_data);
typedef void (*OutputChangedFunc)(gboolean enabled, gpointer user_data);
typedef gboolean (*LauncherChangedFunc)(const char *path, GError **error, gpointer user_data);
typedef gboolean (*OpenWithChangedFunc)(OpenerMode mode, const char *command, GError **error, gpointer user_data);
typedef gboolean (*ProfilesChangedFunc)(const ProfilesSettings *options, GError **error, gpointer user_data);
typedef void (*RestartProfilesFunc)(gpointer user_data);
typedef gboolean (*RaknetChangedFunc)(gboolean enabled, GError **error, gpointer user_data);
typedef void (*CheckUpdatesFunc)(gpointer user_data);
typedef gboolean (*ScriptsChangedFunc)(const char *path, GError **error, gpointer user_data);
typedef void (*SingleLogChangedFunc)(gboolean enabled, gpointer user_data);
typedef void (*CloseTabsFunc)(gpointer user_data);
typedef void (*ReloadSettingsFunc)(gpointer user_data);
typedef gboolean (*AppearanceChangedFunc)(const AppearanceSettings *settings, GError **error,
                                          gpointer user_data);

GtkWidget *settings_new(const EditorSettings *settings, SettingsChangedFunc changed,
                       RestoreTabsChangedFunc restore_changed, OutputChangedFunc output_changed,
                       SingleLogChangedFunc single_log_changed,
                       LauncherChangedFunc launcher_changed, OpenWithChangedFunc open_with_changed,
                       ProfilesChangedFunc profiles_changed, RestartProfilesFunc restart_profiles,
                       RaknetChangedFunc raknet_changed, CheckUpdatesFunc check_updates,
                       ScriptsChangedFunc scripts_changed, CloseTabsFunc close_tabs,
                       ReloadSettingsFunc reload_settings, const char *version, gpointer user_data);
void settings_set_editor(GtkWidget *page, const EditorSettings *settings);
void settings_set_restore_tabs(GtkWidget *page, gboolean enabled);
void settings_set_output(GtkWidget *page, gboolean enabled);
void settings_set_single_log(GtkWidget *page, gboolean enabled);
void settings_set_launcher(GtkWidget *page, const char *path);
void settings_set_open_with(GtkWidget *page, OpenerMode mode, const char *command);
void settings_set_profiles_options(GtkWidget *page, const ProfilesSettings *options);
void settings_set_profiles(GtkWidget *page, const char *const *names);
void settings_set_raknet(GtkWidget *page, gboolean enabled);
void settings_check_updates(GtkWidget *page);
void settings_set_scripts_path(GtkWidget *page, const char *path);
void settings_set_appearance_handler(GtkWidget *page, AppearanceChangedFunc changed, gpointer user_data);
void settings_set_appearance(GtkWidget *page, const AppearanceSettings *settings);
void settings_shutdown(GtkWidget *page);
