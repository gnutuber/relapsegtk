#include "search.h"

#include <gtksourceview/gtksource.h>

typedef struct {
    GtkTextView *editor;
    GtkSourceSearchContext *context;
    GtkSearchEntry *entry;
    GtkLabel *count;
    GtkWidget *previous;
    GtkWidget *next;
} Search;

static Search *search_data(GtkWidget *widget)
{
    return g_object_get_data(G_OBJECT(widget), "search");
}

static void update_count(GtkWidget *widget)
{
    Search *search = search_data(widget);
    GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search->context);
    const char *query = gtk_source_search_settings_get_search_text(settings);
    int count = gtk_source_search_context_get_occurrences_count(search->context);
    int position = 0;
    GtkTextIter start, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(search->editor);
    if (count > 0 && gtk_text_buffer_get_selection_bounds(buffer, &start, &end))
        position = gtk_source_search_context_get_occurrence_position(search->context, &start, &end);
    g_autofree char *label = !query || !*query ? g_strdup("") :
        count < 0 ? g_strdup("Searching…") :
        count == 0 ? g_strdup("No matches") :
        position > 0 ? g_strdup_printf("%d / %d", position, count) :
        g_strdup_printf("%d match%s", count, count == 1 ? "" : "es");
    gtk_label_set_text(search->count, label);
    gtk_widget_set_sensitive(search->previous, query && *query && count != 0);
    gtk_widget_set_sensitive(search->next, query && *query && count != 0);
    if (query && *query && count == 0)
        gtk_widget_add_css_class(GTK_WIDGET(search->entry), "no-matches");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(search->entry), "no-matches");
}

static void find_match(GtkWidget *widget, gboolean backward, gboolean include_selection)
{
    Search *search = search_data(widget);
    GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search->context);
    const char *query = gtk_source_search_settings_get_search_text(settings);
    if (!query || !*query)
        return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(search->editor);
    GtkTextIter start, end, from;
    gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
    from = backward || include_selection ? start : end;
    gboolean found = backward
        ? gtk_source_search_context_backward(search->context, &from, &start, &end, NULL)
        : gtk_source_search_context_forward(search->context, &from, &start, &end, NULL);
    if (found) {
        gtk_text_buffer_select_range(buffer, &start, &end);
        gtk_text_view_scroll_to_iter(search->editor, &start, 0.1, FALSE, 0, 0);
    }
    update_count(widget);
}

static void query_changed(GtkWidget *widget)
{
    if (!gtk_widget_get_visible(widget))
        return;
    Search *search = search_data(widget);
    GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search->context);
    gtk_source_search_settings_set_search_text(settings, gtk_editable_get_text(GTK_EDITABLE(search->entry)));
    find_match(widget, FALSE, TRUE);
    update_count(widget);
}

void search_step(GtkWidget *widget, gboolean backward)
{
    Search *search = search_data(widget);
    if (!gtk_widget_get_visible(widget)) {
        gtk_widget_set_visible(widget, TRUE);
        gtk_source_search_context_set_highlight(search->context, TRUE);
        gtk_source_search_settings_set_search_text(
            gtk_source_search_context_get_settings(search->context),
            gtk_editable_get_text(GTK_EDITABLE(search->entry)));
    }
    if (!*gtk_editable_get_text(GTK_EDITABLE(search->entry))) {
        gtk_widget_grab_focus(GTK_WIDGET(search->entry));
        return;
    }
    find_match(widget, backward, FALSE);
}

static void next_match(GtkWidget *widget)
{
    search_step(widget, FALSE);
}

static void previous_match(GtkWidget *widget)
{
    search_step(widget, TRUE);
}

static void close_search(GtkWidget *widget)
{
    Search *search = search_data(widget);
    gtk_source_search_context_set_highlight(search->context, FALSE);
    gtk_source_search_settings_set_search_text(
        gtk_source_search_context_get_settings(search->context), NULL);
    gtk_widget_set_visible(widget, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(search->editor));
}

static void case_changed(GtkToggleButton *button, GtkWidget *widget)
{
    Search *search = search_data(widget);
    gtk_source_search_settings_set_case_sensitive(
        gtk_source_search_context_get_settings(search->context), gtk_toggle_button_get_active(button));
    find_match(widget, FALSE, TRUE);
    update_count(widget);
}

static gboolean search_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, GtkWidget *widget)
{
    (void)keycode;
    if (!gtk_widget_get_visible(widget))
        return FALSE;
    if (keyval == GDK_KEY_Escape) {
        close_search(widget);
        return TRUE;
    }
    if (gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)) == widget &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && (modifiers & GDK_SHIFT_MASK)) {
        previous_match(widget);
        return TRUE;
    }
    return FALSE;
}

void search_show(GtkWidget *widget)
{
    Search *search = search_data(widget);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(search->editor);
    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end) &&
        gtk_text_iter_get_line(&start) == gtk_text_iter_get_line(&end) &&
        gtk_text_iter_get_offset(&end) - gtk_text_iter_get_offset(&start) <= 256) {
        g_autofree char *selected = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(search->entry), selected);
    }
    gtk_widget_set_visible(widget, TRUE);
    gtk_source_search_context_set_highlight(search->context, TRUE);
    query_changed(widget);
    gtk_widget_grab_focus(GTK_WIDGET(search->entry));
    gtk_editable_select_region(GTK_EDITABLE(search->entry), 0, -1);
}

static void free_search(gpointer data)
{
    Search *search = data;
    g_object_unref(search->context);
    g_object_unref(search->editor);
    g_free(search);
}

static GtkWidget *search_button(GtkWidget *bar, const char *text, const char *label, GCallback callback)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    g_signal_connect_object(button, "clicked", callback, bar, G_CONNECT_SWAPPED);
    gtk_box_append(GTK_BOX(bar), button);
    return button;
}

GtkWidget *search_new(GtkWidget *editor)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "editor-search");
    gtk_widget_set_visible(bar, FALSE);
    Search *search = g_new0(Search, 1);
    search->editor = g_object_ref(GTK_TEXT_VIEW(editor));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(search->editor);
    g_autoptr(GtkSourceSearchSettings) settings = gtk_source_search_settings_new();
    gtk_source_search_settings_set_wrap_around(settings, TRUE);
    search->context = gtk_source_search_context_new(GTK_SOURCE_BUFFER(buffer), settings);
    gtk_source_search_context_set_highlight(search->context, FALSE);
    g_object_set_data_full(G_OBJECT(bar), "search", search, free_search);
    search->entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(search->entry, "Find in script…");
    gtk_editable_set_width_chars(GTK_EDITABLE(search->entry), 8);
    gtk_widget_set_hexpand(GTK_WIDGET(search->entry), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(search->entry),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Find in script", -1);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(search->entry));
    search->count = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(search->count), "search-count");
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(search->count));
    GtkWidget *match_case = gtk_toggle_button_new_with_label("Aa");
    gtk_widget_set_tooltip_text(match_case, "Match case");
    gtk_accessible_update_property(GTK_ACCESSIBLE(match_case), GTK_ACCESSIBLE_PROPERTY_LABEL, "Match case", -1);
    g_signal_connect_object(match_case, "toggled", G_CALLBACK(case_changed), bar, 0);
    gtk_box_append(GTK_BOX(bar), match_case);
    search->previous = search_button(bar, "↑", "Previous match (Shift+Enter / Shift+F3)", G_CALLBACK(previous_match));
    search->next = search_button(bar, "↓", "Next match (Enter / F3)", G_CALLBACK(next_match));
    search_button(bar, "×", "Close search (Escape)", G_CALLBACK(close_search));
    g_signal_connect_object(search->entry, "changed", G_CALLBACK(query_changed), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->entry, "activate", G_CALLBACK(next_match), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->entry, "next-match", G_CALLBACK(next_match), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->entry, "previous-match", G_CALLBACK(previous_match), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->entry, "stop-search", G_CALLBACK(close_search), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->context, "notify::occurrences-count", G_CALLBACK(update_count), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(buffer, "notify::cursor-position", G_CALLBACK(update_count), bar, G_CONNECT_SWAPPED);
    GtkWidget *targets[] = {bar, editor};
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i) {
        GtkEventController *keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect_object(keys, "key-pressed", G_CALLBACK(search_key), bar, 0);
        gtk_widget_add_controller(targets[i], keys);
    }
    update_count(bar);
    return bar;
}
