#include "app.h"
#include "socket/sessions.h"
#include "ui/style.h"
#include "ui/window.h"
#include "ui/tray.h"
#include "ui/console.h"
#include "settings/session.h"
#include "settings/output.h"
#include "socket/bridge.h"
#include "profiles/account.h"
#include "profiles/cache.h"
#include "profiles/store.h"
#include "settings/launcher.h"
#include "settings/opener.h"
#include "settings/profiles.h"
#include "settings/appearance.h"
#include "settings/raknet.h"
#include "settings/scripts.h"
#include "state/log.h"
#include "update/check.h"
#include <signal.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    GtkWindow *window;
    Sessions *sessions;
    char *scripts_path;
    char *launcher_path;
    char **socket_paths;
    guint discovery;
    guint update_timer;
    gboolean manual_update_check;
    gboolean redirect_output;
    gboolean single_log;
    ProfilesSettings profiles_options;
    AppearanceSettings appearance;
    GHashTable *launches;
    GHashTable *crashes;
    struct RestartBatch *restart_batch;
    guint restart_timer;
    GHashTable *consoles;
    Tray *tray;
    GHashTable *account_attempts;
    GCancellable *account_scan;
    char **scanned_sockets;
    guint scan_timer;
    gboolean scan_ready;
} App;

typedef struct {
    App *app;
    char *path;
    GtkWindow *window;
} ConsoleView;

static void free_console_view(gpointer user_data)
{
    ConsoleView *view = user_data;
    gtk_window_destroy(view->window);
    g_free(view->path);
    g_free(view);
}

static void console_answer(gint64 id, const char *text, gpointer user_data)
{
    ConsoleView *view = user_data;
    sessions_console_input(view->app->sessions, view->path, id, text);
}

static ConsoleView *console_view(App *app, const char *path)
{
    ConsoleView *view = g_hash_table_lookup(app->consoles, path);
    if (view) return view;
    view = g_new0(ConsoleView, 1);
    view->app = app;
    view->path = g_strdup(path);
    view->window = console_window_new(app->window, console_answer, view);
    g_hash_table_insert(app->consoles, view->path, view);
    return view;
}

static void console_show(const char *path, gboolean visible, gpointer user_data)
{
    console_window_show(console_view(user_data, path)->window, visible);
}

static void console_title(const char *path, const char *title, gpointer user_data)
{
    console_window_set_title(console_view(user_data, path)->window, title);
}

static void console_clear(const char *path, gpointer user_data)
{
    console_window_clear(console_view(user_data, path)->window);
}

static void console_entry(const char *path, const char *level, const char *text, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;
    if (!log_append(path, app->single_log, level, text, &error))
        g_printerr("Could not write the log: %s\n", error->message);
    console_window_append(console_view(app, path)->window, level, text);
}

static void console_input(const char *path, gint64 id, gpointer user_data)
{
    console_window_request_input(console_view(user_data, path)->window, id);
}

static void update_connection_status(App *app);

static guint count_ready_clients(App *app)
{
    guint ready = 0;
    for (guint i = 0; app->socket_paths && app->socket_paths[i]; ++i)
        if (sessions_can_execute(app->sessions, app->socket_paths[i])) ++ready;
    return ready;
}

static void update_execute(gpointer user_data)
{
    App *app = user_data;
    guint selected = app->socket_paths ? g_strv_length(app->socket_paths) : 0;
    guint ready = count_ready_clients(app);
    window_set_execute_state(app->window, ready > 0 && window_get_buffer(app->window) != NULL, ready, selected);
}

static void output_state_changed(const char *path, OutputState state, const char *detail, gpointer user_data)
{
    App *app = user_data;
    window_set_output_status(app->window, path, detail,
        sessions_is_active(app->sessions, path));
    if (state == OUTPUT_FAILED)
        window_append_output(app->window, path, "error", detail);
    update_connection_status(app);
}

static void output_entry(const char *path, const char *level, const char *text, gpointer user_data)
{
    App *app = user_data;
    window_append_output(app->window, path, level, text);
}

static gboolean save_output(gboolean enabled, GError **error, gpointer user_data)
{
    App *app = user_data;
    app->redirect_output = enabled;
    sessions_set_output_enabled(app->sessions, enabled);
    return output_settings_save(enabled, error);
}

static gboolean save_single_log(gboolean enabled, GError **error, gpointer user_data)
{
    App *app = user_data;
    app->single_log = enabled;
    window_set_single_log(app->window, enabled);
    return output_log_settings_save(enabled, error);
}

static void open_script(GFile *file, const char *display_name, gpointer user_data)
{
    App *app = user_data;
    window_open_script(app->window, file, display_name);
}

static void open_tab_externally(GFile *file, const char *display_name, gpointer user_data)
{
    (void)display_name;
    App *app = user_data;
    OpenerSettings opener;
    g_autoptr(GError) error = NULL;
    if (!opener_settings_load(&opener, &error))
        g_printerr("Could not load script opener preference: %s\n", error->message);
    if (opener.mode == OPENER_EDITOR || opener.mode == OPENER_NOTHING) {
        opener_settings_clear(&opener);
        return;
    }
    if (!file) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Please save the script");
        gtk_alert_dialog_set_detail(alert,
            "Draft tabs only live in the session cache and have no file to open yet.");
        gtk_alert_dialog_show(alert, app->window);
    } else if (!opener_open(file, &opener, &error)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not open script externally");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "Unknown error");
        gtk_alert_dialog_show(alert, app->window);
    }
    opener_settings_clear(&opener);
}

typedef struct {
    GWeakRef window;
    char *url;
    char *name;
    gboolean is_page;
} DownloadChoice;

static void download_finished(const char *path, const char *error, gpointer user_data)
{
    DownloadChoice *choice = user_data;
    g_autoptr(GtkWindow) window = g_weak_ref_get(&choice->window);
    if (window) {
        g_autoptr(GtkAlertDialog) alert;
        if (error) {
            alert = gtk_alert_dialog_new("Could not download the update");
            gtk_alert_dialog_set_detail(alert, error);
        } else {
            alert = gtk_alert_dialog_new("Update downloaded");
            g_autofree char *detail = g_strdup_printf(
                "The new AppImage was saved to\n%s\nRun it when you are ready to switch.", path);
            gtk_alert_dialog_set_detail(alert, detail);
        }
        gtk_alert_dialog_show(alert, window);
    }
    g_weak_ref_clear(&choice->window);
    g_free(choice->url);
    g_free(choice->name);
    g_free(choice);
}

static void download_choice_finished(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    DownloadChoice *choice = user_data;
    int index = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL);
    if (index == 1) {
        g_autoptr(GtkWindow) window = g_weak_ref_get(&choice->window);
        if (window && choice->is_page) {
            g_autoptr(GError) error = NULL;
            if (!g_app_info_launch_default_for_uri(choice->url, NULL, &error)) {
                g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not open the release page");
                gtk_alert_dialog_set_detail(alert, error->message);
                gtk_alert_dialog_show(alert, window);
            }
        } else if (window) {
            DownloadChoice *download = g_new0(DownloadChoice, 1);
            g_weak_ref_init(&download->window, window);
            download->url = g_strdup(choice->url);
            download->name = g_strdup(choice->name);
            g_autofree char *directory = update_download_directory();
            update_download_async(download->url, download->name, directory,
                                  download_finished, download);
        }
    }
    g_weak_ref_clear(&choice->window);
    g_free(choice->url);
    g_free(choice->name);
    g_free(choice);
}

static void manual_update_checked(const UpdateInfo *info, const char *error, gpointer user_data)
{
    App *app = user_data;
    gboolean manual = app->manual_update_check;
    app->manual_update_check = FALSE;
    if (!app->window)
        return;
    if (error) {
        if (!manual)
            return;
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not check for updates");
        gtk_alert_dialog_set_detail(alert, error);
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    if (!info || !update_version_newer(info->version, update_current_version())) {
        if (!manual)
            return;
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("RelapseGtk is up to date");
        g_autofree char *detail = g_strdup_printf("You are running %s.", update_current_version());
        gtk_alert_dialog_set_detail(alert, detail);
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    gboolean has_appimage = info->download_url && *info->download_url;
    const char *new_version = info->version[0] == 'v' ? info->version + 1 : info->version;
    g_autofree char *title = g_strdup_printf("RelapseGtk %s is available", new_version);
    g_autofree char *directory = update_download_directory();
    g_autofree char *detail = has_appimage
        ? g_strdup_printf("You are running %s. The new AppImage will be saved to\n%s\nand will not be installed automatically.",
                          update_current_version(), directory)
        : g_strdup_printf("You are running %s. This release has no AppImage attached; open the release page instead\n%s",
                          update_current_version(), info->release_url ? info->release_url : "");
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, detail);
    const char *buttons[] = {"Later", has_appimage ? "Download" : "Open release page", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);
    DownloadChoice *choice = g_new0(DownloadChoice, 1);
    g_weak_ref_init(&choice->window, app->window);
    choice->url = g_strdup(has_appimage ? info->download_url : info->release_url);
    choice->name = g_strdup(info->asset_name);
    choice->is_page = !has_appimage;
    gtk_alert_dialog_choose(alert, app->window, NULL, download_choice_finished, choice);
}

static void check_updates_clicked(gpointer user_data)
{
    App *app = user_data;
    app->manual_update_check = TRUE;
    update_check_async(manual_update_checked, app);
}

static gboolean check_updates_startup(gpointer user_data)
{
    App *app = user_data;
    app->update_timer = 0;
    update_check_async(manual_update_checked, app);
    return G_SOURCE_REMOVE;
}

static gboolean save_open_with(OpenerMode mode, const char *command, GError **error, gpointer user_data)
{
    (void)user_data;
    OpenerSettings settings = {.mode = mode, .command = (char *)command};
    return opener_settings_save(&settings, error);
}

static gboolean save_editor_settings(const EditorSettings *settings, GError **error, gpointer user_data)
{
    (void)user_data;
    return editor_settings_save(settings, error);
}

static gboolean save_appearance(const AppearanceSettings *settings, GError **error, gpointer user_data)
{
    App *app = user_data;
    appearance_settings_copy(&app->appearance, settings);
    return appearance_settings_save(&app->appearance, error);
}

static gboolean save_restore_tabs(gboolean enabled, GError **error, gpointer user_data)
{
    (void)user_data;
    return session_settings_save(enabled, error);
}

static void apply_settings(App *app)
{
    g_autoptr(GError) error = NULL;
    char *scripts = scripts_settings_load(&error);
    if (!scripts) {
        g_printerr("Could not load script folder preference: %s\n", error->message);
    } else {
        g_free(app->scripts_path);
        app->scripts_path = scripts;
        window_set_scripts_path(app->window, scripts);
    }
    g_clear_error(&error);
    EditorSettings editor;
    if (!editor_settings_load(&editor, &error))
        g_printerr("Could not load settings: %s\n", error->message);
    else
        window_set_editor_settings(app->window, &editor);
    g_clear_error(&error);
    gboolean redirect;
    if (!output_settings_load(&redirect, &error))
        g_printerr("Could not load output preference: %s\n", error->message);
    else {
        window_set_output_enabled(app->window, redirect);
        app->redirect_output = redirect;
    }
    g_clear_error(&error);
    gboolean single_log = FALSE;
    if (!output_log_settings_load(&single_log, &error))
        g_printerr("Could not load log preference: %s\n", error->message);
    else {
        app->single_log = single_log;
        window_set_single_log(app->window, single_log);
    }
    g_clear_error(&error);
    g_free(app->launcher_path);
    app->launcher_path = NULL;
    if (!launcher_settings_load(&app->launcher_path, &error))
        g_printerr("Could not load launcher preference: %s\n", error->message);
    window_set_launcher(app->window, app->launcher_path);
    g_clear_error(&error);
    OpenerSettings opener;
    if (!opener_settings_load(&opener, &error))
        g_printerr("Could not load script opener preference: %s\n", error->message);
    window_set_open_with(app->window, opener.mode, opener.command);
    opener_settings_clear(&opener);
    g_clear_error(&error);
    ProfilesSettings profiles = {0};
    if (!profiles_settings_load(&profiles, &error)) {
        g_printerr("Could not load profile launch settings: %s\n", error->message);
    } else {
        profiles_settings_clear(&app->profiles_options);
        profiles_settings_copy(&app->profiles_options, &profiles);
        window_set_profiles_options(app->window, &app->profiles_options);
    }
    profiles_settings_clear(&profiles);
    g_clear_error(&error);
    gboolean raknet = TRUE;
    if (!raknet_settings_load(&raknet))
        g_printerr("Could not read the Relapse config; RakNet state is unknown.\n");
    window_set_raknet(app->window, raknet);
    AppearanceSettings appearance = {0};
    if (!appearance_settings_load(&appearance, &error))
        g_printerr("Could not load appearance settings: %s\n", error->message);
    appearance_settings_copy(&app->appearance, &appearance);
    window_set_appearance(app->window, &app->appearance);
    appearance_settings_clear(&appearance);
    g_clear_error(&error);
}

static void settings_imported(gpointer user_data)
{
    App *app = user_data;
    apply_settings(app);
    g_autoptr(GError) error = NULL;
    gboolean restore = TRUE;
    if (!session_settings_load(&restore, &error))
        g_printerr("Could not load session preference: %s\n", error->message);
    else
        window_set_restore_tabs(app->window, restore);
}

static void update_connection_status(App *app)
{
    guint selected = app->socket_paths ? g_strv_length(app->socket_paths) : 0;
    if (selected > 1) {
        guint ready = count_ready_clients(app);
        g_autofree char *label = g_strdup_printf("%u of %u ready", ready, selected);
        window_set_status(app->window, label, "Execute sends to the selected clients that are ready.", ready > 0);
    } else if (!selected) {
        window_set_status(app->window, "No target", "Select clients beside Execute.", FALSE);
    } else switch (sessions_get_state(app->sessions, app->socket_paths[0])) {
        case SOCKET_CLIENT_READY:
            window_set_status(app->window, "Ready", "Connected. Ready to execute scripts.", TRUE);
            break;
        case SOCKET_CLIENT_WAITING:
            window_set_status(app->window, "Not ready", "Connected. Waiting for a game to be ready.", FALSE);
            break;
        default:
            window_set_status(app->window, "Disconnected", "Waiting for a socket to become available.", FALSE);
            break;
    }
    update_execute(app);
}

static void socket_state_changed(const char *path, SocketClientState state, const char *detail, gpointer user_data)
{
    (void)detail;
    App *app = user_data;
    gboolean active = sessions_is_active(app->sessions, path);
    if (state != SOCKET_CLIENT_READY) {
        g_hash_table_remove(app->consoles, path);
        window_set_output_status(app->window, path,
            !active ? "Closed" : state == SOCKET_CLIENT_WAITING ? "Waiting for a game" : "Reconnecting…", active);
    }
    if (app->socket_paths && g_strv_contains((const char *const *)app->socket_paths, path)) update_connection_status(app);
}

static void script_result(const char *path, gint64 script_id, gboolean submitted, const char *detail, gpointer user_data)
{
    (void)script_id;
    App *app = user_data;
    if (!submitted)
        window_append_output(app->window, path, "error", detail ? detail : "Script submission failed.");
}

static void select_socket(const char *const *paths, gpointer user_data)
{
    App *app = user_data;
    g_auto(GStrv) selected = g_strdupv((char **)paths);
    g_strfreev(app->socket_paths);
    app->socket_paths = g_steal_pointer(&selected);
    if (paths[0] && !paths[1]) window_set_output_target(app->window, paths[0]);
    update_connection_status(app);
}

static gboolean refresh_profiles(gpointer user_data);

typedef struct {
    GWeakRef window;
    char *name;
    char *cookie;
} ScanLookup;

static void scan_checked(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    ScanLookup *lookup = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(ProfileAccount) account = profile_account_lookup_finish(result, &error);
    if (account) profile_account_save(lookup->name, account, NULL);
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        profile_cache_store_invalid(lookup->cookie);
    profile_cache_release(lookup->cookie);
    g_autoptr(GtkWindow) window = g_weak_ref_get(&lookup->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (app) refresh_profiles(app);
    g_weak_ref_clear(&lookup->window);
    g_free(lookup->name);
    g_free(lookup->cookie);
    g_free(lookup);
}

static void scan_profile(App *app, const char *name)
{
    if (!app->scan_ready || !app->account_scan || !profile_name_valid(name)) return;
    g_autofree char *cookie = profile_cookie_read_any(name, NULL);
    if (!cookie) return;
    gboolean valid = FALSE;
    g_autoptr(ProfileAccount) cached = NULL;
    if (profile_cache_lookup(cookie, &valid, &cached)) return;
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    g_autofree char *key = g_strdup_printf("%s:%s", name, hash);
    if (g_hash_table_contains(app->account_attempts, key)) return;
    if (!profile_cache_claim(cookie)) return;
    g_hash_table_add(app->account_attempts, g_strdup(key));
    ScanLookup *lookup = g_new0(ScanLookup, 1);
    g_weak_ref_init(&lookup->window, app->window);
    lookup->name = g_strdup(name);
    lookup->cookie = g_steal_pointer(&cookie);
    profile_account_lookup_async(lookup->cookie, app->account_scan, scan_checked, lookup);
}

static void scan_instances(App *app, const char *const *sockets)
{
    for (guint i = 0; sockets[i]; ++i) {
        g_autofree char *name = profile_socket_name(sockets[i]);
        scan_profile(app, name);
    }
}

static gboolean scan_startup(gpointer user_data)
{
    App *app = user_data;
    app->scan_timer = 0;
    app->scan_ready = TRUE;
    g_auto(GStrv) names = profiles_list();
    for (guint i = 0; names[i]; ++i) scan_profile(app, names[i]);
    return G_SOURCE_REMOVE;
}

static gboolean refresh_profiles(gpointer user_data)
{
    App *app = user_data;
    g_auto(GStrv) names = profiles_list();
    g_auto(GStrv) sockets = profile_sockets_list("/tmp");
    sessions_update(app->sessions, (const char *const *)sockets);
    g_auto(GStrv) active = sessions_list_active(app->sessions);
    window_update_profiles(app->window, (const char *const *)names,
        (const char *const *)active);
    update_connection_status(app);
    if (!app->scanned_sockets ||
        !g_strv_equal((const char *const *)sockets, (const char *const *)app->scanned_sockets)) {
        g_strfreev(app->scanned_sockets);
        app->scanned_sockets = g_strdupv((char **)sockets);
        scan_instances(app, (const char *const *)app->scanned_sockets);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean save_launcher(const char *path, GError **error, gpointer user_data)
{
    App *app = user_data;
    if (!launcher_settings_save(path, error)) return FALSE;
    g_free(app->launcher_path);
    app->launcher_path = g_strdup(path);
    return TRUE;
}

static gboolean launcher_configured(const char *path)
{
    return path && *path && g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
}

static void launcher_scan_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
    if (choice != 1 || !app->window)
        return;
    g_autofree char *path = launcher_find();
    if (!path) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not find relapse-launcher");
        gtk_alert_dialog_set_detail(alert,
            "Searched ~/Downloads, then your home folder two levels deep.");
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    g_autoptr(GError) error = NULL;
    if (!save_launcher(path, &error, app)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save the launcher path");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "Unknown error");
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    window_set_launcher(app->window, app->launcher_path);
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Relapse launcher found");
    gtk_alert_dialog_set_detail(alert, path);
    gtk_alert_dialog_show(alert, app->window);
}

static void prompt_missing_launcher(App *app)
{
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Find the Relapse launcher?");
    gtk_alert_dialog_set_detail(alert,
        "No relapse-launcher binary is configured. RelapseGtk can scan ~/Downloads first, "
        "then your home folder two levels deep, and use the newest copy it finds.");
    const char *buttons[] = {"Not now", "Scan", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 1);
    gtk_alert_dialog_choose(alert, app->window, NULL, launcher_scan_finished, app);
}

static gboolean save_scripts_path(const char *path, GError **error, gpointer user_data)
{
    App *app = user_data;
    if (!scripts_settings_save(path, error)) return FALSE;
    g_free(app->scripts_path);
    app->scripts_path = g_strdup(path);
    window_set_scripts_path(app->window, path);
    return TRUE;
}

static gboolean profile_running(const char *name, gpointer user_data)
{
    App *app = user_data;
    return g_hash_table_contains(app->launches, name) || profile_is_running(name);
}

typedef struct {
    char *name;
    GSubprocess *process;
    char *place_id;
    char *job_id;
    gboolean stopping;
} LaunchState;

static void launch_state_free(gpointer data)
{
    LaunchState *state = data;
    if (!state) return;
    g_clear_object(&state->process);
    g_free(state->name);
    g_free(state->place_id);
    g_free(state->job_id);
    g_free(state);
}

typedef struct { guint count; gint64 last; } CrashRecord;

static void crash_record_free(gpointer data)
{
    g_free(data);
}

typedef struct {
    GWeakRef window;
    char *name;
} Launch;

typedef struct {
    GWeakRef window;
    char *name;
    char *place_id;
    char *job_id;
} Rejoin;

typedef struct {
    char *name;
    char *place_id;
    char *job_id;
    GPtrArray *pids;
} RestartTarget;

typedef struct RestartBatch {
    App *app;
    GWeakRef window;
    GPtrArray *targets;
    guint index;
    guint delay;
    guint timer;
    guint kill_timer;
    gint64 deadline;
} RestartBatch;

static gboolean launch_profile(const char *name, const char *place_id, const char *job_id,
                               GError **error, gpointer user_data);
static void restart_batch_free(RestartBatch *batch);

static void rejoin_free(Rejoin *rejoin)
{
    g_weak_ref_clear(&rejoin->window);
    g_free(rejoin->name);
    g_free(rejoin->place_id);
    g_free(rejoin->job_id);
    g_free(rejoin);
}

static gboolean rejoin_fired(gpointer user_data)
{
    Rejoin *rejoin = user_data;
    g_autoptr(GtkWindow) window = g_weak_ref_get(&rejoin->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (app && app->window && !g_hash_table_contains(app->launches, rejoin->name)) {
        g_autoptr(GError) error = NULL;
        if (!launch_profile(rejoin->name, rejoin->place_id, rejoin->job_id, &error, app))
            g_printerr("Could not rejoin %s: %s\n", rejoin->name,
                error ? error->message : "Unknown error.");
    }
    rejoin_free(rejoin);
    return G_SOURCE_REMOVE;
}

static void schedule_rejoin(App *app, const char *name, const char *place_id, const char *job_id)
{
    Rejoin *rejoin = g_new0(Rejoin, 1);
    g_weak_ref_init(&rejoin->window, app->window);
    rejoin->name = g_strdup(name);
    const char *place = app->profiles_options.place_id;
    const char *job = app->profiles_options.job_id;
    rejoin->place_id = g_strdup(place && *place ? place : place_id);
    rejoin->job_id = g_strdup(place && *place ? job : job_id);
    g_timeout_add(1000, rejoin_fired, rejoin);
}

static gboolean auto_rejoin_allowed(App *app, const char *name)
{
    /* A launch that always crashes must not turn into a relaunch loop. */
    gint64 now = g_get_monotonic_time();
    CrashRecord *record = g_hash_table_lookup(app->crashes, name);
    if (!record || now - record->last > 60 * G_USEC_PER_SEC) {
        record = g_new0(CrashRecord, 1);
        g_hash_table_replace(app->crashes, g_strdup(name), record);
    }
    record->last = now;
    record->count++;
    return record->count <= 5;
}

static void launch_finished(GObject *process, GAsyncResult *result, gpointer user_data)
{
    Launch *launch = user_data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_subprocess_wait_check_finish(G_SUBPROCESS(process), result, &error);
    g_autoptr(GtkWindow) window = g_weak_ref_get(&launch->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (app && app->window) {
        LaunchState *state = g_hash_table_lookup(app->launches, launch->name);
        if (state) {
            g_hash_table_steal(app->launches, launch->name);
            if (!state->stopping && !success) {
                if (app->profiles_options.auto_rejoin &&
                    auto_rejoin_allowed(app, state->name)) {
                    schedule_rejoin(app, state->name, state->place_id, state->job_id);
                } else {
                    window_set_profile_launch_error(window, launch->name,
                        error ? error->message : "The client stopped unexpectedly.");
                }
            }
            launch_state_free(state);
        }
        refresh_profiles(app);
    }
    g_weak_ref_clear(&launch->window);
    g_free(launch->name);
    g_free(launch);
}

static gboolean launch_profile(const char *name, const char *place_id, const char *job_id,
                                GError **error, gpointer user_data)
{
    App *app = user_data;
    if (!app->launcher_path || !*app->launcher_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Choose a launcher in Settings first.");
        return FALSE;
    }
    if (g_hash_table_contains(app->launches, name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "This profile is already running.");
        return FALSE;
    }
    GSubprocess *process = profile_launch(app->launcher_path, name, place_id, job_id, error);
    if (!process) return FALSE;
    LaunchState *state = g_new0(LaunchState, 1);
    state->name = g_strdup(name);
    state->process = process;
    state->place_id = g_strdup(place_id);
    state->job_id = g_strdup(job_id);
    g_hash_table_replace(app->launches, g_strdup(name), state);
    Launch *launch = g_new0(Launch, 1);
    g_weak_ref_init(&launch->window, app->window);
    launch->name = g_strdup(name);
    g_subprocess_wait_check_async(process, NULL, launch_finished, launch);
    refresh_profiles(app);
    return TRUE;
}

typedef struct {
    App *app;
    char **names;
    guint index;
    guint delay;
    guint timer;
} LaunchAll;

static void launch_all_free(LaunchAll *batch)
{
    if (batch->timer)
        g_source_remove(batch->timer);
    g_strfreev(batch->names);
    g_free(batch);
}

static gboolean launch_all_next(gpointer user_data);

static void launch_all_schedule(LaunchAll *batch)
{
    batch->timer = g_timeout_add(batch->delay ? batch->delay : 1, launch_all_next, batch);
}

static gboolean launch_all_next(gpointer user_data)
{
    LaunchAll *batch = user_data;
    App *app = batch->app;
    batch->timer = 0;
    if (!app->window) {
        launch_all_free(batch);
        return G_SOURCE_REMOVE;
    }
    if (!app->launcher_path || !*app->launcher_path) {
        window_show_settings(app->window);
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Select a launcher first");
        gtk_alert_dialog_set_detail(alert,
            "Choose your launcher executable under Profiles in Settings, then launch again.");
        gtk_alert_dialog_show(alert, app->window);
        launch_all_free(batch);
        return G_SOURCE_REMOVE;
    }
    while (batch->names[batch->index]) {
        const char *name = batch->names[batch->index++];
        if (profile_running(name, app))
            continue;
        g_autoptr(GError) error = NULL;
        if (!launch_profile(name, NULL, NULL, &error, app)) {
            g_autoptr(GtkAlertDialog) alert =
                gtk_alert_dialog_new("Could not launch Profile %s", name);
            gtk_alert_dialog_set_detail(alert, error ? error->message : "Unknown error.");
            gtk_alert_dialog_show(alert, app->window);
            launch_all_free(batch);
            return G_SOURCE_REMOVE;
        }
        launch_all_schedule(batch);
        return G_SOURCE_REMOVE;
    }
    launch_all_free(batch);
    return G_SOURCE_REMOVE;
}

static void launch_all_profiles(const char *const *names, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GPtrArray) pending = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; names[i]; ++i) {
        if (g_strv_contains((const char *const *)app->profiles_options.excluded, names[i]))
            continue;
        g_ptr_array_add(pending, g_strdup(names[i]));
    }
    if (!pending->len) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Nothing to launch");
        gtk_alert_dialog_set_detail(alert,
            "Every Profile is either skipped in Settings or already running.");
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    g_ptr_array_add(pending, NULL);
    LaunchAll *batch = g_new0(LaunchAll, 1);
    batch->app = app;
    batch->delay = (guint)CLAMP(app->profiles_options.delay_ms,
        PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX);
    batch->names = (char **)g_ptr_array_free(g_steal_pointer(&pending), FALSE);
    launch_all_next(batch);
}

static void restart_target_free(gpointer data)
{
    RestartTarget *target = data;
    g_free(target->name);
    g_free(target->place_id);
    g_free(target->job_id);
    profile_processes_free(target->pids);
    g_free(target);
}

static void restart_batch_free(RestartBatch *batch)
{
    if (batch->timer)
        g_source_remove(batch->timer);
    if (batch->kill_timer)
        g_source_remove(batch->kill_timer);
    if (batch->app && batch->app->restart_batch == batch)
        batch->app->restart_batch = NULL;
    g_ptr_array_unref(batch->targets);
    g_weak_ref_clear(&batch->window);
    g_free(batch);
}

static pid_t launch_pid(GSubprocess *process)
{
    const char *identifier = process ? g_subprocess_get_identifier(process) : NULL;
    return identifier ? (pid_t)g_ascii_strtoll(identifier, NULL, 10) : 0;
}

static void process_terminate(GSubprocess *process, int signal_number)
{
    pid_t pid = launch_pid(process);
    if (pid > 0 && kill(-pid, signal_number) == 0) return;
    if (process) g_subprocess_send_signal(process, signal_number);
}

static gboolean restart_relaunch_next(gpointer user_data)
{
    RestartBatch *batch = user_data;
    batch->timer = 0;
    g_autoptr(GtkWindow) window = g_weak_ref_get(&batch->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (!app || !app->window) {
        restart_batch_free(batch);
        return G_SOURCE_REMOVE;
    }
    while (batch->index < batch->targets->len) {
        RestartTarget *target = g_ptr_array_index(batch->targets, batch->index);
        if (g_hash_table_contains(app->launches, target->name)) {
            /* Wait for the SIGKILL to land before relaunching this client. */
            if (!batch->deadline)
                batch->deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
            if (g_get_monotonic_time() < batch->deadline) {
                batch->timer = g_timeout_add(100, restart_relaunch_next, batch);
                return G_SOURCE_REMOVE;
            }
            ++batch->index;
            continue;
        }
        ++batch->index;
        const char *place = app->profiles_options.place_id;
        const char *place_id = place && *place ? place : target->place_id;
        const char *job_id = place && *place ? app->profiles_options.job_id : target->job_id;
        g_autoptr(GError) error = NULL;
        if (!launch_profile(target->name, place_id, job_id, &error, app))
            g_printerr("Could not restart %s: %s\n", target->name,
                error ? error->message : "Unknown error.");
        batch->timer = g_timeout_add(batch->delay ? batch->delay : 1,
            restart_relaunch_next, batch);
        return G_SOURCE_REMOVE;
    }
    restart_batch_free(batch);
    return G_SOURCE_REMOVE;
}

static gboolean restart_force_kill(gpointer user_data)
{
    RestartBatch *batch = user_data;
    batch->kill_timer = 0;
    for (guint i = 0; i < batch->targets->len; ++i) {
        RestartTarget *target = g_ptr_array_index(batch->targets, i);
        profile_processes_kill(target->pids, SIGKILL);
        GPtrArray *remaining = profile_processes(target->name, 0);
        profile_processes_kill(remaining, SIGKILL);
        profile_processes_free(remaining);
    }
    return G_SOURCE_REMOVE;
}

static void restart_all(App *app, gboolean manual)
{
    if (!app->window) return;
    if (!g_hash_table_size(app->launches)) {
        if (manual) {
            g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("No clients are running");
            gtk_alert_dialog_set_detail(alert,
                "Launch a Profile before restarting the clients.");
            gtk_alert_dialog_show(alert, app->window);
        }
        return;
    }
    g_clear_pointer(&app->restart_batch, restart_batch_free);
    RestartBatch *batch = g_new0(RestartBatch, 1);
    batch->app = app;
    batch->targets = g_ptr_array_new_with_free_func(restart_target_free);
    g_weak_ref_init(&batch->window, app->window);
    batch->delay = app->profiles_options.restart_mode == PROFILES_RESTART_SEQUENTIAL
        ? (guint)CLAMP(app->profiles_options.delay_ms, PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX)
        : 0;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->launches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LaunchState *state = value;
        state->stopping = TRUE;
        RestartTarget *target = g_new0(RestartTarget, 1);
        target->name = g_strdup(state->name);
        target->place_id = g_strdup(state->place_id);
        target->job_id = g_strdup(state->job_id);
        target->pids = profile_processes(state->name, launch_pid(state->process));
        profile_processes_kill(target->pids, SIGTERM);
        process_terminate(state->process, SIGTERM);
        g_ptr_array_add(batch->targets, target);
    }
    batch->kill_timer = g_timeout_add(200, restart_force_kill, batch);
    app->restart_batch = batch;
    batch->timer = g_timeout_add(400, restart_relaunch_next, batch);
}

static gboolean restart_timer_fired(gpointer user_data)
{
    App *app = user_data;
    if (app->window) restart_all(app, FALSE);
    return G_SOURCE_CONTINUE;
}

static void arm_restart_timer(App *app)
{
    if (app->restart_timer) {
        g_source_remove(app->restart_timer);
        app->restart_timer = 0;
    }
    int minutes = app->profiles_options.restart_minutes;
    if (minutes > 0 && app->window)
        app->restart_timer = g_timeout_add_seconds((guint)minutes * 60u, restart_timer_fired, app);
}

static void restart_profiles_now(gpointer user_data)
{
    restart_all(user_data, TRUE);
}

typedef struct {
    char *name;
    GPtrArray *pids;
} KillRequest;

static gboolean kill_escalate(gpointer user_data)
{
    KillRequest *kill = user_data;
    profile_processes_kill(kill->pids, SIGKILL);
    GPtrArray *remaining = profile_processes(kill->name, 0);
    profile_processes_kill(remaining, SIGKILL);
    profile_processes_free(remaining);
    profile_processes_free(kill->pids);
    g_free(kill->name);
    g_free(kill);
    return G_SOURCE_REMOVE;
}

static gboolean kill_profile(const char *name, GError **error, gpointer user_data)
{
    App *app = user_data;
    if (!profile_name_valid(name))
        { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid profile name."); return FALSE; }
    LaunchState *state = g_hash_table_lookup(app->launches, name);
    if (state) state->stopping = TRUE;
    GPtrArray *pids = profile_processes(name, state ? launch_pid(state->process) : 0);
    if (!pids->len) {
        profile_processes_free(pids);
        { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "This Profile is not running."); return FALSE; }
    }
    profile_processes_kill(pids, SIGTERM);
    if (state) process_terminate(state->process, SIGTERM);
    KillRequest *kill = g_new0(KillRequest, 1);
    kill->name = g_strdup(name);
    kill->pids = pids;
    g_timeout_add(200, kill_escalate, kill);
    return TRUE;
}

static void kill_all_profiles(gpointer user_data)
{
    App *app = user_data;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->launches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LaunchState *state = value;
        state->stopping = TRUE;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) pkill = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error, "pkill", "-9", "sober", NULL);
    if (!pkill) {
        g_printerr("Could not stop the clients: %s\n", error ? error->message : "pkill is unavailable");
        return;
    }
    g_subprocess_wait(pkill, NULL, NULL);
}

static gboolean save_raknet(gboolean enabled, GError **error, gpointer user_data)
{
    (void)user_data;
    return raknet_settings_save(enabled, error);
}

static gboolean save_profiles_options(const ProfilesSettings *options,
                                      GError **error, gpointer user_data)
{
    App *app = user_data;
    ProfilesSettings next;
    profiles_settings_copy(&next, options);
    if (!profiles_settings_save(&next, error)) {
        profiles_settings_clear(&next);
        return FALSE;
    }
    profiles_settings_clear(&app->profiles_options);
    app->profiles_options = next;
    arm_restart_timer(app);
    return TRUE;
}

static void execute_script(gpointer user_data)
{
    App *app = user_data;
    GtkTextBuffer *buffer = window_get_buffer(app->window);
    if (!buffer || !app->socket_paths || !app->socket_paths[0])
        return;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    g_autofree char *source = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    g_autofree char *name = g_strdup(window_get_script_name(app->window));
    g_auto(GStrv) targets = g_strdupv(app->socket_paths);
    for (guint i = 0; targets[i]; ++i) {
        const char *failure = NULL;
        SocketClientSendResult result = sessions_can_execute(app->sessions, targets[i])
            ? sessions_execute(app->sessions, targets[i], name, source) : SOCKET_CLIENT_NOT_READY;
        switch (result) {
            case SOCKET_CLIENT_QUEUED: continue;
            case SOCKET_CLIENT_NOT_READY: failure = "Client is not ready; script was not sent."; break;
            case SOCKET_CLIENT_TOO_LARGE: failure = "Script is too large."; break;
            case SOCKET_CLIENT_BACKPRESSURE: failure = "Send queue is full — try again."; break;
            case SOCKET_CLIENT_IDS_EXHAUSTED: failure = "Restart to reset script IDs."; break;
        }
        window_append_output(app->window, targets[i], "error", failure);
        g_printerr("%s: %s\n", targets[i], failure);
    }
}

static void window_destroyed(GtkWidget *window, gpointer user_data)
{
    (void)window;
    App *app = user_data;
    app->window = NULL;
    g_clear_pointer(&app->tray, tray_free);
    if (app->discovery) {
        g_source_remove(app->discovery);
        app->discovery = 0;
    }
    if (app->update_timer) {
        g_source_remove(app->update_timer);
        app->update_timer = 0;
    }
    if (app->scan_timer) {
        g_source_remove(app->scan_timer);
        app->scan_timer = 0;
    }
    if (app->restart_timer) {
        g_source_remove(app->restart_timer);
        app->restart_timer = 0;
    }
    g_clear_pointer(&app->restart_batch, restart_batch_free);
    if (app->account_scan) g_cancellable_cancel(app->account_scan);
    g_clear_pointer(&app->sessions, sessions_free);
}

static void free_app(gpointer user_data)
{
    App *app = user_data;
    window_destroyed(NULL, app);
    g_free(app->scripts_path);
    g_free(app->launcher_path);
    g_strfreev(app->socket_paths);
    g_strfreev(app->scanned_sockets);
    g_clear_object(&app->account_scan);
    g_hash_table_unref(app->account_attempts);
    profiles_settings_clear(&app->profiles_options);
    g_hash_table_unref(app->launches);
    g_hash_table_unref(app->crashes);
    g_hash_table_unref(app->consoles);
    g_free(app);
}

static void tray_activate_app(gpointer user_data)
{
    App *app = user_data;
    if (app->window) gtk_window_present(app->window);
}

static void tray_quit_app(gpointer user_data)
{
    App *app = user_data;
    if (app->window) gtk_window_destroy(app->window);
}

static gboolean window_close_requested(GtkWindow *window, gpointer user_data)
{
    App *app = user_data;
    if (!app->tray) return FALSE;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void startup(GtkApplication *application, gpointer user_data)
{
    (void)application;
    (void)user_data;
    style_install(gdk_display_get_default());
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    GtkWindow *existing = gtk_application_get_active_window(application);
    if (existing) {
        gtk_window_present(existing);
        return;
    }

    App *app = g_new0(App, 1);
    app->launches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, launch_state_free);
    app->crashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, crash_record_free);
    app->consoles = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_console_view);
    app->account_attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->account_scan = g_cancellable_new();
    g_autoptr(GError) settings_error = NULL;
    app->scripts_path = scripts_settings_load(&settings_error);
    if (!app->scripts_path) {
        g_printerr("Could not load script folder preference: %s\n", settings_error->message);
        app->scripts_path = scripts_directory_default();
    }
    WindowHandlers window_handlers = {
        .execute = execute_script, .open_script = open_script, .tab_open = open_tab_externally,
        .active_script_changed = update_execute,
        .editor_settings_changed = save_editor_settings, .restore_tabs_changed = save_restore_tabs,
        .output_changed = save_output, .single_log_changed = save_single_log,
        .launcher_changed = save_launcher, .scripts_changed = save_scripts_path,
        .open_with_changed = save_open_with, .profiles_changed = save_profiles_options,
        .restart_profiles = restart_profiles_now,
        .raknet_changed = save_raknet, .kill_profile = kill_profile,
        .kill_all_profiles = kill_all_profiles,
        .check_updates = check_updates_clicked,
        .socket_selected = select_socket, .launch_profile = launch_profile,
        .launch_all_profiles = launch_all_profiles, .profile_running = profile_running,
        .appearance_changed = save_appearance,
        .settings_imported = settings_imported
    };
    app->window = window_new(application, app->scripts_path, &window_handlers, app);
    apply_settings(app);
    window_set_status(app->window, "Disconnected", "Waiting for a socket to become available.", FALSE);
    g_object_set_data_full(G_OBJECT(app->window), "app", app, free_app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(window_destroyed), app);
    app->tray = tray_new(app->window, tray_activate_app, tray_quit_app, app);
    if (app->tray)
        g_signal_connect(app->window, "close-request", G_CALLBACK(window_close_requested), app);
    gboolean restore_tabs;
    g_clear_error(&settings_error);
    if (!session_settings_load(&restore_tabs, &settings_error))
        g_printerr("Could not load session preference: %s\n", settings_error->message);
    g_autoptr(GError) session_error = NULL;
    if (!window_start_session(app->window, restore_tabs, &session_error)) {
        g_printerr("Could not restore tabs: %s\n", session_error->message);
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not restore previous tabs");
        g_autofree char *detail = g_strdup_printf("%s\nThe existing session file has been left unchanged.", session_error->message);
        gtk_alert_dialog_set_detail(alert, detail);
        gtk_alert_dialog_show(alert, app->window);
    }
    SessionHandlers session_handlers = {
        .state_changed = socket_state_changed, .output_changed = output_state_changed,
        .entry = output_entry, .result = script_result,
        .console_show = console_show, .console_title = console_title, .console_clear = console_clear,
        .console_entry = console_entry, .console_input = console_input
    };
    app->sessions = sessions_new(&session_handlers, app);
    sessions_set_output_enabled(app->sessions, app->redirect_output);

    refresh_profiles(app);
    app->discovery = g_timeout_add_seconds(1, refresh_profiles, app);
    app->scan_timer = g_timeout_add(300, scan_startup, app);
    arm_restart_timer(app);
    app->update_timer = g_timeout_add_seconds(2, check_updates_startup, app);
    gtk_window_present(app->window);
    if (!launcher_configured(app->launcher_path))
        prompt_missing_launcher(app);
}

int app_run(int argc, char **argv)
{
    GtkApplication *application = gtk_application_new(
        "org.relapse.RelapseGtk", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
