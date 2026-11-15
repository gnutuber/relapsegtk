#include "logs.h"

#include "state/log.h"

#include <glib/gstdio.h>
#include <string.h>

#define LOG_VIEW_MAX_BYTES (4u * 1024u * 1024u)

typedef struct {
    GtkDropDown *picker;
    GtkStringList *items;
    GPtrArray *paths;
    GtkTextView *view;
    GtkTextBuffer *buffer;
    guint timer;
    guint ticks;
    gboolean single;
    gsize loaded_size;
    gint64 loaded_mtime;
} Logs;

static Logs *logs_data(GtkWidget *widget)
{
    return g_object_get_data(G_OBJECT(widget), "logs");
}

static const char *selected_path(Logs *logs)
{
    guint selected = gtk_drop_down_get_selected(logs->picker);
    return selected < logs->paths->len ? g_ptr_array_index(logs->paths, selected) : NULL;
}

static gint compare_names(gconstpointer left, gconstpointer right)
{
    return g_strcmp0(*(const char *const *)left, *(const char *const *)right);
}

static void add_entry(GPtrArray *paths, GPtrArray *names, const char *path, const char *name)
{
    g_ptr_array_add(paths, g_strdup(path));
    g_ptr_array_add(names, g_strdup(name));
}

static gboolean same_paths(Logs *logs, GPtrArray *paths)
{
    if (!logs->paths || logs->paths->len != paths->len)
        return FALSE;
    for (guint i = 0; i < paths->len; ++i)
        if (g_strcmp0(g_ptr_array_index(logs->paths, i), g_ptr_array_index(paths, i)) != 0)
            return FALSE;
    return TRUE;
}

static void rebuild_list(Logs *logs, gboolean force)
{
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);

    if (logs->single) {
        g_autofree char *path = log_path_for(NULL, TRUE);
        add_entry(paths, names, path, "relapsegtk.rlog");
    } else {
        g_autofree char *directory = log_directory();
        g_autofree char *clients = g_build_filename(directory, "clients", NULL);
        GDir *dir = g_dir_open(clients, 0, NULL);
        if (dir) {
            const char *name;
            while ((name = g_dir_read_name(dir))) {
                if (!g_str_has_suffix(name, ".rlog"))
                    continue;
                g_ptr_array_add(files, g_strdup(name));
            }
            g_dir_close(dir);
        }
        g_ptr_array_sort(files, compare_names);
        for (guint i = 0; i < files->len; ++i) {
            const char *file = g_ptr_array_index(files, i);
            g_autofree char *path = g_build_filename(clients, file, NULL);
            g_autofree char *label = g_strndup(file, strlen(file) - strlen(".rlog"));
            add_entry(paths, names, path, label);
        }
        if (!files->len)
            add_entry(paths, names, "", "No logs yet");
    }

    if (!force && same_paths(logs, paths))
        return;

    g_autofree char *keep = logs->paths ? g_strdup(selected_path(logs)) : NULL;
    GPtrArray *old_paths = logs->paths;
    logs->paths = g_steal_pointer(&paths);
    GtkStringList *old_items = logs->items;
    logs->items = gtk_string_list_new(NULL);
    for (guint i = 0; i < names->len; ++i)
        gtk_string_list_append(logs->items, g_ptr_array_index(names, i));
    gtk_drop_down_set_model(logs->picker, G_LIST_MODEL(logs->items));
    guint index = 0;
    if (keep)
        for (guint i = 0; i < logs->paths->len; ++i)
            if (g_strcmp0(g_ptr_array_index(logs->paths, i), keep) == 0) {
                index = i;
                break;
            }
    gtk_drop_down_set_selected(logs->picker, index);
    g_clear_object(&old_items);
    g_clear_pointer(&old_paths, g_ptr_array_unref);
}

static void scroll_to_end(Logs *logs)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(logs->buffer, &end);
    GtkTextMark *mark = gtk_text_buffer_create_mark(logs->buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(logs->view, mark);
    gtk_text_buffer_delete_mark(logs->buffer, mark);
}

static void load_selected(Logs *logs, gboolean force)
{
    const char *path = selected_path(logs);
    GStatBuf info;
    if (!path || !*path || g_stat(path, &info) != 0) {
        if (force || logs->loaded_size || logs->loaded_mtime) {
            gtk_text_buffer_set_text(logs->buffer, "No rconsole output yet.\n", -1);
            logs->loaded_size = 0;
            logs->loaded_mtime = 0;
        }
        return;
    }
    if (!force && info.st_size == (goffset)logs->loaded_size && info.st_mtime == logs->loaded_mtime)
        return;
    g_autofree char *data = NULL;
    gsize size = 0;
    if (!g_file_get_contents(path, &data, &size, NULL))
        return;
    const char *start = data;
    gboolean append = !force && logs->loaded_size && size > logs->loaded_size &&
        size <= LOG_VIEW_MAX_BYTES;
    if (append) {
        start = data + logs->loaded_size;
    } else if (size > LOG_VIEW_MAX_BYTES) {
        start = data + (size - LOG_VIEW_MAX_BYTES);
        const char *newline = strchr(start, '\n');
        if (newline)
            start = newline + 1;
    }
    logs->loaded_size = size;
    logs->loaded_mtime = info.st_mtime;
    if (append) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(logs->buffer, &end);
        gtk_text_buffer_insert(logs->buffer, &end, start, -1);
    } else {
        gtk_text_buffer_set_text(logs->buffer, start, -1);
    }
    scroll_to_end(logs);
}

static gboolean tick(gpointer user_data)
{
    GtkWidget *page = user_data;
    Logs *logs = logs_data(page);
    if (!logs)
        return G_SOURCE_REMOVE;
    if (++logs->ticks % 10 == 0)
        rebuild_list(logs, FALSE);
    load_selected(logs, FALSE);
    return G_SOURCE_CONTINUE;
}

static void picker_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    GtkWidget *page = user_data;
    Logs *logs = logs_data(page);
    if (!logs)
        return;
    logs->loaded_size = 0;
    logs->loaded_mtime = 0;
    load_selected(logs, TRUE);
}

static gboolean trim_file(const char *path, guint keep, GError **error)
{
    g_autofree char *data = NULL;
    gsize size = 0;
    if (!g_file_get_contents(path, &data, &size, error))
        return FALSE;
    gsize end = size;
    while (end > 0 && data[end - 1] == '\n')
        --end;
    gsize start = 0;
    guint count = 0;
    for (gsize pos = end; pos > 0; --pos) {
        if (data[pos - 1] != '\n')
            continue;
        if (++count == keep) {
            start = pos;
            break;
        }
    }
    if (start == 0)
        return TRUE;
    return g_file_set_contents_full(path, data + start, (gssize)(size - start),
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

static void clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *page = user_data;
    Logs *logs = logs_data(page);
    if (!logs)
        return;
    const char *path = selected_path(logs);
    if (!path || !*path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    g_autoptr(GError) error = NULL;
    if (!trim_file(path, 5000, &error)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not clear the log");
        gtk_alert_dialog_set_detail(alert, error->message);
        GtkRoot *root = gtk_widget_get_root(page);
        if (GTK_IS_WINDOW(root))
            gtk_alert_dialog_show(alert, GTK_WINDOW(root));
        return;
    }
    GStatBuf info;
    if (g_stat(path, &info) == 0) {
        logs->loaded_size = (gsize)info.st_size;
        logs->loaded_mtime = info.st_mtime;
    } else {
        logs->loaded_size = 0;
        logs->loaded_mtime = 0;
    }
    gtk_text_buffer_set_text(logs->buffer, "", -1);
}

static void logs_destroyed(gpointer user_data)
{
    Logs *logs = user_data;
    if (logs->timer) {
        g_source_remove(logs->timer);
        logs->timer = 0;
    }
    g_clear_object(&logs->items);
    g_clear_pointer(&logs->paths, g_ptr_array_unref);
    g_free(logs);
}

GtkWidget *logs_new(void)
{
    Logs *logs = g_new0(Logs, 1);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(page, "logs-page");
    g_object_set_data_full(G_OBJECT(page), "logs", logs, logs_destroyed);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = gtk_label_new("Logs");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(title, "settings-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(header), title);

    logs->items = NULL;
    logs->paths = NULL;
    logs->picker = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_add_css_class(GTK_WIDGET(logs->picker), "logs-picker");
    gtk_accessible_update_property(GTK_ACCESSIBLE(logs->picker),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Client whose log is shown", -1);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(logs->picker));

    GtkWidget *clear = gtk_button_new_with_label("Clear log");
    gtk_widget_set_tooltip_text(clear, "Delete every line except the last 5000 and empty the view");
    gtk_accessible_update_property(GTK_ACCESSIBLE(clear),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Clear the log, keeping the last 5000 lines on disk", -1);
    gtk_box_append(GTK_BOX(header), clear);
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_clicked), page);
    gtk_box_append(GTK_BOX(page), header);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
        GTK_POLICY_AUTOMATIC);
    GtkWidget *view = gtk_text_view_new();
    logs->view = GTK_TEXT_VIEW(view);
    logs->buffer = gtk_text_view_get_buffer(logs->view);
    gtk_text_view_set_editable(logs->view, FALSE);
    gtk_text_view_set_cursor_visible(logs->view, FALSE);
    gtk_text_view_set_monospace(logs->view, TRUE);
    gtk_text_view_set_wrap_mode(logs->view, GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(view, "logs-view");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_box_append(GTK_BOX(page), scroll);

    gtk_text_buffer_set_text(logs->buffer, "No rconsole output yet.\n", -1);
    g_signal_connect(logs->picker, "notify::selected", G_CALLBACK(picker_changed), page);
    logs->timer = g_timeout_add(100, tick, page);
    logs_refresh(page);
    return page;
}

void logs_refresh(GtkWidget *page)
{
    Logs *logs = logs_data(page);
    if (!logs)
        return;
    rebuild_list(logs, TRUE);
    logs->loaded_size = 0;
    logs->loaded_mtime = 0;
    load_selected(logs, TRUE);
}

void logs_set_single_file(GtkWidget *page, gboolean single)
{
    Logs *logs = logs_data(page);
    if (!logs || logs->single == single)
        return;
    logs->single = single;
    logs_refresh(page);
    gtk_widget_set_sensitive(GTK_WIDGET(logs->picker), !single);
}
