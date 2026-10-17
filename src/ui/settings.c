#include "settings.h"

typedef struct {
    GtkSpinButton *font_size;
    GtkSwitch *word_wrap;
    GtkToggleButton *indent[3];
    GtkSwitch *restore_tabs;
    GtkSwitch *output;
    GtkSpinButton *mcp_port;
    GtkSwitch *mcp_expose_execute;
    GtkSwitch *mcp_expose_logs;
    GtkEntry *mcp_token;
    gboolean updating;
    SettingsChangedFunc changed;
    RestoreTabsChangedFunc restore_changed;
    OutputChangedFunc output_changed;
    McpChangedFunc mcp_changed;
    guint mcp_debounce;
    gpointer user_data;
} Settings;

static const int indent_widths[] = {2, 4, 8};

void settings_set_output(GtkWidget *page, gboolean enabled)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_switch_set_active(state->output, enabled);
    state->updating = FALSE;
}

static void output_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Settings *state = user_data;
    if (!state->updating && state->output_changed)
        state->output_changed(gtk_switch_get_active(GTK_SWITCH(object)), state->user_data);
}

void settings_set_restore_tabs(GtkWidget *page, gboolean enabled)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_switch_set_active(state->restore_tabs, enabled);
    state->updating = FALSE;
}

static void restore_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Settings *state = user_data;
    if (!state->updating && state->restore_changed)
        state->restore_changed(gtk_switch_get_active(GTK_SWITCH(object)), state->user_data);
}

void settings_set_editor(GtkWidget *page, const EditorSettings *settings)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_spin_button_set_value(state->font_size, settings->font_size);
    gtk_switch_set_active(state->word_wrap, settings->word_wrap);
    for (guint i = 0; i < G_N_ELEMENTS(indent_widths); ++i)
        if (settings->indent_width == indent_widths[i])
            gtk_toggle_button_set_active(state->indent[i], TRUE);
    state->updating = FALSE;
}

static void values_changed(GtkWidget *widget, gpointer user_data)
{
    Settings *state = user_data;
    if (state->updating || !state->changed ||
        (GTK_IS_TOGGLE_BUTTON(widget) && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))))
        return;
    EditorSettings settings = {
        .font_size = gtk_spin_button_get_value_as_int(state->font_size),
        .word_wrap = gtk_switch_get_active(state->word_wrap),
        .indent_width = 4
    };
    for (guint i = 0; i < G_N_ELEMENTS(indent_widths); ++i)
        if (gtk_toggle_button_get_active(state->indent[i]))
            settings.indent_width = indent_widths[i];
    state->changed(&settings, state->user_data);
}

static void wrap_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    values_changed(GTK_WIDGET(object), user_data);
}

static void emit_mcp(Settings *state)
{
    if (state->updating || !state->mcp_changed) return;
    McpSettings s = {
        .port = gtk_spin_button_get_value_as_int(state->mcp_port),
        .expose_execute = gtk_switch_get_active(state->mcp_expose_execute),
        .expose_logs = gtk_switch_get_active(state->mcp_expose_logs),
        .token = (char *)gtk_editable_get_text(GTK_EDITABLE(state->mcp_token)),
    };
    state->mcp_changed(&s, state->user_data);
}

static gboolean mcp_flush(gpointer user_data)
{
    Settings *state = user_data;
    state->mcp_debounce = 0;
    emit_mcp(state);
    return G_SOURCE_REMOVE;
}

static void mcp_notify(Settings *state, guint delay_ms)
{
    if (state->updating) return;
    if (state->mcp_debounce) g_source_remove(state->mcp_debounce);
    state->mcp_debounce = g_timeout_add(delay_ms, mcp_flush, state);
}

static void mcp_switch_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object; (void)pspec;
    mcp_notify(user_data, 0);
}

static void mcp_value_changed(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    mcp_notify(user_data, 250);
}

static void mcp_token_changed(GtkEditable *entry, gpointer user_data)
{
    (void)entry;
    mcp_notify(user_data, 400);
}

void settings_set_mcp(GtkWidget *page, const McpSettings *s)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (!state || !s) return;
    state->updating = TRUE;
    gtk_spin_button_set_value(state->mcp_port, s->port);
    gtk_switch_set_active(state->mcp_expose_execute, s->expose_execute);
    gtk_switch_set_active(state->mcp_expose_logs, s->expose_logs);
    gtk_editable_set_text(GTK_EDITABLE(state->mcp_token), s->token ? s->token : "");
    state->updating = FALSE;
}

static GtkWidget *label_new(const char *text, const char *style)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (style)
        gtk_widget_add_css_class(label, style);
    return label;
}

static void add_row(GtkWidget *group, const char *title, const char *description, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class(row, "settings-row");
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(labels, TRUE);
    gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(labels), label_new(title, "settings-label"));
    GtkWidget *detail = label_new(description, "settings-description");
    gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(detail), 33);
    gtk_box_append(GTK_BOX(labels), detail);
    gtk_box_append(GTK_BOX(row), labels);
    GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(slot, 132, -1);
    gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(slot), control);
    gtk_box_append(GTK_BOX(row), slot);
    gtk_box_append(GTK_BOX(group), row);
}

static GtkWidget *section_new(GtkWidget *column, const char *title, GtkWidget **out_group)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(section, TRUE);
    gtk_box_append(GTK_BOX(column), section);
    gtk_box_append(GTK_BOX(section), label_new(title, "settings-group-title"));
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(group, "settings-group");
    gtk_widget_set_hexpand(group, TRUE);
    gtk_box_append(GTK_BOX(section), group);
    *out_group = group;
    return section;
}

GtkWidget *settings_new(const EditorSettings *settings, SettingsChangedFunc changed,
                       RestoreTabsChangedFunc restore_callback, OutputChangedFunc output_callback,
                       McpChangedFunc mcp_callback, gpointer user_data)
{
    Settings *state = g_new0(Settings, 1);
    state->changed = changed;
    state->restore_changed = restore_callback;
    state->output_changed = output_callback;
    state->mcp_changed = mcp_callback;
    state->user_data = user_data;
    GtkWidget *page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_object_set_data_full(G_OBJECT(page), "settings", state, g_free);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(content, "settings-page");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(page), content);
    gtk_box_append(GTK_BOX(content), label_new("Settings", "settings-title"));

    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_add_css_class(columns, "settings-columns");
    gtk_widget_set_hexpand(columns, TRUE);
    gtk_box_append(GTK_BOX(content), columns);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_valign(left, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(columns), left);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_valign(right, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(columns), right);

    GtkWidget *editor_group = NULL;
    section_new(left, "Editor", &editor_group);

    GtkWidget *font = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->font_size = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(
        EDITOR_FONT_SIZE_MIN, EDITOR_FONT_SIZE_MAX, 1));
    gtk_widget_add_css_class(GTK_WIDGET(state->font_size), "font-size");
    gtk_spin_button_set_numeric(state->font_size, TRUE);
    gtk_spin_button_set_update_policy(state->font_size, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->font_size), 2);
    gtk_editable_set_alignment(GTK_EDITABLE(state->font_size), 0.5);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->font_size),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Editor font size in pixels", -1);
    gtk_box_append(GTK_BOX(font), GTK_WIDGET(state->font_size));
    gtk_box_append(GTK_BOX(font), label_new("px", "settings-description"));
    add_row(editor_group, "Font size", "Editor text and line numbers", font);

    state->word_wrap = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->word_wrap), "word-wrap");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->word_wrap),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Word wrap", -1);
    add_row(editor_group, "Word wrap", "Wrap long lines to fit the editor", GTK_WIDGET(state->word_wrap));

    GtkWidget *indent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(indent, "indent-options");
    for (guint i = 0; i < G_N_ELEMENTS(indent_widths); ++i) {
        g_autofree char *text = g_strdup_printf("%d", indent_widths[i]);
        state->indent[i] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(text));
        if (i)
            gtk_toggle_button_set_group(state->indent[i], state->indent[0]);
        g_autofree char *label = g_strdup_printf("Indent with %d spaces", indent_widths[i]);
        gtk_accessible_update_property(GTK_ACCESSIBLE(state->indent[i]),
            GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
        gtk_box_append(GTK_BOX(indent), GTK_WIDGET(state->indent[i]));
        g_signal_connect(state->indent[i], "toggled", G_CALLBACK(values_changed), state);
    }
    add_row(editor_group, "Indentation", "Spaces used by Tab and auto-indent", indent);
    settings_set_editor(page, settings);
    g_signal_connect(state->font_size, "value-changed", G_CALLBACK(values_changed), state);
    g_signal_connect(state->word_wrap, "notify::active", G_CALLBACK(wrap_changed), state);

    GtkWidget *session_group = NULL;
    section_new(left, "Session", &session_group);
    state->restore_tabs = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->restore_tabs), "restore-tabs");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restore_tabs),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Restore tabs on startup", -1);
    gtk_switch_set_active(state->restore_tabs, TRUE);
    add_row(session_group, "Restore tabs on startup", "Reopen tabs and unsaved drafts", GTK_WIDGET(state->restore_tabs));
    g_signal_connect(state->restore_tabs, "notify::active", G_CALLBACK(restore_changed), state);

    GtkWidget *output_group = NULL;
    section_new(left, "Output", &output_group);
    state->output = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->output), "redirect-output");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->output),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Redirect script output", -1);
    add_row(output_group, "Redirect script output", "Show your scripts’ output below the editor", GTK_WIDGET(state->output));
    g_signal_connect(state->output, "notify::active", G_CALLBACK(output_changed), state);

    GtkWidget *mcp_group = NULL;
    section_new(right, "MCP server", &mcp_group);

    state->mcp_port = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_widget_add_css_class(GTK_WIDGET(state->mcp_port), "mcp-port");
    gtk_spin_button_set_numeric(state->mcp_port, TRUE);
    gtk_spin_button_set_value(state->mcp_port, 7331);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->mcp_port), 5);
    gtk_editable_set_alignment(GTK_EDITABLE(state->mcp_port), 0.5);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->mcp_port),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "MCP server port", -1);
    add_row(mcp_group, "Port", "TCP port for local MCP clients", GTK_WIDGET(state->mcp_port));

    state->mcp_expose_execute = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->mcp_expose_execute), "mcp-execute");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->mcp_expose_execute),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Expose execute over MCP", -1);
    add_row(mcp_group, "Expose execute", "Let MCP clients run scripts", GTK_WIDGET(state->mcp_expose_execute));

    state->mcp_expose_logs = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->mcp_expose_logs), "mcp-logs");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->mcp_expose_logs),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Expose logs over MCP", -1);
    add_row(mcp_group, "Expose logs", "Stream script output to MCP clients", GTK_WIDGET(state->mcp_expose_logs));

    state->mcp_token = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->mcp_token), "mcp-token");
    gtk_entry_set_visibility(state->mcp_token, FALSE);
    gtk_entry_set_input_purpose(state->mcp_token, GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_placeholder_text(state->mcp_token, "unset");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->mcp_token), 14);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->mcp_token),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "MCP bearer token", -1);
    add_row(mcp_group, "Bearer token", "Required in the Authorization header", GTK_WIDGET(state->mcp_token));

    g_signal_connect(state->mcp_port, "value-changed", G_CALLBACK(mcp_value_changed), state);
    g_signal_connect(state->mcp_expose_execute, "notify::active", G_CALLBACK(mcp_switch_changed), state);
    g_signal_connect(state->mcp_expose_logs, "notify::active", G_CALLBACK(mcp_switch_changed), state);
    g_signal_connect(state->mcp_token, "changed", G_CALLBACK(mcp_token_changed), state);

    return page;
}
