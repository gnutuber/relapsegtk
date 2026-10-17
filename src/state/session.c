#include "session.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

enum { MaxSessionBytes = 128 * 1024 * 1024, MaxSessionTabs = 512 };

static void tab_free(gpointer data)
{
    SessionTab *tab = data;
    g_free(tab->name);
    g_free(tab->uri);
    g_free(tab->text);
    g_free(tab);
}

Session *session_new(void)
{
    Session *session = g_new0(Session, 1);
    session->tabs = g_ptr_array_new_with_free_func(tab_free);
    session->next_untitled = 1;
    return session;
}

void session_free(Session *session)
{
    if (!session)
        return;
    g_ptr_array_unref(session->tabs);
    g_free(session);
}

static char *session_path(void)
{
    return g_build_filename(g_get_user_state_dir(), "relapsegtk", "session.json", NULL);
}

static gboolean invalid(GError **error, const char *detail)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, detail);
    return FALSE;
}

static gboolean integer_member(JsonObject *object, const char *name, gint64 *value)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || json_node_get_value_type(node) != G_TYPE_INT64)
        return FALSE;
    *value = json_node_get_int(node);
    return TRUE;
}

static gboolean string_member(JsonObject *object, const char *name, char **value, gboolean nullable)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node)
        return FALSE;
    if (nullable && JSON_NODE_HOLDS_NULL(node))
        return TRUE;
    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const char *text = json_node_get_string(node);
    if (!text || !g_utf8_validate(text, -1, NULL))
        return FALSE;
    *value = g_strdup(text);
    return TRUE;
}

gboolean session_load(Session **result, GError **error)
{
    *result = NULL;
    g_autofree char *path = session_path();
    GStatBuf info;
    if (g_stat(path, &info) != 0) {
        int saved_errno = errno;
        if (saved_errno == ENOENT)
            return TRUE;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not read session: %s", g_strerror(saved_errno));
        return FALSE;
    }
    if (!S_ISREG(info.st_mode) || info.st_size > MaxSessionBytes)
        return invalid(error, "Session file is not a regular file or exceeds 128 MiB");
    g_autofree char *contents = NULL;
    gsize size;
    if (!g_file_get_contents(path, &contents, &size, error))
        return FALSE;
    if (size > MaxSessionBytes || memchr(contents, '\0', size))
        return invalid(error, "Session file is too large or contains invalid text");
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, contents, size, error))
        return FALSE;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return invalid(error, "Invalid session document");
    JsonObject *object = json_node_get_object(root);
    gint64 version, active, next;
    if (!integer_member(object, "version", &version) || version != 1)
        return invalid(error, "Unsupported session version");
    JsonNode *tabs_node = json_object_get_member(object, "tabs");
    if (!tabs_node || !JSON_NODE_HOLDS_ARRAY(tabs_node) ||
        !integer_member(object, "active_tab", &active) ||
        !integer_member(object, "next_untitled", &next) || next < 1 || next >= G_MAXUINT)
        return invalid(error, "Invalid session metadata");
    JsonArray *array = json_node_get_array(tabs_node);
    guint count = json_array_get_length(array);
    if (count > MaxSessionTabs || active < 0 || active >= MAX(count, 1u))
        return invalid(error, "Invalid session tab count or active tab");
    g_autoptr(Session) session = session_new();
    session->active_tab = active;
    session->next_untitled = next;
    g_autoptr(GHashTable) files = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < count; ++i) {
        JsonNode *node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_OBJECT(node))
            return invalid(error, "Invalid session tab");
        JsonObject *entry = json_node_get_object(node);
        SessionTab *tab = g_new0(SessionTab, 1);
        g_ptr_array_add(session->tabs, tab);
        gint64 cursor;
        JsonNode *modified = json_object_get_member(entry, "modified");
        if (!string_member(entry, "name", &tab->name, FALSE) || !*tab->name || strlen(tab->name) > 4096 ||
            !string_member(entry, "uri", &tab->uri, TRUE) ||
            !string_member(entry, "text", &tab->text, TRUE) ||
            !integer_member(entry, "cursor", &cursor) || cursor < 0 || cursor > G_MAXINT ||
            !modified || json_node_get_value_type(modified) != G_TYPE_BOOLEAN)
            return invalid(error, "Invalid session tab fields");
        tab->cursor = cursor;
        tab->modified = json_node_get_boolean(modified);
        if ((!tab->text && (!tab->uri || tab->modified)) ||
            (tab->uri && (!g_uri_peek_scheme(tab->uri) || !g_hash_table_add(files, tab->uri))))
            return invalid(error, "Invalid or duplicate session file");
    }
    *result = g_steal_pointer(&session);
    return TRUE;
}

static void add_string(JsonBuilder *builder, const char *name, const char *value)
{
    json_builder_set_member_name(builder, name);
    if (value)
        json_builder_add_string_value(builder, value);
    else
        json_builder_add_null_value(builder);
}

gboolean session_save(const Session *session, GError **error)
{
    if (session->tabs->len > MaxSessionTabs)
        return invalid(error, "Session exceeds 512 tabs; the previous snapshot was kept");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "active_tab");
    json_builder_add_int_value(builder, session->active_tab);
    json_builder_set_member_name(builder, "next_untitled");
    json_builder_add_int_value(builder, session->next_untitled);
    json_builder_set_member_name(builder, "tabs");
    json_builder_begin_array(builder);
    for (guint i = 0; i < session->tabs->len; ++i) {
        SessionTab *tab = g_ptr_array_index(session->tabs, i);
        json_builder_begin_object(builder);
        add_string(builder, "name", tab->name);
        add_string(builder, "uri", tab->uri);
        add_string(builder, "text", tab->text);
        json_builder_set_member_name(builder, "modified");
        json_builder_add_boolean_value(builder, tab->modified);
        json_builder_set_member_name(builder, "cursor");
        json_builder_add_int_value(builder, tab->cursor);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize size;
    g_autofree char *contents = json_generator_to_data(generator, &size);
    if (size > MaxSessionBytes)
        return invalid(error, "Session exceeds 128 MiB; the previous snapshot was kept");
    g_autofree char *path = session_path();
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not create session directory: %s", g_strerror(saved_errno));
        return FALSE;
    }
    return g_file_set_contents_full(path, contents, size,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

struct SessionWriter {
    guint refs;
    GApplication *application;
    SessionErrorFunc error;
    gpointer user_data;
    Session *pending;
    gboolean writing;
    gboolean reported_error;
};

static void writer_unref(SessionWriter *writer)
{
    if (--writer->refs)
        return;
    session_free(writer->pending);
    g_clear_object(&writer->application);
    g_free(writer);
}

static void write_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
    (void)source;
    (void)cancellable;
    g_autoptr(GError) error = NULL;
    if (session_save(data, &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

static void writer_start(SessionWriter *writer);

static void written(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    SessionWriter *writer = user_data;
    g_autoptr(GError) error = NULL;
    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        g_printerr("Could not save session: %s\n", error->message);
        if (!writer->reported_error && writer->error)
            writer->error(error->message, writer->user_data);
        writer->reported_error = TRUE;
    } else {
        writer->reported_error = FALSE;
    }
    if (writer->pending) {
        writer_start(writer);
    } else {
        writer->writing = FALSE;
        if (writer->application)
            g_application_release(writer->application);
    }
    writer_unref(writer);
}

static void writer_start(SessionWriter *writer)
{
    ++writer->refs;
    g_autoptr(GTask) task = g_task_new(NULL, NULL, written, writer);
    g_task_set_task_data(task, g_steal_pointer(&writer->pending), (GDestroyNotify)session_free);
    g_task_run_in_thread(task, write_thread);
}

SessionWriter *session_writer_new(GApplication *application, SessionErrorFunc error, gpointer user_data)
{
    SessionWriter *writer = g_new0(SessionWriter, 1);
    writer->refs = 1;
    writer->application = application ? g_object_ref(application) : NULL;
    writer->error = error;
    writer->user_data = user_data;
    return writer;
}

void session_writer_queue(SessionWriter *writer, Session *session)
{
    session_free(writer->pending);
    writer->pending = session;
    if (!writer->writing) {
        writer->writing = TRUE;
        if (writer->application)
            g_application_hold(writer->application);
        writer_start(writer);
    }
}

void session_writer_free(SessionWriter *writer)
{
    if (!writer)
        return;
    writer->error = NULL;
    writer->user_data = NULL;
    writer_unref(writer);
}
