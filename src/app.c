#include "app.h"
#include "socket/client.h"
#include "ui/style.h"
#include "ui/window.h"
#include "settings/session.h"
#include "settings/output.h"
#include "settings/mcp.h"
#include "output/controller.h"
#include "mcp/server.h"

typedef struct {
    GtkWindow *window;
    SocketClient *socket;
    OutputController *output;
    OutputState output_state;
    McpServer *mcp;
    char *scripts_path;
} App;

static void update_execute(gpointer user_data)
{
    App *app = user_data;
    window_set_execute_enabled(app->window,
        socket_client_is_ready(app->socket) && app->output_state != OUTPUT_CONNECTING &&
        window_get_buffer(app->window) != NULL);
}

static void output_state_changed(OutputState state, const char *detail, gpointer user_data)
{
    App *app = user_data;
    app->output_state = state;
    window_set_output_status(app->window, detail);
    if (state == OUTPUT_ACTIVE)
        window_append_output(app->window, "info", "Output capture started.");
    else if (state == OUTPUT_FAILED)
        window_append_output(app->window, "error", detail);
    update_execute(app);
}

static void output_entry(const char *level, const char *text, gpointer user_data)
{
    App *app = user_data;
    window_append_output(app->window, level, text);
    if (app->mcp) mcp_server_record_log(app->mcp, level, text);
}

static void mcp_execute_source(const char *name, const char *source, gpointer user_data)
{
    App *app = user_data;
    if (!app->socket || !socket_client_is_ready(app->socket)) {
        if (app->mcp) mcp_server_record_log(app->mcp, "error", "mcp execute: executor not ready");
        return;
    }
    switch (socket_client_execute(app->socket, name ? name : "mcp", source, NULL)) {
        case SOCKET_CLIENT_QUEUED:
            if (app->mcp) mcp_server_record_log(app->mcp, "info", "mcp execute: submitted");
            break;
        case SOCKET_CLIENT_NOT_READY:
            if (app->mcp) mcp_server_record_log(app->mcp, "error", "mcp execute: executor not ready");
            break;
        case SOCKET_CLIENT_TOO_LARGE:
            if (app->mcp) mcp_server_record_log(app->mcp, "error", "mcp execute: script too large");
            break;
        case SOCKET_CLIENT_BACKPRESSURE:
            if (app->mcp) mcp_server_record_log(app->mcp, "error", "mcp execute: send queue full");
            break;
        case SOCKET_CLIENT_IDS_EXHAUSTED:
            if (app->mcp) mcp_server_record_log(app->mcp, "error", "mcp execute: script IDs exhausted");
            break;
    }
}

static gboolean save_mcp(const McpSettings *settings, GError **error, gpointer user_data)
{
    App *app = user_data;
    McpServerConfig cfg = {
        .port = settings->port,
        .expose_execute = settings->expose_execute,
        .expose_logs = settings->expose_logs,
        .token = settings->token,
    };
    if (app->mcp && !mcp_server_apply(app->mcp, &cfg, error))
        return FALSE;
    return mcp_settings_save(settings, error);
}

static gboolean save_output(gboolean enabled, GError **error, gpointer user_data)
{
    App *app = user_data;
    output_controller_set_enabled(app->output, enabled);
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

static void socket_state_changed(SocketClientState state, const char *detail, gpointer user_data)
{
    App *app = user_data;
    output_controller_sync(app->output);
    update_execute(app);
    switch (state) {
        case SOCKET_CLIENT_READY:
            window_set_status(app->window, "Ready", "Connected. Ready to execute scripts.", TRUE);
            break;
        case SOCKET_CLIENT_WAITING:
            window_set_status(app->window, "Not ready", "Connected. Waiting for a game to be ready.", FALSE);
            break;
        case SOCKET_CLIENT_CONNECTING:
        case SOCKET_CLIENT_DISCONNECTED:
            window_set_status(app->window, "Disconnected", "Not connected. Retrying automatically.", FALSE);
            break;
    }
    if (state == SOCKET_CLIENT_DISCONNECTED && detail)
        g_printerr("Disconnected: %s\n", detail);
}

static void script_result(gint64 script_id, gboolean submitted, const char *detail, gpointer user_data)
{
    App *app = user_data;
    if (output_controller_result(app->output, script_id, submitted, detail))
        return;
    if (!submitted)
        window_append_output(app->window, "error", detail ? detail : "Script submission failed.");
    g_print("[script %" G_GINT64_FORMAT "] %s%s%s\n", script_id,
            submitted ? "submitted" : "error", detail ? ": " : "", detail ? detail : "");
}

static void socket_log(const char *level, const char *text, gpointer user_data)
{
    (void)user_data;
    g_print("[%s] %s\n", level, text);
}

static void socket_error(const char *detail, gpointer user_data)
{
    (void)user_data;
    g_printerr("%s\n", detail);
}

static void execute_script(gpointer user_data)
{
    App *app = user_data;
    GtkTextBuffer *buffer = window_get_buffer(app->window);
    if (!buffer || !socket_client_is_ready(app->socket) || app->output_state == OUTPUT_CONNECTING)
        return;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    g_autofree char *source = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    switch (socket_client_execute(app->socket, window_get_script_name(app->window), source, NULL)) {
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
    g_clear_pointer(&app->mcp, mcp_server_free);
    g_clear_pointer(&app->output, output_controller_free);
    g_clear_pointer(&app->socket, socket_client_free);
}

static void free_app(gpointer user_data)
{
    App *app = user_data;
    window_destroyed(NULL, app);
    g_free(app->scripts_path);
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
    app->scripts_path = g_build_filename(g_get_home_dir(), "relapse-scripts-tmpdir", NULL);
    WindowHandlers window_handlers = {
        .execute = execute_script, .open_script = open_script, .active_script_changed = update_execute,
        .editor_settings_changed = save_editor_settings, .restore_tabs_changed = save_restore_tabs,
        .output_changed = save_output, .mcp_changed = save_mcp
    };
    app->window = window_new(application, app->scripts_path, &window_handlers, app);
    EditorSettings settings;
    g_autoptr(GError) settings_error = NULL;
    if (!editor_settings_load(&settings, &settings_error))
        g_printerr("Could not load settings: %s\n", settings_error->message);
    window_set_editor_settings(app->window, &settings);
    SocketClientHandlers socket_handlers = {
        .state_changed = socket_state_changed, .result = script_result,
        .log = socket_log, .error = socket_error
    };
    app->socket = socket_client_new(NULL, &socket_handlers, app);
    OutputHandlers output_handlers = {.state_changed = output_state_changed, .entry = output_entry};
    app->output = output_controller_new(app->socket, &output_handlers, app);
    gboolean redirect;
    g_clear_error(&settings_error);
    if (!output_settings_load(&redirect, &settings_error))
        g_printerr("Could not load output preference: %s\n", settings_error->message);
    window_set_output_enabled(app->window, redirect);
    output_controller_set_enabled(app->output, redirect);
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
    app->mcp = mcp_server_new(mcp_execute_source, app);
    McpSettings mcp = {0};
    mcp_settings_defaults(&mcp);
    g_clear_error(&settings_error);
    if (!mcp_settings_load(&mcp, &settings_error))
        g_printerr("Could not load MCP settings: %s\n", settings_error->message);
    window_set_mcp_settings(app->window, &mcp);
    McpServerConfig mcp_cfg = {
        .port = mcp.port, .expose_execute = mcp.expose_execute,
        .expose_logs = mcp.expose_logs, .token = mcp.token,
    };
    g_autoptr(GError) mcp_error = NULL;
    if (!mcp_server_apply(app->mcp, &mcp_cfg, &mcp_error))
        g_printerr("Could not start MCP server: %s\n", mcp_error->message);
    mcp_settings_clear(&mcp);

    socket_client_start(app->socket);
    gtk_window_present(app->window);
}

int app_run(int argc, char **argv)
{
    GtkApplication *application = gtk_application_new(
        "org.relapse.RelapseGtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
