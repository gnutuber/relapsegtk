#include "autoexec.h"

#include <errno.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct Autoexec Autoexec;

typedef struct {
    Autoexec *state;
    GtkWidget *frame;
    char *directory;
    gboolean enabled;
} AutoexecList;

struct Autoexec {
    GtkWidget *page;
    char *scripts_path;
    char *autoexec_path;
    AutoexecList available;
    AutoexecList enabled;
    AutoexecList *drag_source;
};

char *autoexec_directory_path(void)
{
    return g_build_filename(g_get_home_dir(), ".local", "share", "relapse", "autoexec", NULL);
}

static void show_error(Autoexec *state, const char *message)
{
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not update Autoexec");
    gtk_alert_dialog_set_detail(alert, message);
    GtkRoot *root = gtk_widget_get_root(state->page);
    gtk_alert_dialog_show(alert, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL);
}

static GdkTexture *drag_icon(const char *resource)
{
    g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_new_from_resource_at_scale(
        resource, 48, 48, TRUE, NULL);
    if (!scaled)
        return NULL;
    GdkPixbuf *icon = scaled;
    g_autoptr(GdkPixbuf) flattened = NULL;
    GdkDisplay *display = gdk_display_get_default();
    if (display && !gdk_display_is_composited(display) && gdk_pixbuf_get_has_alpha(scaled)) {
        int width = gdk_pixbuf_get_width(scaled);
        int height = gdk_pixbuf_get_height(scaled);
        flattened = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
        if (flattened) {
            gdk_pixbuf_fill(flattened, 0x1c1d20ff);
            gdk_pixbuf_composite(scaled, flattened, 0, 0, width, height, 0, 0, 1, 1,
                GDK_INTERP_NEAREST, 255);
            icon = flattened;
        }
    }
    gboolean alpha = gdk_pixbuf_get_has_alpha(icon);
    gsize stride = (gsize)gdk_pixbuf_get_rowstride(icon);
    gsize size = stride * (gsize)gdk_pixbuf_get_height(icon);
    g_autoptr(GBytes) bytes = g_bytes_new(gdk_pixbuf_read_pixels(icon), size);
    return gdk_memory_texture_new(gdk_pixbuf_get_width(icon),
        gdk_pixbuf_get_height(icon), alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        bytes, stride);
}

static void drag_started(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
    (void)source; (void)drag;
    AutoexecList *list = user_data;
    list->state->drag_source = list;
}

static void drag_finished(GtkDragSource *source, GdkDrag *drag, gboolean delete_data,
                          gpointer user_data)
{
    (void)source; (void)drag; (void)delete_data;
    Autoexec *state = ((AutoexecList *)user_data)->state;
    state->drag_source = NULL;
    if (state->available.frame)
        gtk_widget_remove_css_class(state->available.frame, "drop-target");
    if (state->enabled.frame)
        gtk_widget_remove_css_class(state->enabled.frame, "drop-target");
}

static GtkWidget *row_new(gpointer item, gpointer user_data)
{
    AutoexecList *list = user_data;
    GFileInfo *info = G_FILE_INFO(item);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_add_css_class(row, "autoexec-row");
    GtkWidget *label = gtk_label_new(g_file_info_get_display_name(info));
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (!path)
        return row;
    gtk_widget_set_tooltip_text(row, path);
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_COPY);
    g_autoptr(GdkContentProvider) provider = gdk_content_provider_new_typed(G_TYPE_STRING, path);
    gtk_drag_source_set_content(source, provider);
    const char *icon = list->enabled
        ? "/org/relapse/RelapseGtk/icons/editor.png"
        : "/org/relapse/RelapseGtk/icons/autoexec.png";
    g_autoptr(GdkTexture) icon_texture = drag_icon(icon);
    if (icon_texture)
        gtk_drag_source_set_icon(source, GDK_PAINTABLE(icon_texture), 24, 24);
    g_signal_connect(source, "drag-begin", G_CALLBACK(drag_started), list);
    g_signal_connect(source, "drag-end", G_CALLBACK(drag_finished), list);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
    return row;
}

static gboolean dropped(GtkDropTarget *target, const GValue *value, double x, double y,
                        gpointer user_data)
{
    (void)target; (void)x; (void)y;
    AutoexecList *list = user_data;
    Autoexec *state = list->state;
    gtk_widget_remove_css_class(list->frame, "drop-target");
    if (!G_VALUE_HOLDS_STRING(value))
        return FALSE;
    const char *source = g_value_get_string(value);
    if (!source || !g_path_is_absolute(source) || !g_file_test(source, G_FILE_TEST_IS_REGULAR))
        return FALSE;
    g_autofree char *directory = g_path_get_dirname(source);
    g_autofree char *base = g_path_get_basename(source);
    g_autofree char *destination = g_build_filename(state->autoexec_path, base, NULL);
    g_autoptr(GError) error = NULL;
    gboolean copied;
    if (list->enabled) {
        if (!g_str_equal(directory, state->scripts_path))
            return FALSE;
        g_autoptr(GFile) from = g_file_new_for_path(source);
        g_autoptr(GFile) to = g_file_new_for_path(destination);
        copied = g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
    } else {
        if (!g_str_equal(directory, state->autoexec_path))
            return FALSE;
        copied = g_unlink(destination) == 0;
        if (!copied)
            g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Could not remove %s: %s", base, g_strerror(errno));
    }
    if (!copied) {
        show_error(state, error ? error->message : "Unknown error.");
        return FALSE;
    }
    return TRUE;
}

static const char *drop_path(GtkDropTarget *target)
{
    const GValue *value = gtk_drop_target_get_value(target);
    if (!value || !G_VALUE_HOLDS_STRING(value))
        return NULL;
    const char *path = g_value_get_string(value);
    if (!path || !g_path_is_absolute(path))
        return NULL;
    return path;
}

static gboolean drop_would_be_handled(AutoexecList *list, GtkDropTarget *target)
{
    if (list->state->drag_source == list)
        return FALSE;
    const char *path = drop_path(target);
    if (!path)
        return list->state->drag_source != NULL;
    g_autofree char *directory = g_path_get_dirname(path);
    return g_str_equal(directory,
        list->enabled ? list->state->scripts_path : list->state->autoexec_path);
}

static GdkDragAction drag_entered(GtkDropTarget *target, double x, double y, gpointer user_data)
{
    (void)x; (void)y;
    AutoexecList *list = user_data;
    if (!drop_would_be_handled(list, target)) {
        gtk_widget_remove_css_class(list->frame, "drop-target");
        gtk_drop_target_reject(target);
        return 0;
    }
    gtk_widget_add_css_class(list->frame, "drop-target");
    return GDK_ACTION_COPY;
}

static void drag_left(GtkDropTarget *target, gpointer user_data)
{
    (void)target;
    AutoexecList *list = user_data;
    gtk_widget_remove_css_class(list->frame, "drop-target");
}

static void update_placeholder(GtkDirectoryList *directory, GParamSpec *property, GtkLabel *label)
{
    (void)property;
    const GError *error = gtk_directory_list_get_error(directory);
    if (error) {
        g_autofree char *message = g_strdup_printf("Could not read this folder:\n%s", error->message);
        gtk_label_set_text(label, message);
    } else if (gtk_directory_list_is_loading(directory)) {
        gtk_label_set_text(label, "Loading scripts…");
    } else {
        const char *base = g_object_get_data(G_OBJECT(label), "autoexec-placeholder");
        gtk_label_set_text(label, base ? base : "No files yet.");
    }
}

static gboolean is_file(gpointer item, gpointer user_data)
{
    (void)user_data;
    return g_file_info_get_file_type(G_FILE_INFO(item)) == G_FILE_TYPE_REGULAR;
}

static gint compare_names(gconstpointer first, gconstpointer second, gpointer user_data)
{
    (void)user_data;
    g_autofree char *a = g_utf8_casefold(
        g_file_info_get_display_name(G_FILE_INFO((gpointer)first)), -1);
    g_autofree char *b = g_utf8_casefold(
        g_file_info_get_display_name(G_FILE_INFO((gpointer)second)), -1);
    return g_utf8_collate(a, b);
}

static GtkWidget *list_new(AutoexecList *list, const char *placeholder_text)
{
    GtkWidget *widget = gtk_list_box_new();
    gtk_widget_add_css_class(widget, "autoexec-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(widget), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(widget), FALSE);
    GtkWidget *placeholder = gtk_label_new(placeholder_text);
    gtk_widget_add_css_class(placeholder, "autoexec-empty");
    gtk_label_set_wrap(GTK_LABEL(placeholder), TRUE);
    gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(placeholder), "autoexec-placeholder",
        g_strdup(placeholder_text), g_free);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(widget), placeholder);

    GtkDirectoryList *directory = gtk_directory_list_new(
        "standard::name,standard::display-name,standard::type", NULL);
    gtk_directory_list_set_monitored(directory, TRUE);
    g_signal_connect_object(directory, "notify::loading", G_CALLBACK(update_placeholder),
                            placeholder, 0);
    g_signal_connect_object(directory, "notify::error", G_CALLBACK(update_placeholder),
                            placeholder, 0);
    GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(directory),
        GTK_FILTER(gtk_custom_filter_new(is_file, NULL, NULL)));
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered),
        GTK_SORTER(gtk_custom_sorter_new(compare_names, NULL, NULL)));
    gtk_list_box_bind_model(GTK_LIST_BOX(widget), G_LIST_MODEL(sorted), row_new, list, NULL);
    g_object_unref(sorted);

    if (g_mkdir_with_parents(list->directory, 0700) != 0) {
        g_autofree char *message = g_strdup_printf("Could not create %s:\n%s",
            list->directory, g_strerror(errno));
        gtk_label_set_text(GTK_LABEL(placeholder), message);
    } else {
        g_autoptr(GFile) folder = g_file_new_for_path(list->directory);
        gtk_directory_list_set_file(directory, folder);
    }

    GtkDropTarget *target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
    gtk_drop_target_set_preload(target, TRUE);
    g_signal_connect(target, "drop", G_CALLBACK(dropped), list);
    g_signal_connect(target, "enter", G_CALLBACK(drag_entered), list);
    g_signal_connect(target, "leave", G_CALLBACK(drag_left), list);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(target));
    return widget;
}

static GtkWidget *column_new(AutoexecList *list, const char *title, const char *hint,
                             const char *placeholder_text, GtkWidget **out_scroll)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(column, TRUE);
    gtk_widget_set_vexpand(column, TRUE);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name = gtk_label_new(title);
    gtk_widget_add_css_class(name, "autoexec-title");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(header), name);
    GtkWidget *detail = gtk_label_new(hint);
    gtk_widget_add_css_class(detail, "autoexec-hint");
    gtk_label_set_xalign(GTK_LABEL(detail), 1);
    gtk_box_append(GTK_BOX(header), detail);
    gtk_widget_set_tooltip_text(name, list->directory);
    gtk_box_append(GTK_BOX(column), header);

    list->frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(list->frame, "autoexec-frame");
    gtk_widget_set_vexpand(list->frame, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroll, "autoexec-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_new(list, placeholder_text));
    gtk_box_append(GTK_BOX(list->frame), scroll);
    gtk_box_append(GTK_BOX(column), list->frame);
    *out_scroll = scroll;
    return column;
}

static void free_autoexec(gpointer user_data)
{
    Autoexec *state = user_data;
    g_free(state->scripts_path);
    g_free(state->autoexec_path);
    g_free(state->available.directory);
    g_free(state->enabled.directory);
    g_free(state);
}

GtkWidget *autoexec_new(const char *scripts_path)
{
    g_return_val_if_fail(scripts_path != NULL, NULL);
    Autoexec *state = g_new0(Autoexec, 1);
    state->scripts_path = g_strdup(scripts_path);
    state->autoexec_path = autoexec_directory_path();
    state->available.state = state;
    state->available.directory = g_strdup(scripts_path);
    state->available.enabled = FALSE;
    state->enabled.state = state;
    state->enabled.directory = g_strdup(state->autoexec_path);
    state->enabled.enabled = TRUE;

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(page, "autoexec-page");
    gtk_widget_set_vexpand(page, TRUE);
    state->page = page;
    g_object_set_data_full(G_OBJECT(page), "autoexec", state, free_autoexec);

    GtkWidget *title = gtk_label_new("Autoexec");
    gtk_widget_add_css_class(title, "autoexec-page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(page), title);

    GtkWidget *description = gtk_label_new(
        "Every script in this folder runs automatically when an executor instance starts. "
        "Drag a script from your Scripts folder into Autoexec to copy it there, or drag it "
        "back to remove it from Autoexec. Your original script is never moved.");
    gtk_widget_add_css_class(description, "autoexec-description");
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 90);
    gtk_box_append(GTK_BOX(page), description);

    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_hexpand(columns, TRUE);
    gtk_widget_set_vexpand(columns, TRUE);
    gtk_box_append(GTK_BOX(page), columns);

    GtkWidget *scroll = NULL;
    GtkWidget *available = column_new(&state->available, "Scripts", "drag to the right →",
        "No scripts yet. Add a script to your Scripts folder first.", &scroll);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(columns), available);

    GtkWidget *enabled = column_new(&state->enabled, "Autoexec", "← drag back to remove",
        "Nothing here yet. Drag scripts from the left column to auto execute them.", &scroll);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(columns), enabled);
    return page;
}
