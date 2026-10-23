#include "app.h"
#include "socket/sessions.h"
#include "ui/style.h"
#include "ui/window.h"
#include "settings/session.h"
#include "settings/output.h"
#include "socket/bridge.h"
#include "profiles/store.h"
#include "settings/launcher.h"

typedef struct {
    GtkWindow *window;
    Sessions *sessions;
    char *scripts_path;
    char *launcher_path;
    char *socket_path;
    guint discovery;
    gboolean redirect_output;
    GHashTable *launches;
} App;

static gboolean refresh_profiles(gpointer user_data);

static void update_execute(gpointer user_data)
{
    App *app = user_data;
    window_set_execute_enabled(app->window,
        sessions_can_execute(app->sessions, app->socket_path) &&
        window_get_buffer(app->window) != NULL);
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
    switch (sessions_get_state(app->sessions, app->socket_path)) {
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
    window_set_output_target(app->window, app->socket_path);
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
    switch (sessions_execute(app->sessions, app->socket_path, window_get_script_name(app->window), source)) {
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
        .execute = execute_script, .open_script = open_script, .active_script_changed = update_execute,
        .editor_settings_changed = save_editor_settings, .restore_tabs_changed = save_restore_tabs,
        .output_changed = save_output, .launcher_changed = save_launcher,
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
