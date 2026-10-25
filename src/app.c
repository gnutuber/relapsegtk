#include "app.h"
#include "socket/sessions.h"
#include "ui/style.h"
#include "ui/window.h"
#include "settings/session.h"
#include "settings/output.h"
#include "socket/bridge.h"
#include "profiles/store.h"
#include "settings/launcher.h"
#include "settings/opener.h"
#include "update/check.h"

typedef struct {
    GtkWindow *window;
    Sessions *sessions;
    char *scripts_path;
    char *launcher_path;
    char *socket_path;
    guint discovery;
    guint update_timer;
    gboolean manual_update_check;
    gboolean redirect_output;
    GHashTable *launches;
} App;

static gboolean refresh_profiles(gpointer user_data);

static void update_execute(gpointer user_data)
{
    App *app = user_data;
    gboolean can_execute = app->socket_path && *app->socket_path
        ? sessions_can_execute(app->sessions, app->socket_path)
        : sessions_any_can_execute(app->sessions);
    window_set_execute_enabled(app->window,
        can_execute && window_get_buffer(app->window) != NULL);
}

static void output_state_changed(const char *path, OutputState state, const char *detail, gpointer user_data)
{
    App *app = user_data;
    window_set_output_status(app->window, path, detail,
        sessions_is_active(app->sessions, path));
    if (state == OUTPUT_FAILED)
        window_append_output(app->window, path, "error", detail);
    update_execute(app);
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
    if (opener.mode != OPENER_EDITOR && opener.mode != OPENER_NOTHING &&
        !opener_open(file, &opener, &error)) {
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
} UpdateChoice;

static void update_download_finished(const char *path, const char *error, gpointer user_data)
{
    UpdateChoice *choice = user_data;
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

static void update_choice_finished(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    UpdateChoice *choice = user_data;
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
            UpdateChoice *download = g_new0(UpdateChoice, 1);
            g_weak_ref_init(&download->window, window);
            download->url = g_strdup(choice->url);
            download->name = g_strdup(choice->name);
            g_autofree char *directory = update_download_directory();
            update_download_async(download->url, download->name, directory,
                                  update_download_finished, download);
        }
    }
    g_weak_ref_clear(&choice->window);
    g_free(choice->url);
    g_free(choice->name);
    g_free(choice);
}

static void update_checked(const UpdateInfo *info, const char *error, gpointer user_data)
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
    UpdateChoice *choice = g_new0(UpdateChoice, 1);
    g_weak_ref_init(&choice->window, app->window);
    choice->url = g_strdup(has_appimage ? info->download_url : info->release_url);
    choice->name = g_strdup(info->asset_name);
    choice->is_page = !has_appimage;
    gtk_alert_dialog_choose(alert, app->window, NULL, update_choice_finished, choice);
}

static void check_updates(gpointer user_data)
{
    App *app = user_data;
    app->manual_update_check = TRUE;
    update_check_async(update_checked, app);
}

static gboolean check_updates_on_startup(gpointer user_data)
{
    App *app = user_data;
    app->update_timer = 0;
    update_check_async(update_checked, app);
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

static gboolean save_restore_tabs(gboolean enabled, GError **error, gpointer user_data)
{
    (void)user_data;
    return session_settings_save(enabled, error);
}

static void update_connection_status(App *app)
{
    SocketClientState state = app->socket_path && *app->socket_path
        ? sessions_get_state(app->sessions, app->socket_path)
        : sessions_aggregate_state(app->sessions);
    switch (state) {
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
    if (state != SOCKET_CLIENT_READY)
        window_set_output_status(app->window, path,
            !active ? "Closed" : state == SOCKET_CLIENT_WAITING ? "Waiting for a game" : "Reconnecting…", active);
    if (g_strcmp0(path, app->socket_path) == 0) update_connection_status(app);
}

static void script_result(const char *path, gint64 script_id, gboolean submitted, const char *detail, gpointer user_data)
{
    (void)script_id;
    App *app = user_data;
    if (!submitted)
        window_append_output(app->window, path, "error", detail ? detail : "Script submission failed.");
}

static void select_socket(const char *path, gpointer user_data)
{
    App *app = user_data;
    if (g_strcmp0(path, app->socket_path) == 0) return;
    g_autofree char *selected = g_strdup(path);
    g_free(app->socket_path);
    app->socket_path = g_steal_pointer(&selected);
    window_set_output_target(app->window, app->socket_path ? app->socket_path : "");
    update_connection_status(app);
    refresh_profiles(app);
}

static gboolean refresh_profiles(gpointer user_data)
{
    App *app = user_data;
    g_auto(GStrv) names = profiles_list();
    g_auto(GStrv) sockets = profile_sockets_list("/tmp");
    sessions_update(app->sessions, (const char *const *)sockets);
    if ((!app->socket_path && sockets[0]) || (app->socket_path && !sockets[0])) {
        select_socket(sockets[0], app);
        return G_SOURCE_CONTINUE;
    }
    window_update_profiles(app->window, (const char *const *)names,
        (const char *const *)sockets, app->socket_path);
    update_connection_status(app);
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

static gboolean profile_running(const char *name, gpointer user_data)
{
    App *app = user_data;
    return g_hash_table_contains(app->launches, name) || profile_is_running(name);
}

typedef struct {
    GWeakRef window;
    char *name;
} Launch;

static void launch_finished(GObject *process, GAsyncResult *result, gpointer user_data)
{
    Launch *launch = user_data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_subprocess_wait_check_finish(G_SUBPROCESS(process), result, &error);
    g_autoptr(GtkWindow) window = g_weak_ref_get(&launch->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (app && app->window) {
        g_hash_table_remove(app->launches, launch->name);
        refresh_profiles(app);
        if (!success) {
            g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Profile %s exited with an error", launch->name);
            gtk_alert_dialog_set_detail(alert, error->message);
            gtk_alert_dialog_show(alert, window);
        }
    }
    g_weak_ref_clear(&launch->window);
    g_free(launch->name);
    g_free(launch);
}

static void launch_profile(const char *name, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;
    if (!app->launcher_path || !*app->launcher_path) {
        window_show_settings(app->window);
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Select a launcher first");
        gtk_alert_dialog_set_detail(alert, "Choose your launcher executable under Profiles in Settings, then launch again.");
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    if (g_hash_table_contains(app->launches, name)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Profile %s is already running", name);
        gtk_alert_dialog_set_detail(alert, "Select its socket in the titlebar to connect when it becomes available.");
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    GSubprocess *process = profile_launch(app->launcher_path, name, &error);
    if (!process) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not launch Profile %s", name);
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, app->window);
        return;
    }
    g_hash_table_insert(app->launches, g_strdup(name), process);
    Launch *launch = g_new0(Launch, 1);
    g_weak_ref_init(&launch->window, app->window);
    launch->name = g_strdup(name);
    g_subprocess_wait_check_async(process, NULL, launch_finished, launch);
    refresh_profiles(app);
}

static void execute_script(gpointer user_data)
{
    App *app = user_data;
    GtkTextBuffer *buffer = window_get_buffer(app->window);
    if (!buffer || !sessions_can_execute(app->sessions, app->socket_path))
        return;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    g_autofree char *source = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    const char *name = window_get_script_name(app->window);
    SocketClientSendResult sent;
    if (app->socket_path && *app->socket_path) {
        sent = sessions_execute(app->sessions, app->socket_path, name, source);
    } else {
        guint queued = 0;
        sent = sessions_execute_all(app->sessions, name, source, &queued);
        if (sent == SOCKET_CLIENT_QUEUED && queued == 0)
            sent = SOCKET_CLIENT_NOT_READY;
    }
    switch (sent) {
        case SOCKET_CLIENT_QUEUED:
            break;
        case SOCKET_CLIENT_NOT_READY:
            g_printerr("Executor is not ready\n");
            break;
        case SOCKET_CLIENT_TOO_LARGE:
            g_printerr("Script is too large\n");
            break;
        case SOCKET_CLIENT_BACKPRESSURE:
            g_printerr("Send queue is full — try again\n");
            break;
        case SOCKET_CLIENT_IDS_EXHAUSTED:
            g_printerr("Restart to reset script IDs\n");
            break;
    }
}

static void window_destroyed(GtkWidget *window, gpointer user_data)
{
    (void)window;
    App *app = user_data;
    app->window = NULL;
    if (app->discovery) {
        g_source_remove(app->discovery);
        app->discovery = 0;
    }
    if (app->update_timer) {
        g_source_remove(app->update_timer);
        app->update_timer = 0;
    }
    g_clear_pointer(&app->sessions, sessions_free);
}

static void free_app(gpointer user_data)
{
    App *app = user_data;
    window_destroyed(NULL, app);
    g_free(app->scripts_path);
    g_free(app->launcher_path);
    g_free(app->socket_path);
    g_hash_table_unref(app->launches);
    g_free(app);
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
    app->launches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    app->scripts_path = g_build_filename(g_get_home_dir(), "relapse-scripts-tmpdir", NULL);
    WindowHandlers window_handlers = {
        .execute = execute_script, .open_script = open_script, .tab_open = open_tab_externally,
        .active_script_changed = update_execute,
        .editor_settings_changed = save_editor_settings, .restore_tabs_changed = save_restore_tabs,
        .output_changed = save_output, .launcher_changed = save_launcher,
        .open_with_changed = save_open_with, .check_updates = check_updates,
        .socket_selected = select_socket, .launch_profile = launch_profile, .profile_running = profile_running
    };
    app->window = window_new(application, app->scripts_path, &window_handlers, app);
    EditorSettings settings;
    g_autoptr(GError) settings_error = NULL;
    if (!editor_settings_load(&settings, &settings_error))
        g_printerr("Could not load settings: %s\n", settings_error->message);
    window_set_editor_settings(app->window, &settings);
    gboolean redirect;
    g_clear_error(&settings_error);
    if (!output_settings_load(&redirect, &settings_error))
        g_printerr("Could not load output preference: %s\n", settings_error->message);
    window_set_output_enabled(app->window, redirect);
    app->redirect_output = redirect;
    g_clear_error(&settings_error);
    if (!launcher_settings_load(&app->launcher_path, &settings_error))
        g_printerr("Could not load launcher preference: %s\n", settings_error->message);
    window_set_launcher(app->window, app->launcher_path);
    g_clear_error(&settings_error);
    OpenerSettings opener;
    if (!opener_settings_load(&opener, &settings_error))
        g_printerr("Could not load script opener preference: %s\n", settings_error->message);
    window_set_open_with(app->window, opener.mode, opener.command);
    opener_settings_clear(&opener);
    window_set_status(app->window, "Disconnected", "Waiting for a socket to become available.", FALSE);
    g_object_set_data_full(G_OBJECT(app->window), "app", app, free_app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(window_destroyed), app);
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
        .entry = output_entry, .result = script_result
    };
    app->sessions = sessions_new(&session_handlers, app);
    sessions_set_output_enabled(app->sessions, app->redirect_output);

    refresh_profiles(app);
    app->discovery = g_timeout_add_seconds(1, refresh_profiles, app);
    app->update_timer = g_timeout_add_seconds(2, check_updates_on_startup, app);
    gtk_window_present(app->window);
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
