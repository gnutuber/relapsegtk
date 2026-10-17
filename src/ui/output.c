#include "output.h"

typedef struct {
    GtkTextView *view;
    GtkTextBuffer *buffer;
    GtkLabel *status;
    GtkAdjustment *scroll;
    gboolean adjusting;
    GtkToggleButton *follow;
} Output;

static void clear_output(GtkButton *button, gpointer user_data)
{
    (void)button;
    Output *output = user_data;
    gtk_text_buffer_set_text(output->buffer, "", 0);
    gtk_toggle_button_set_active(output->follow, TRUE);
}

static void copy_output(GtkButton *button, gpointer user_data)
{
    Output *output = user_data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(output->buffer, &start, &end);
    g_autofree char *text = gtk_text_buffer_get_text(output->buffer, &start, &end, FALSE);
    if (!text || !*text) return;
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(button));
    gdk_clipboard_set_text(clipboard, text);
}

static void scroll_changed(GtkAdjustment *scroll, gpointer user_data)
{
    Output *output = user_data;
    if (output->adjusting || !gtk_toggle_button_get_active(output->follow)) return;
    output->adjusting = TRUE;
    gtk_adjustment_set_value(scroll, MAX(0, gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll)));
    output->adjusting = FALSE;
}

static void follow_changed(GtkToggleButton *button, gpointer user_data)
{
    (void)button;
    Output *output = user_data;
    scroll_changed(output->scroll, output);
}

void output_set_status(GtkWidget *pane, const char *text)
{
    Output *output = g_object_get_data(G_OBJECT(pane), "output");
    gtk_label_set_text(output->status, text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(output->status), text);
}

void output_append(GtkWidget *pane, const char *level, const char *text)
{
    Output *output = g_object_get_data(G_OBJECT(pane), "output");
    g_autofree char *valid = g_utf8_make_valid(text, -1);
    if (g_utf8_strlen(valid, -1) > 4096)
        *g_utf8_offset_to_pointer(valid, 4096) = '\0';
    const char *tag = g_str_equal(level, "warn") ? "warn" :
                      g_str_equal(level, "error") ? "error" : "info";
    const char *prefix = g_str_equal(level, "print") ? "" :
                         g_str_equal(tag, "warn") ? "[warn] " :
                         g_str_equal(tag, "error") ? "[error] " : "[info] ";
    g_autofree char *line = g_strconcat(prefix, valid, "\n", NULL);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(output->buffer, &end);
    gtk_text_buffer_insert_with_tags_by_name(output->buffer, &end, line, -1, tag, NULL);
    GtkTextIter first, cutoff;
    gtk_text_buffer_get_start_iter(output->buffer, &first);
    int lines = gtk_text_buffer_get_line_count(output->buffer);
    int chars = gtk_text_buffer_get_char_count(output->buffer);
    if (lines > 2000 || chars > 262144) {
        gtk_text_buffer_get_iter_at_offset(output->buffer, &cutoff, MAX(0, chars - 262144));
        if (gtk_text_iter_get_line(&cutoff) < lines - 2000)
            gtk_text_buffer_get_iter_at_line(output->buffer, &cutoff, lines - 2000);
        else if (!gtk_text_iter_starts_line(&cutoff))
            gtk_text_iter_forward_line(&cutoff);
        gtk_text_buffer_delete(output->buffer, &first, &cutoff);
    }
}

GtkWidget *output_new(void)
{
    Output *output = g_new0(Output, 1);
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(pane, "output-pane");
    g_object_set_data_full(G_OBJECT(pane), "output", output, g_free);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "output-header");
    GtkWidget *title = gtk_label_new("Output");
    gtk_widget_add_css_class(title, "output-title");
    gtk_box_append(GTK_BOX(header), title);
    output->status = GTK_LABEL(gtk_label_new("Waiting for a game"));
    gtk_widget_add_css_class(GTK_WIDGET(output->status), "output-status");
    gtk_label_set_ellipsize(output->status, PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(output->status, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(output->status), TRUE);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(output->status));
    output->follow = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Follow"));
    gtk_widget_add_css_class(GTK_WIDGET(output->follow), "output-follow");
    gtk_widget_set_tooltip_text(GTK_WIDGET(output->follow), "Follow new messages; turn off to read earlier output");
    gtk_toggle_button_set_active(output->follow, TRUE);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(output->follow));
    GtkWidget *copy = gtk_button_new_with_label("Copy");
    gtk_widget_add_css_class(copy, "output-copy");
    gtk_widget_set_tooltip_text(copy, "Copy all output to the clipboard");
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_output), output);
    gtk_box_append(GTK_BOX(header), copy);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(clear, "output-clear");
    gtk_widget_set_tooltip_text(clear, "Clear displayed output");
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_output), output);
    gtk_box_append(GTK_BOX(header), clear);
    gtk_box_append(GTK_BOX(pane), header);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 48);
    gtk_box_append(GTK_BOX(pane), scroll);
    output->view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(output->view), "output-text");
    gtk_accessible_update_property(GTK_ACCESSIBLE(output->view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Script output", -1);
    gtk_text_view_set_editable(output->view, FALSE);
    gtk_text_view_set_cursor_visible(output->view, FALSE);
    gtk_text_view_set_wrap_mode(output->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(output->view, TRUE);
    gtk_text_view_set_left_margin(output->view, 8);
    gtk_text_view_set_right_margin(output->view, 8);
    gtk_text_view_set_top_margin(output->view, 6);
    gtk_text_view_set_bottom_margin(output->view, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(output->view));
    output->buffer = gtk_text_view_get_buffer(output->view);
    gtk_text_buffer_set_enable_undo(output->buffer, FALSE);
    gtk_text_buffer_create_tag(output->buffer, "info", "foreground", "#d8dbe0", NULL);
    gtk_text_buffer_create_tag(output->buffer, "warn", "foreground", "#e3bd7a", NULL);
    gtk_text_buffer_create_tag(output->buffer, "error", "foreground", "#e28b8b", NULL);
    output->scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_signal_connect_after(output->scroll, "changed", G_CALLBACK(scroll_changed), output);
    g_signal_connect_after(output->scroll, "value-changed", G_CALLBACK(scroll_changed), output);
    g_signal_connect(output->follow, "toggled", G_CALLBACK(follow_changed), output);
    return pane;
}
