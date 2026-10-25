#include "output.h"
#include "tab_strip.h"
#include "profiles/store.h"
#include <gtksourceview/gtksource.h>
#include <string.h>

typedef struct Output Output;
typedef struct {
    char text[9];
    gboolean first;
} Timestamp;
typedef struct {
    Output *output;
    GtkTextBuffer *buffer;
    GArray *timestamps;
    GtkWidget *tab;
    GtkWidget *select;
    GtkWidget *close;
    char *path;
    gboolean follow;
    gboolean active;
    double position;
} Source;

struct Output {
    GtkTextView *view;
    GtkSourceGutterRenderer *gutter;
    GtkWidget *tabs;
    GtkWidget *empty;
    GHashTable *sources;
    Source *selected;
    Source *merged;
    GtkAdjustment *scroll;
    GtkWidget *jump;
    gboolean all_selected;
    gboolean adjusting;
    guint scroll_idle;
};

static void free_source(gpointer data)
{
    Source *source = data;
    g_object_unref(source->buffer);
    g_array_unref(source->timestamps);
    g_free(source->path);
    g_free(source);
}

static void free_output(gpointer data)
{
    Output *output = data;
    if (output->scroll_idle) g_source_remove(output->scroll_idle);
    g_hash_table_unref(output->sources);
    g_free(output);
}

static void timestamp_data(GtkSourceGutterRendererText *renderer, GtkSourceGutterLines *lines,
                           guint line, gpointer data)
{
    (void)lines;
    Output *output = data;
    Source *source = output->selected;
    const char *text = "";
    if (source && line < source->timestamps->len) {
        Timestamp *stamp = &g_array_index(source->timestamps, Timestamp, line);
        if (stamp->first) text = stamp->text;
    }
    gtk_source_gutter_renderer_text_set_text(renderer, text, -1);
}

static void size_timestamps(GtkWidget *renderer, gpointer data)
{
    (void)data;
    int width;
    gtk_source_gutter_renderer_text_measure(GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), "00:00:00", &width, NULL);
    gtk_widget_set_size_request(renderer, width + 20, -1);
}

static void update_jump(Output *output)
{
    gboolean more = output->scroll &&
        gtk_adjustment_get_upper(output->scroll) - gtk_adjustment_get_page_size(output->scroll) > 2;
    gtk_widget_set_visible(output->jump,
        output->selected && !output->selected->follow && more);
}

static gboolean scroll_to_position(gpointer data)
{
    Output *output = data;
    output->scroll_idle = 0;
    if (!output->selected) return G_SOURCE_REMOVE;
    output->adjusting = TRUE;
    GtkAdjustment *scroll = output->scroll;
    double bottom = MAX(0, gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll));
    gtk_adjustment_set_value(scroll, output->selected->follow ? bottom : MIN(bottom, output->selected->position));
    output->adjusting = FALSE;
    update_jump(output);
    return G_SOURCE_REMOVE;
}

static void queue_scroll(Output *output)
{
    if (!output->scroll_idle) output->scroll_idle = g_idle_add(scroll_to_position, output);
}

static void scroll_changed(GtkAdjustment *scroll, gpointer data)
{
    (void)scroll;
    Output *output = data;
    if (output->selected && !output->adjusting) queue_scroll(output);
}

static gboolean scroll_input(GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
    (void)controller;
    (void)dx;
    Output *output = data;
    if (dy < 0 && output->selected) {
        output->selected->follow = FALSE;
        output->selected->position = gtk_adjustment_get_value(output->scroll);
        if (output->scroll_idle) g_source_remove(output->scroll_idle);
        output->scroll_idle = 0;
        update_jump(output);
    }
    return FALSE;
}

static void position_changed(GtkAdjustment *scroll, gpointer data)
{
    Output *output = data;
    if (!output->selected || output->adjusting || output->scroll_idle) return;
    Source *source = output->selected;
    source->position = gtk_adjustment_get_value(scroll);
    double bottom = gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll);
    source->follow = source->position >= bottom - 2;
    update_jump(output);
}

static void apply_highlights(Output *output)
{
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, output->sources);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Source *source = value;
        gboolean active = source == output->selected ||
            (output->all_selected && source != output->merged);
        if (active) gtk_widget_add_css_class(source->select, "active");
        else gtk_widget_remove_css_class(source->select, "active");
    }
}

static guint source_count(Output *output)
{
    guint count = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, output->sources);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (value != output->merged) ++count;
    return count;
}

static void update_tab_visibility(Output *output)
{
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, output->sources);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Source *source = value;
        gboolean visible = source == output->merged ? output->all_selected : !output->all_selected;
        gtk_widget_set_visible(source->tab, visible);
    }
}

static void select_source(Source *source)
{
    Output *output = source->output;
    if (output->selected == source) return;
    output->selected = source;
    apply_highlights(output);
    output->adjusting = TRUE;
    gtk_text_view_set_buffer(output->view, source->buffer);
    output->adjusting = FALSE;
    scroll_changed(output->scroll, output);
    update_jump(output);
    update_tab_visibility(output);
    tab_strip_reveal(output->tabs, source->tab);
}

static void source_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    select_source(data);
}

static void close_source(GtkButton *button, gpointer data)
{
    (void)button;
    Source *source = data;
    Output *output = source->output;
    if (source->active || source == output->merged) return;
    if (output->selected == source) {
        GtkWidget *next = gtk_widget_get_next_sibling(source->tab);
        if (!next) next = gtk_widget_get_prev_sibling(source->tab);
        output->selected = NULL;
        if (next) select_source(g_object_get_data(G_OBJECT(next), "source"));
        else gtk_text_view_set_buffer(output->view, NULL);
    }
    gtk_box_remove(tab_strip_content(output->tabs), source->tab);
    g_hash_table_remove(output->sources, source->path);
    gtk_widget_set_visible(output->empty, source_count(output) == 0);
    update_tab_visibility(output);
    if (!output->selected) {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, output->sources);
        if (g_hash_table_iter_next(&iter, NULL, &value)) select_source(value);
    }
}

static Source *ensure_source(Output *output, const char *path)
{
    Source *source = g_hash_table_lookup(output->sources, path);
    if (source) return source;
    source = g_new0(Source, 1);
    source->output = output;
    source->path = g_strdup(path);
    source->follow = TRUE;
    source->active = TRUE;
    source->buffer = GTK_TEXT_BUFFER(gtk_source_buffer_new(NULL));
    source->timestamps = g_array_new(FALSE, TRUE, sizeof(Timestamp));
    gtk_text_buffer_set_enable_undo(source->buffer, FALSE);
    gtk_text_buffer_create_tag(source->buffer, "print", "foreground", "#d8dbe0", NULL);
    gtk_text_buffer_create_tag(source->buffer, "info", "foreground", "#94b9e3", NULL);
    gtk_text_buffer_create_tag(source->buffer, "warn", "foreground", "#e3bd7a", NULL);
    gtk_text_buffer_create_tag(source->buffer, "error", "foreground", "#e28b8b", NULL);
    gboolean merged = !*path;
    source->tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(source->tab, "output-tab");
    if (!merged) gtk_widget_add_css_class(source->tab, "running");
    g_object_set_data(G_OBJECT(source->tab), "source", source);
    g_autofree char *name = merged ? g_strdup("All") : profile_socket_name(path);
    source->select = gtk_button_new();
    gtk_widget_add_css_class(source->select, "output-source");
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *activity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(activity, "output-activity");
    gtk_widget_set_valign(activity, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(content), activity);
    GtkLabel *label = GTK_LABEL(gtk_label_new(name));
    gtk_widget_add_css_class(GTK_WIDGET(label), "output-source-name");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(label));
    gtk_button_set_child(GTK_BUTTON(source->select), content);
    gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(label, 20);
    gtk_box_append(GTK_BOX(source->tab), source->select);
    g_signal_connect(source->select, "clicked", G_CALLBACK(source_clicked), source);
    source->close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(source->close, "tab-close");
    gtk_widget_set_tooltip_text(source->close, "Close output history");
    gtk_widget_set_visible(source->close, FALSE);
    g_signal_connect(source->close, "clicked", G_CALLBACK(close_source), source);
    gtk_box_append(GTK_BOX(source->tab), source->close);
    g_hash_table_insert(output->sources, source->path, source);
    if (merged) {
        output->merged = source;
        gtk_widget_add_css_class(source->tab, "running");
        gtk_box_prepend(tab_strip_content(output->tabs), source->tab);
        gtk_widget_set_tooltip_text(source->select, "Combined output from every instance");
    } else {
        gtk_box_append(tab_strip_content(output->tabs), source->tab);
        gtk_widget_set_visible(output->empty, FALSE);
        if (!output->selected) select_source(source);
    }
    update_tab_visibility(output);
    return source;
}

void output_set_target(GtkWidget *pane, const char *path)
{
    Output *output = g_object_get_data(G_OBJECT(pane), "output");
    const char *target = path ? path : "";
    output->all_selected = !*target;
    Source *source = ensure_source(output, target);
    if (output->selected != source) select_source(source);
    apply_highlights(output);
    update_tab_visibility(output);
}

void output_set_status(GtkWidget *pane, const char *path, const char *text, gboolean active)
{
    Output *output = g_object_get_data(G_OBJECT(pane), "output");
    Source *source = ensure_source(output, path);
    source->active = active;
    g_autofree char *tip = g_strdup_printf("%s\n%s — %s", path, active ? "Running" : "Closed", text);
    gtk_widget_set_tooltip_text(source->select, tip);
    gtk_widget_set_visible(source->close, !active);
    if (active) gtk_widget_add_css_class(source->tab, "running");
    else gtk_widget_remove_css_class(source->tab, "running");
}

static void jump_to_latest(GtkButton *button, gpointer data)
{
    (void)button;
    Output *output = data;
    if (!output->selected) return;
    output->selected->follow = TRUE;
    if (output->scroll_idle) g_source_remove(output->scroll_idle);
    output->scroll_idle = 0;
    scroll_to_position(output);
    update_jump(output);
}

static void clear_output(GtkButton *button, gpointer data)
{
    (void)button;
    Output *output = data;
    if (!output->selected) return;
    output->selected->follow = TRUE;
    g_array_set_size(output->selected->timestamps, 0);
    gtk_text_buffer_set_text(output->selected->buffer, "", 0);
}

static void copy_output(GtkButton *button, gpointer data)
{
    Output *output = data;
    if (!output->selected) return;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(output->selected->buffer, &start, &end);
    g_autofree char *text = gtk_text_buffer_get_text(output->selected->buffer, &start, &end, FALSE);
    if (*text) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), text);
}

static void append_line(Output *output, Source *source, const char *level, const char *text);

void output_append(GtkWidget *pane, const char *path, const char *level, const char *text)
{
    Output *output = g_object_get_data(G_OBJECT(pane), "output");
    Source *source = ensure_source(output, path);
    append_line(output, source, level, text);
    Source *merged = ensure_source(output, "");
    if (source_count(output) > 1) {
        g_autofree char *name = profile_socket_name(path);
        g_autofree char *tagged = g_strdup_printf("[%s] %s", name, text);
        append_line(output, merged, level, tagged);
    } else {
        append_line(output, merged, level, text);
    }
    update_tab_visibility(output);
}

static void append_line(Output *output, Source *source, const char *level, const char *text)
{
    GtkTextBuffer *buffer = source->buffer;
    if (source == output->selected && source->follow) queue_scroll(output);
    g_autofree char *valid = g_utf8_make_valid(text, -1);
    if (g_utf8_strlen(valid, -1) > 4096) *g_utf8_offset_to_pointer(valid, 4096) = '\0';
    const char *tag = g_str_equal(level, "warn") ? "warn" : g_str_equal(level, "error") ? "error" :
                      g_str_equal(level, "info") ? "info" : "print";
    g_autofree char *line = g_str_has_suffix(valid, "\n") ? g_strdup(valid) : g_strconcat(valid, "\n", NULL);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *time = g_date_time_format(now, "%H:%M:%S");
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    guint start_line = gtk_text_iter_get_line(&end);
    gtk_text_buffer_insert_with_tags_by_name(buffer, &end, line, -1, tag, NULL);
    guint end_line = gtk_text_iter_get_line(&end);
    g_array_set_size(source->timestamps, gtk_text_buffer_get_line_count(buffer));
    for (guint i = start_line; i < end_line; ++i) {
        Timestamp *stamp = &g_array_index(source->timestamps, Timestamp, i);
        g_strlcpy(stamp->text, time, sizeof(stamp->text));
        stamp->first = i == start_line;
    }
    GtkTextIter first, cutoff;
    gtk_text_buffer_get_start_iter(buffer, &first);
    int lines = gtk_text_buffer_get_line_count(buffer);
    int chars = gtk_text_buffer_get_char_count(buffer);
    if (lines > 2000 || chars > 262144) {
        gtk_text_buffer_get_iter_at_offset(buffer, &cutoff, MAX(0, chars - 262144));
        if (gtk_text_iter_get_line(&cutoff) < lines - 2000)
            gtk_text_buffer_get_iter_at_line(buffer, &cutoff, lines - 2000);
        else if (!gtk_text_iter_starts_line(&cutoff)) gtk_text_iter_forward_line(&cutoff);
        guint removed = gtk_text_iter_get_line(&cutoff);
        g_array_remove_range(source->timestamps, 0, removed);
        if (source->timestamps->len) g_array_index(source->timestamps, Timestamp, 0).first = TRUE;
        gtk_text_buffer_delete(buffer, &first, &cutoff);
    }
}

GtkWidget *output_new(void)
{
    gtk_source_init();
    Output *output = g_new0(Output, 1);
    output->sources = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_source);
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(pane, "output-pane");
    g_object_set_data_full(G_OBJECT(pane), "output", output, free_output);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "output-header");
    GtkWidget *title = gtk_label_new("Output");
    gtk_widget_add_css_class(title, "output-title");
    gtk_box_append(GTK_BOX(header), title);
    output->empty = gtk_label_new("No sockets");
    gtk_widget_add_css_class(output->empty, "output-status");
    gtk_box_append(GTK_BOX(header), output->empty);
    output->tabs = tab_strip_new();
    gtk_widget_add_css_class(output->tabs, "output-tabs");
    gtk_box_append(GTK_BOX(header), output->tabs);
    GtkWidget *copy = gtk_button_new_with_label("Copy");
    gtk_widget_add_css_class(copy, "output-copy");
    gtk_widget_set_tooltip_text(copy, "Copy this socket's output");
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_output), output);
    gtk_box_append(GTK_BOX(header), copy);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(clear, "output-clear");
    gtk_widget_set_tooltip_text(clear, "Clear this socket's output");
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_output), output);
    gtk_box_append(GTK_BOX(header), clear);
    gtk_box_append(GTK_BOX(pane), header);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_box_append(GTK_BOX(pane), overlay);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 48);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);
    output->view = GTK_TEXT_VIEW(gtk_source_view_new());
    output->gutter = gtk_source_gutter_renderer_text_new();
    gtk_widget_add_css_class(GTK_WIDGET(output->gutter), "output-timestamps");
    gtk_source_gutter_renderer_set_xpad(output->gutter, 10);
    gtk_source_gutter_renderer_set_alignment_mode(output->gutter, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST);
    gtk_widget_set_size_request(GTK_WIDGET(output->gutter), 80, -1);
    g_signal_connect(output->gutter, "realize", G_CALLBACK(size_timestamps), NULL);
    g_signal_connect(output->gutter, "query-data", G_CALLBACK(timestamp_data), output);
    GtkSourceGutter *gutter = gtk_source_view_get_gutter(GTK_SOURCE_VIEW(output->view), GTK_TEXT_WINDOW_LEFT);
    gtk_source_gutter_insert(gutter, output->gutter, 0);
    gtk_widget_add_css_class(GTK_WIDGET(output->view), "output-text");
    gtk_accessible_update_property(GTK_ACCESSIBLE(output->view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Script output", -1);
    gtk_text_view_set_editable(output->view, FALSE);
    gtk_text_view_set_cursor_visible(output->view, FALSE);
    gtk_text_view_set_wrap_mode(output->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(output->view, TRUE);
    gtk_text_view_set_left_margin(output->view, 10);
    gtk_text_view_set_right_margin(output->view, 8);
    gtk_text_view_set_top_margin(output->view, 8);
    gtk_text_view_set_bottom_margin(output->view, 8);
    gtk_text_view_set_pixels_above_lines(output->view, 1);
    gtk_text_view_set_pixels_below_lines(output->view, 2);
    gtk_text_view_set_pixels_inside_wrap(output->view, 3);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(output->view));
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
    g_signal_connect(wheel, "scroll", G_CALLBACK(scroll_input), output);
    gtk_widget_add_controller(scroll, wheel);
    output->scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_signal_connect(output->scroll, "changed", G_CALLBACK(scroll_changed), output);
    g_signal_connect(output->scroll, "value-changed", G_CALLBACK(position_changed), output);

    output->jump = gtk_button_new();
    gtk_widget_add_css_class(output->jump, "output-jump");
    gtk_widget_set_tooltip_text(output->jump, "Jump to the latest line");
    gtk_accessible_update_property(GTK_ACCESSIBLE(output->jump),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Jump to the latest line", -1);
    GtkWidget *jump_icon = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/caret-down.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(jump_icon), 14);
    gtk_button_set_child(GTK_BUTTON(output->jump), jump_icon);
    gtk_widget_set_halign(output->jump, GTK_ALIGN_END);
    gtk_widget_set_valign(output->jump, GTK_ALIGN_END);
    gtk_widget_set_margin_end(output->jump, 14);
    gtk_widget_set_margin_bottom(output->jump, 10);
    gtk_widget_set_visible(output->jump, FALSE);
    g_signal_connect(output->jump, "clicked", G_CALLBACK(jump_to_latest), output);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), output->jump);
    return pane;
}
