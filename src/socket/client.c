#include "client.h"
#include "transport.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct SocketClient {
    RelapseSocket *transport;
    SocketClientHandlers handlers;
    gpointer user_data;
    GMainContext *context;
    GSource *reconnect_source;
    char *path;
    SocketClientState state;
    gboolean hello_received;
    gint64 session;
    gint64 next_script_id;
};

static void set_state(SocketClient *client, SocketClientState state, const char *detail)
{
    client->state = state;
    if (client->handlers.state_changed)
        client->handlers.state_changed(state, detail, client->user_data);
}

static void report_error(SocketClient *client, const char *detail)
{
    if (client->handlers.error)
        client->handlers.error(detail, client->user_data);
}

static gboolean has_type(JsonObject *object, const char *name, GType type)
{
    JsonNode *node = json_object_get_member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == type;
}

static const char *string_member(JsonObject *object, const char *name)
{
    return has_type(object, name, G_TYPE_STRING) ? json_object_get_string_member(object, name) : NULL;
}

static void on_message(RelapseSocket *socket, GBytes *message, gpointer user_data)
{
    SocketClient *client = user_data;
    gsize size;
    const char *data = g_bytes_get_data(message, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!size || memchr(data, '\0', size) || !g_utf8_validate(data, size, NULL) ||
        !json_parser_load_from_data(parser, data, size, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        goto invalid_message;

    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const char *type = string_member(object, "type");
    if (!type || (!client->hello_received && strcmp(type, "hello") != 0))
        goto invalid_message;

    if (strcmp(type, "hello") == 0 || strcmp(type, "status") == 0) {
        if ((strcmp(type, "hello") == 0 && client->hello_received) ||
            !has_type(object, "ready", G_TYPE_BOOLEAN) || !has_type(object, "session", G_TYPE_INT64))
            goto invalid_message;
        gboolean ready = json_object_get_boolean_member(object, "ready");
        gint64 session = json_object_get_int_member(object, "session");
        if (session < 0 || (ready && session == 0))
            goto invalid_message;
        client->hello_received = TRUE;
        client->session = session;
        set_state(client, ready ? SOCKET_CLIENT_READY : SOCKET_CLIENT_WAITING, NULL);
    } else if (strcmp(type, "exec_result") == 0) {
        const char *phase = string_member(object, "phase");
        const char *detail = string_member(object, "detail");
        if (!has_type(object, "script_id", G_TYPE_INT64) || !phase ||
            (strcmp(phase, "submitted") != 0 && strcmp(phase, "error") != 0))
            goto invalid_message;
        if (client->handlers.result)
            client->handlers.result(json_object_get_int_member(object, "script_id"),
                                    strcmp(phase, "submitted") == 0, detail, client->user_data);
    } else if (strcmp(type, "log") == 0) {
        const char *level = string_member(object, "level");
        const char *text = string_member(object, "text");
        if (!level || !text)
            goto invalid_message;
        if (client->handlers.log)
            client->handlers.log(level, text, client->user_data);
    } else if (strcmp(type, "error") == 0) {
        const char *detail = string_member(object, "detail");
        report_error(client, detail ? detail : "Request failed");
    }
    return;

invalid_message:
    relapse_socket_close(socket);
    report_error(client, "Invalid executor message");
}

static void on_close(RelapseSocket *socket, RelapseSocketCloseReason reason,
                     const char *detail, gpointer user_data)
{
    (void)socket;
    (void)reason;
    SocketClient *client = user_data;
    client->hello_received = FALSE;
    client->session = 0;
    set_state(client, SOCKET_CLIENT_DISCONNECTED, detail);
}

static gboolean connect_socket(gpointer user_data)
{
    SocketClient *client = user_data;
    if (relapse_socket_get_state(client->transport) == RELAPSE_SOCKET_CLOSED) {
        g_autoptr(GError) error = NULL;
        client->hello_received = FALSE;
        client->session = 0;
        if (relapse_socket_connect(client->transport, client->path, "@main", &error))
            set_state(client, SOCKET_CLIENT_CONNECTING, NULL);
        else
            set_state(client, SOCKET_CLIENT_DISCONNECTED, NULL);
    }
    return G_SOURCE_CONTINUE;
}

SocketClient *socket_client_new(const char *path, const SocketClientHandlers *handlers, gpointer user_data)
{
    SocketClient *client = g_new0(SocketClient, 1);
    if (handlers)
        client->handlers = *handlers;
    client->user_data = user_data;
    client->path = g_strdup(path);
    client->context = g_main_context_ref_thread_default();
    client->next_script_id = 1;
    RelapseSocketHandlers transport_handlers = {.onMessage = on_message, .onClose = on_close};
    client->transport = relapse_socket_new(&transport_handlers, client);
    return client;
}

void socket_client_start(SocketClient *client)
{
    if (client->reconnect_source)
        return;
    client->reconnect_source = g_timeout_source_new_seconds(1);
    g_source_set_callback(client->reconnect_source, connect_socket, client, NULL);
    g_source_attach(client->reconnect_source, client->context);
    connect_socket(client);
}

gboolean socket_client_is_ready(const SocketClient *client)
{
    return client && client->state == SOCKET_CLIENT_READY &&
        relapse_socket_get_state(client->transport) == RELAPSE_SOCKET_OPEN;
}

gint64 socket_client_get_session(const SocketClient *client)
{
    return socket_client_is_ready(client) ? client->session : 0;
}

GPid socket_client_get_peer_pid(const SocketClient *client)
{
    return client ? relapse_socket_get_peer_pid(client->transport) : 0;
}

const char *socket_client_get_path(const SocketClient *client)
{
    return client->path;
}

SocketClientSendResult socket_client_execute(SocketClient *client, const char *name, const char *source,
                                             gint64 *request_id)
{
    if (!socket_client_is_ready(client))
        return SOCKET_CLIENT_NOT_READY;
    if (client->next_script_id == G_MAXINT64)
        return SOCKET_CLIENT_IDS_EXHAUSTED;

    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "op", "execute");
    json_object_set_int_member(object, "script_id", client->next_script_id);
    json_object_set_int_member(object, "session", client->session);
    json_object_set_string_member(object, "name", name && strlen(name) <= 255 ? name : "editor.luau");
    json_object_set_string_member(object, "source", source);
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, object);
    g_autofree char *json = json_to_string(root, FALSE);
    g_autoptr(GBytes) message = g_bytes_new(json, strlen(json));
    switch (relapse_socket_send(client->transport, message)) {
        case RELAPSE_SOCKET_QUEUED:
            if (request_id)
                *request_id = client->next_script_id;
            ++client->next_script_id;
            return SOCKET_CLIENT_QUEUED;
        case RELAPSE_SOCKET_TOO_LARGE:
            return SOCKET_CLIENT_TOO_LARGE;
        case RELAPSE_SOCKET_BACKPRESSURE:
            return SOCKET_CLIENT_BACKPRESSURE;
        case RELAPSE_SOCKET_SEND_CLOSED:
            return SOCKET_CLIENT_NOT_READY;
    }
    return SOCKET_CLIENT_NOT_READY;
}

void socket_client_free(SocketClient *client)
{
    if (!client)
        return;
    if (client->reconnect_source) {
        g_source_destroy(client->reconnect_source);
        g_source_unref(client->reconnect_source);
    }
    relapse_socket_free(client->transport);
    g_main_context_unref(client->context);
    g_free(client->path);
    g_free(client);
}
