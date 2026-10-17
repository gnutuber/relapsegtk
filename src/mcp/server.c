#include "server.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define MCP_PROTOCOL_VERSION "2025-06-18"
#define MCP_LOG_RING_CAP 1024
#define MCP_MAX_REQUEST 262144

typedef struct {
    gint64 ts_us;
    char *level;
    char *text;
} LogEntry;

struct McpServer {
    GSocketService *service;
    int active_port;
    gboolean expose_execute;
    gboolean expose_logs;
    char *token;
    McpExecuteFunc execute;
    gpointer user_data;
    LogEntry ring[MCP_LOG_RING_CAP];
    guint ring_head;
    guint ring_count;
    GMutex ring_mutex;
};

static void log_entry_clear(LogEntry *e)
{
    g_clear_pointer(&e->level, g_free);
    g_clear_pointer(&e->text, g_free);
    e->ts_us = 0;
}

McpServer *mcp_server_new(McpExecuteFunc execute, gpointer user_data)
{
    McpServer *s = g_new0(McpServer, 1);
    s->execute = execute;
    s->user_data = user_data;
    g_mutex_init(&s->ring_mutex);
    return s;
}

void mcp_server_record_log(McpServer *s, const char *level, const char *text)
{
    if (!s || !text) return;
    g_mutex_lock(&s->ring_mutex);
    guint slot = (s->ring_head + s->ring_count) % MCP_LOG_RING_CAP;
    if (s->ring_count == MCP_LOG_RING_CAP) {
        log_entry_clear(&s->ring[s->ring_head]);
        s->ring_head = (s->ring_head + 1) % MCP_LOG_RING_CAP;
        slot = (s->ring_head + s->ring_count - 1) % MCP_LOG_RING_CAP;
    } else {
        s->ring_count++;
    }
    s->ring[slot].ts_us = g_get_real_time();
    s->ring[slot].level = g_strdup(level ? level : "info");
    s->ring[slot].text = g_strdup(text);
    g_mutex_unlock(&s->ring_mutex);
}

typedef struct {
    McpServer *server;
    GInputStream *in;
    GOutputStream *out;
    GString *buf;
} Client;

static void client_free(Client *c)
{
    if (!c) return;
    if (c->buf) g_string_free(c->buf, TRUE);
    g_free(c);
}

static gchar *json_node_to_compact(JsonNode *root)
{
    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, root);
    return json_generator_to_data(gen, NULL);
}

static void write_http(GOutputStream *out, int status, const char *reason,
                       const char *content_type, const char *body, gsize body_len)
{
    g_autofree char *headers = g_strdup_printf(
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n", status, reason, content_type, body_len);
    g_output_stream_write_all(out, headers, strlen(headers), NULL, NULL, NULL);
    if (body_len) g_output_stream_write_all(out, body, body_len, NULL, NULL, NULL);
}

static void reply_json(GOutputStream *out, int status, const char *reason, JsonNode *root)
{
    g_autofree char *body = json_node_to_compact(root);
    write_http(out, status, reason, "application/json", body, strlen(body));
}

static void reply_error(GOutputStream *out, int status, const char *reason, const char *msg)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "error");
    json_builder_add_string_value(b, msg);
    json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    reply_json(out, status, reason, root);
}

static JsonNode *jrpc_error(JsonNode *id, int code, const char *message)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    json_builder_set_member_name(b, "id");
    if (id) json_builder_add_value(b, json_node_copy(id));
    else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "error");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "code");
    json_builder_add_int_value(b, code);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
    json_builder_end_object(b);
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

static JsonNode *jrpc_result(JsonNode *id, JsonNode *result)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    json_builder_set_member_name(b, "id");
    if (id) json_builder_add_value(b, json_node_copy(id));
    else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "result");
    json_builder_add_value(b, result);
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

static void add_tool_execute(JsonBuilder *b)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "execute");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "Submit a Luau script to the connected game.");
    json_builder_set_member_name(b, "inputSchema");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "required");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "source");
    json_builder_end_array(b);
    json_builder_set_member_name(b, "properties");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "source");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "Luau source code.");
    json_builder_end_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "Optional label for the script.");
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
}

static void add_tool_logs(JsonBuilder *b)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "logs");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "Fetch recent captured script output.");
    json_builder_set_member_name(b, "inputSchema");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "properties");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "limit");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "integer");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "Maximum lines to return (default 200).");
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
}

static JsonNode *handle_initialize(McpServer *s)
{
    (void)s;
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "protocolVersion");
    json_builder_add_string_value(b, MCP_PROTOCOL_VERSION);
    json_builder_set_member_name(b, "capabilities");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tools");
    json_builder_begin_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "serverInfo");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "relapse-mcp");
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, "0.1.0");
    json_builder_end_object(b);
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

static JsonNode *handle_tools_list(McpServer *s)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tools");
    json_builder_begin_array(b);
    if (s->expose_execute) add_tool_execute(b);
    if (s->expose_logs) add_tool_logs(b);
    json_builder_end_array(b);
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

static JsonNode *content_text_result(const char *text, gboolean is_error)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "content");
    json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "text");
    json_builder_set_member_name(b, "text");
    json_builder_add_string_value(b, text ? text : "");
    json_builder_end_object(b);
    json_builder_end_array(b);
    if (is_error) {
        json_builder_set_member_name(b, "isError");
        json_builder_add_boolean_value(b, TRUE);
    }
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

typedef struct { char *name; char *source; McpServer *s; } ExecInvoke;

static gboolean exec_on_main(gpointer data)
{
    ExecInvoke *inv = data;
    if (inv->s->execute)
        inv->s->execute(inv->name, inv->source, inv->s->user_data);
    g_free(inv->name);
    g_free(inv->source);
    g_free(inv);
    return G_SOURCE_REMOVE;
}

static JsonNode *handle_tool_execute(McpServer *s, JsonObject *args)
{
    if (!s->expose_execute)
        return content_text_result("execute is disabled in settings.", TRUE);
    const char *source = args && json_object_has_member(args, "source")
        ? json_object_get_string_member(args, "source") : NULL;
    if (!source || !*source)
        return content_text_result("Missing 'source' argument.", TRUE);
    const char *name = args && json_object_has_member(args, "name")
        ? json_object_get_string_member(args, "name") : "mcp";
    ExecInvoke *inv = g_new0(ExecInvoke, 1);
    inv->name = g_strdup(name);
    inv->source = g_strdup(source);
    inv->s = s;
    g_idle_add(exec_on_main, inv);
    return content_text_result("queued", FALSE);
}

static JsonNode *handle_tool_logs(McpServer *s, JsonObject *args)
{
    if (!s->expose_logs)
        return content_text_result("logs are disabled in settings.", TRUE);
    gint64 limit = 200;
    if (args && json_object_has_member(args, "limit")) {
        gint64 v = json_object_get_int_member(args, "limit");
        if (v > 0 && v <= MCP_LOG_RING_CAP) limit = v;
    }
    GString *out = g_string_new(NULL);
    g_mutex_lock(&s->ring_mutex);
    guint total = s->ring_count;
    guint take = (guint)MIN((guint)limit, total);
    guint start_offset = total - take;
    for (guint i = 0; i < take; ++i) {
        guint idx = (s->ring_head + start_offset + i) % MCP_LOG_RING_CAP;
        gint64 sec = s->ring[idx].ts_us / G_USEC_PER_SEC;
        g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc(sec);
        g_autofree char *stamp = dt ? g_date_time_format(dt, "%H:%M:%S") : g_strdup("");
        g_string_append_printf(out, "%s [%s] %s\n", stamp,
            s->ring[idx].level ? s->ring[idx].level : "info",
            s->ring[idx].text ? s->ring[idx].text : "");
    }
    g_mutex_unlock(&s->ring_mutex);
    JsonNode *node = content_text_result(out->str, FALSE);
    g_string_free(out, TRUE);
    return node;
}

static JsonNode *handle_tools_call(McpServer *s, JsonObject *params, JsonNode *id)
{
    if (!params) return jrpc_error(id, -32602, "Missing params");
    const char *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    JsonObject *args = json_object_has_member(params, "arguments")
        ? json_object_get_object_member(params, "arguments") : NULL;
    if (!name) return jrpc_error(id, -32602, "Missing tool name");
    JsonNode *result = NULL;
    if (g_strcmp0(name, "execute") == 0) result = handle_tool_execute(s, args);
    else if (g_strcmp0(name, "logs") == 0) result = handle_tool_logs(s, args);
    else return jrpc_error(id, -32601, "Unknown tool");
    return jrpc_result(id, result);
}

static JsonNode *dispatch(McpServer *s, JsonNode *root)
{
    if (!JSON_NODE_HOLDS_OBJECT(root))
        return jrpc_error(NULL, -32600, "Invalid Request");
    JsonObject *obj = json_node_get_object(root);
    JsonNode *id = json_object_has_member(obj, "id") ? json_object_get_member(obj, "id") : NULL;
    const char *method = json_object_has_member(obj, "method")
        ? json_object_get_string_member(obj, "method") : NULL;
    JsonObject *params = json_object_has_member(obj, "params")
        ? json_object_get_object_member(obj, "params") : NULL;
    if (!method) return jrpc_error(id, -32600, "Missing method");

    if (g_str_has_prefix(method, "notifications/")) return NULL;
    if (g_strcmp0(method, "initialize") == 0) return jrpc_result(id, handle_initialize(s));
    if (g_strcmp0(method, "tools/list") == 0)  return jrpc_result(id, handle_tools_list(s));
    if (g_strcmp0(method, "tools/call") == 0)  return handle_tools_call(s, params, id);
    if (g_strcmp0(method, "ping") == 0) {
        JsonNode *empty = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(empty, json_object_new());
        return jrpc_result(id, empty);
    }
    return jrpc_error(id, -32601, "Method not found");
}

static gboolean http_split_headers(const char *buf, gsize len, gsize *header_end)
{
    for (gsize i = 3; i < len; ++i) {
        if (buf[i-3] == '\r' && buf[i-2] == '\n' && buf[i-1] == '\r' && buf[i] == '\n') {
            *header_end = i + 1;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean header_line_iget(const char *headers, const char *name, char **out_value)
{
    gsize name_len = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        gsize line_len = eol ? (gsize)(eol - p) : strlen(p);
        if (line_len > name_len + 1 && g_ascii_strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;
            *out_value = g_strndup(v, (gsize)(p + line_len - v));
            return TRUE;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return FALSE;
}

static void serve_connection(McpServer *s, GSocketConnection *conn)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    GString *buf = g_string_new(NULL);
    char chunk[4096];
    gsize header_end = 0;
    while (buf->len < MCP_MAX_REQUEST) {
        gssize n = g_input_stream_read(in, chunk, sizeof chunk, NULL, NULL);
        if (n <= 0) break;
        g_string_append_len(buf, chunk, n);
        if (http_split_headers(buf->str, buf->len, &header_end)) break;
    }
    if (!header_end) { g_string_free(buf, TRUE); return; }

    g_autofree char *headers = g_strndup(buf->str, header_end);
    g_autofree char *method = NULL;
    g_autofree char *path = NULL;
    const char *sp1 = strchr(headers, ' ');
    if (sp1) {
        method = g_strndup(headers, (gsize)(sp1 - headers));
        const char *sp2 = strchr(sp1 + 1, ' ');
        if (sp2) path = g_strndup(sp1 + 1, (gsize)(sp2 - sp1 - 1));
    }
    (void)path;

    if (g_strcmp0(method, "OPTIONS") == 0) {
        const char *body = "";
        write_http(out, 204, "No Content", "text/plain", body, 0);
        g_string_free(buf, TRUE); return;
    }
    if (g_strcmp0(method, "GET") == 0) {
        const char *body = "{\"server\":\"relapse-mcp\"}";
        write_http(out, 200, "OK", "application/json", body, strlen(body));
        g_string_free(buf, TRUE); return;
    }
    if (g_strcmp0(method, "POST") != 0) {
        reply_error(out, 405, "Method Not Allowed", "Only POST is supported for JSON-RPC");
        g_string_free(buf, TRUE); return;
    }

    if (s->token && *s->token) {
        g_autofree char *auth = NULL;
        if (!header_line_iget(headers, "Authorization", &auth) ||
            !g_str_has_prefix(auth, "Bearer ") ||
            g_strcmp0(auth + 7, s->token) != 0) {
            reply_error(out, 401, "Unauthorized", "Missing or invalid bearer token");
            g_string_free(buf, TRUE); return;
        }
    }

    g_autofree char *cl_str = NULL;
    gsize content_length = 0;
    if (header_line_iget(headers, "Content-Length", &cl_str))
        content_length = (gsize)g_ascii_strtoll(cl_str, NULL, 10);
    if (content_length > MCP_MAX_REQUEST) {
        reply_error(out, 413, "Payload Too Large", "Body exceeds limit");
        g_string_free(buf, TRUE); return;
    }
    while ((buf->len - header_end) < content_length) {
        gssize need = (gssize)(content_length - (buf->len - header_end));
        gsize want = (gsize)MIN((gssize)sizeof chunk, need);
        gssize n = g_input_stream_read(in, chunk, want, NULL, NULL);
        if (n <= 0) break;
        g_string_append_len(buf, chunk, n);
    }
    const char *body = buf->str + header_end;
    gsize body_len = buf->len - header_end;

    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) perr = NULL;
    if (!json_parser_load_from_data(parser, body, body_len, &perr)) {
        JsonNode *err = jrpc_error(NULL, -32700, perr ? perr->message : "Parse error");
        reply_json(out, 200, "OK", err);
        json_node_unref(err);
        g_string_free(buf, TRUE); return;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonNode *reply = dispatch(s, root);
    if (reply) {
        reply_json(out, 200, "OK", reply);
        json_node_unref(reply);
    } else {
        write_http(out, 202, "Accepted", "application/json", "{}", 2);
    }
    g_string_free(buf, TRUE);
}

static gboolean on_incoming(GSocketService *service, GSocketConnection *conn,
                            GObject *source, gpointer user_data)
{
    (void)service; (void)source;
    McpServer *s = user_data;
    serve_connection(s, conn);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return TRUE;
}

static void stop_service(McpServer *s)
{
    if (!s->service) return;
    g_socket_service_stop(s->service);
    g_socket_listener_close(G_SOCKET_LISTENER(s->service));
    g_clear_object(&s->service);
    s->active_port = 0;
}

gboolean mcp_server_apply(McpServer *s, const McpServerConfig *cfg, GError **error)
{
    if (!s || !cfg) return FALSE;
    gboolean want_running = cfg->expose_execute || cfg->expose_logs;
    if (want_running && (!cfg->token || !*cfg->token)) {
        stop_service(s);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Set a bearer token before enabling MCP tools");
        return FALSE;
    }
    g_clear_pointer(&s->token, g_free);
    s->token = cfg->token ? g_strdup(cfg->token) : NULL;
    s->expose_execute = cfg->expose_execute;
    s->expose_logs = cfg->expose_logs;

    if (!want_running) { stop_service(s); return TRUE; }
    if (cfg->port < 1 || cfg->port > 65535) {
        stop_service(s);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid MCP port %d", cfg->port);
        return FALSE;
    }
    if (s->service && s->active_port == cfg->port) return TRUE;
    stop_service(s);

    GSocketService *svc = g_socket_service_new();
    g_autoptr(GInetAddress) addr = g_inet_address_new_from_string("127.0.0.1");
    g_autoptr(GSocketAddress) sa = g_inet_socket_address_new(addr, (guint16)cfg->port);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(svc), sa,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, NULL, error)) {
        g_object_unref(svc);
        return FALSE;
    }
    g_signal_connect(svc, "incoming", G_CALLBACK(on_incoming), s);
    g_socket_service_start(svc);
    s->service = svc;
    s->active_port = cfg->port;
    return TRUE;
}

void mcp_server_free(McpServer *s)
{
    if (!s) return;
    stop_service(s);
    g_clear_pointer(&s->token, g_free);
    for (guint i = 0; i < MCP_LOG_RING_CAP; ++i)
        log_entry_clear(&s->ring[i]);
    g_mutex_clear(&s->ring_mutex);
    g_free(s);
}
