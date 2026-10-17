#include "controller.h"
#include "socket/transport.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

struct OutputController {
    SocketClient *client;
    RelapseSocket *channel;
    GMainContext *context;
    GSource *timer;
    OutputHandlers handlers;
    gpointer user_data;
    GHashTable *requests;
    gint64 request_id, session, deadline;
    OutputState state;
    gboolean enabled;
};

static void set_state(OutputController *output, OutputState state, const char *detail)
{
    output->state = state;
    if (output->handlers.state_changed)
        output->handlers.state_changed(state, detail, output->user_data);
}

static void start_timer(OutputController *output);

static void cancel_timer(OutputController *output)
{
    if (output->timer) {
        g_source_destroy(output->timer);
        g_clear_pointer(&output->timer, g_source_unref);
    }
}

static void stop(OutputController *output)
{
    cancel_timer(output);
    g_clear_pointer(&output->channel, relapse_socket_free);
    output->request_id = 0;
}

static void fail(OutputController *output, const char *detail)
{
    stop(output);
    set_state(output, OUTPUT_FAILED, detail);
}

static void connected(RelapseSocket *socket, gpointer user_data)
{
    OutputController *output = user_data;
    static const char subscribe[] = "{\"op\":\"subscribe\",\"version\":1}";
    g_autoptr(GBytes) message = g_bytes_new_static(subscribe, sizeof(subscribe) - 1);
    if (relapse_socket_send(socket, message) != RELAPSE_SOCKET_QUEUED)
        fail(output, "Could not start output capture. Toggle redirection off and on to retry.");
}

static void closed(RelapseSocket *socket, RelapseSocketCloseReason reason, const char *detail, gpointer user_data)
{
    (void)socket; (void)reason; (void)detail;
    OutputController *output = user_data;
    if (output->state == OUTPUT_ACTIVE) {
        output->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        set_state(output, OUTPUT_CONNECTING, "Reconnecting…");
        start_timer(output);
    }
}

static gboolean member_is(JsonObject *object, const char *key, GType type)
{
    JsonNode *node = json_object_get_member(object, key);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == type;
}

static const char *string_member(JsonObject *object, const char *key)
{
    return member_is(object, key, G_TYPE_STRING) ? json_object_get_string_member(object, key) : NULL;
}

static gboolean integer_member(JsonObject *object, const char *key, gint32 minimum,
                               gint32 maximum, gint64 *result)
{
    gint64 value;
    if (member_is(object, key, G_TYPE_INT64)) {
        value = json_object_get_int_member(object, key);
    } else if (member_is(object, key, G_TYPE_DOUBLE)) {
        double number = json_object_get_double_member(object, key);
        if (!isfinite(number) || number < minimum || number > maximum)
            return FALSE;
        value = (gint64)number;
        if (number != (double)value)
            return FALSE;
    } else {
        return FALSE;
    }
    if (value < minimum || value > maximum)
        return FALSE;
    if (result)
        *result = value;
    return TRUE;
}

static gboolean valid_level(const char *level)
{
    return level && (g_str_equal(level, "print") || g_str_equal(level, "warn") ||
                     g_str_equal(level, "error") || g_str_equal(level, "info"));
}

static void dropped(OutputController *output, gint64 count)
{
    if (count > 0 && output->handlers.entry) {
        g_autofree char *text = g_strdup_printf("%" G_GINT64_FORMAT " messages dropped: output arrived too quickly.", count);
        output->handlers.entry("warn", text, output->user_data);
    }
}

static void message_received(RelapseSocket *socket, GBytes *message, gpointer user_data)
{
    (void)socket;
    OutputController *output = user_data;
    gsize size;
    const char *data = g_bytes_get_data(message, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!size || size > 1024 * 1024 || memchr(data, 0, size) || !g_utf8_validate(data, size, NULL) ||
        !json_parser_load_from_data(parser, data, size, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        goto invalid;
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const char *type = string_member(object, "type");
    if (!type) goto invalid;
    if (g_str_equal(type, "subscribed")) {
        if (output->state != OUTPUT_CONNECTING || !integer_member(object, "version", 1, 1, NULL)) goto invalid;
        cancel_timer(output);
        set_state(output, OUTPUT_ACTIVE, "Capturing");
    } else if (g_str_equal(type, "failed")) {
        const char *detail = string_member(object, "detail");
        if (!detail || strlen(detail) > 4096) goto invalid;
        fail(output, detail);
    } else if (g_str_equal(type, "output")) {
        JsonNode *node = json_object_get_member(object, "entries");
        gint64 lost;
        if (output->state != OUTPUT_ACTIVE || !node || !JSON_NODE_HOLDS_ARRAY(node) ||
            !integer_member(object, "dropped", 0, G_MAXINT32, &lost)) goto invalid;
        JsonArray *entries = json_node_get_array(node);
        guint length = json_array_get_length(entries);
        if (!length || length > 32) goto invalid;
        for (guint i = 0; i < length; ++i) {
            JsonNode *entry = json_array_get_element(entries, i);
            if (!JSON_NODE_HOLDS_OBJECT(entry)) goto invalid;
            JsonObject *row = json_node_get_object(entry);
            const char *text = string_member(row, "text");
            if (!valid_level(string_member(row, "level")) || !text || strlen(text) > 4096) goto invalid;
        }
        for (guint i = 0; i < length; ++i) {
            JsonObject *row = json_array_get_object_element(entries, i);
            if (output->handlers.entry)
                output->handlers.entry(string_member(row, "level"), string_member(row, "text"), output->user_data);
        }
        dropped(output, lost);
    } else if (g_str_equal(type, "dropped")) {
        gint64 count;
        if (output->state != OUTPUT_ACTIVE || !integer_member(object, "count", 1, G_MAXINT32, &count)) goto invalid;
        dropped(output, count);
    } else goto invalid;
    return;
invalid:
    fail(output, "Invalid output message. Toggle redirection off and on to retry.");
}

static gboolean tick(gpointer user_data)
{
    OutputController *output = user_data;
    if (output->state == OUTPUT_ACTIVE)
        return G_SOURCE_CONTINUE;
    if (g_get_monotonic_time() >= output->deadline) {
        fail(output, "Output setup timed out. Toggle redirection off and on to retry.");
        return G_SOURCE_REMOVE;
    }
    if (relapse_socket_get_state(output->channel) == RELAPSE_SOCKET_CLOSED)
        relapse_socket_connect(output->channel, socket_client_get_path(output->client), "relapsegtk.output.v1", NULL);
    return G_SOURCE_CONTINUE;
}

static void start_timer(OutputController *output)
{
    output->timer = g_timeout_source_new(250);
    g_source_set_callback(output->timer, tick, output, NULL);
    g_source_attach(output->timer, output->context);
}

static void start(OutputController *output)
{
    stop(output);
    if (!output->enabled) {
        set_state(output, OUTPUT_OFF, "Off");
        return;
    }
    if (!output->session) {
        set_state(output, OUTPUT_WAITING, "Waiting for a game");
        return;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) resource = g_resources_lookup_data("/org/relapse/RelapseGtk/scripts/output.luau",
                                                       G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!resource) {
        fail(output, error->message);
        return;
    }
    gsize length;
    const char *data = g_bytes_get_data(resource, &length);
    g_autofree char *source = g_strndup(data, length);
    if (socket_client_execute(output->client, "relapsegtk-output.luau", source, &output->request_id) != SOCKET_CLIENT_QUEUED) {
        fail(output, "Could not queue output setup. Toggle redirection off and on to retry.");
        return;
    }
    if (g_hash_table_size(output->requests) >= 256)
        g_hash_table_remove_all(output->requests);
    gint64 *id = g_new(gint64, 1);
    *id = output->request_id;
    g_hash_table_add(output->requests, id);
    RelapseSocketHandlers handlers = {.onConnect = connected, .onMessage = message_received, .onClose = closed};
    output->channel = relapse_socket_new(&handlers, output);
    output->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    set_state(output, OUTPUT_CONNECTING, "Starting capture…");
    start_timer(output);
    tick(output);
}

OutputController *output_controller_new(SocketClient *client, const OutputHandlers *handlers, gpointer user_data)
{
    OutputController *output = g_new0(OutputController, 1);
    output->client = client;
    output->context = g_main_context_ref_thread_default();
    output->requests = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    if (handlers) output->handlers = *handlers;
    output->user_data = user_data;
    return output;
}

void output_controller_set_enabled(OutputController *output, gboolean enabled)
{
    enabled = !!enabled;
    if (output->enabled == enabled) return;
    output->enabled = enabled;
    output->session = socket_client_get_session(output->client);
    start(output);
}

void output_controller_sync(OutputController *output)
{
    gint64 session = socket_client_get_session(output->client);
    if (output->session == session) return;
    output->session = session;
    start(output);
}

gboolean output_controller_result(OutputController *output, gint64 id, gboolean submitted, const char *detail)
{
    if (!g_hash_table_remove(output->requests, &id)) return FALSE;
    if (id == output->request_id && !submitted)
        fail(output, detail ? detail : "Output setup failed.");
    return TRUE;
}

void output_controller_free(OutputController *output)
{
    if (!output) return;
    stop(output);
    g_hash_table_unref(output->requests);
    g_main_context_unref(output->context);
    g_free(output);
}
