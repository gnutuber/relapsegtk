#include "settings.h"

#include <glib/gstdio.h>
#include <unistd.h>

#include "settings/profiles.h"

typedef struct {
    GtkSpinButton *font_size;
    GtkSwitch *word_wrap;
    GtkToggleButton *indent[3];
    GtkSwitch *restore_tabs;
    GtkSwitch *output;
    gboolean updating;
    SettingsChangedFunc changed;
    RestoreTabsChangedFunc restore_changed;
    OutputChangedFunc output_changed;
    GtkWidget *launcher_button;
    GtkLabel *launcher_path;
    GCancellable *picker;
    LauncherChangedFunc launcher_changed;
    GtkDropDown *open_with;
    GtkEntry *open_command;
    GtkWidget *open_command_row;
    OpenWithChangedFunc open_with_changed;
    GtkSpinButton *launch_delay;
    GtkWidget *exclude_button;
    GtkLabel *exclude_summary;
    GtkWindow *exclude_dialog;
    GtkListBox *exclude_list;
    char **excluded;
    char **profiles;
    ProfilesChangedFunc profiles_changed;
    CheckUpdatesFunc check_updates;
    gboolean stopped;
    gpointer user_data;
} Settings;

static const int indent_widths[] = {2, 4, 8};

static GtkWidget *label_new(const char *text, const char *style);
static void exclude_saved(GtkButton *button, gpointer user_data);

void settings_set_launcher(GtkWidget *page, const char *path)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    g_autofree char *name = path && *path ? g_filename_display_basename(path) : g_strdup("None selected");
    gtk_label_set_text(state->launcher_path, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->launcher_path), path);
}

static void launcher_chosen(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, &error);
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        g_clear_object(&state->picker);
        gtk_widget_set_sensitive(state->launcher_button, TRUE);
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR) || g_access(path, X_OK) != 0)
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a local executable file.");
            else if (!state->launcher_changed || state->launcher_changed(path, &error, state->user_data))
                settings_set_launcher(page, path);
        }
        if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not select launcher");
            gtk_alert_dialog_set_detail(alert, error->message);
            gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(page)));
        }
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void select_launcher(GtkButton *button, gpointer user_data)
{
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped || state->picker) return;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select launcher");
    gtk_file_dialog_set_accept_label(dialog, "Select");
    gtk_file_dialog_set_modal(dialog, TRUE);
    state->picker = g_cancellable_new();
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, page);
    gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(page)), state->picker, launcher_chosen, owner);
}

static const char *open_with_labels[] = {
    "Editor tab", "File manager", "System default", "Custom command", "Nothing",
};

static void update_open_command_row(Settings *state)
{
    guint selected = gtk_drop_down_get_selected(state->open_with);
    gtk_widget_set_visible(state->open_command_row, selected == OPENER_COMMAND);
}

static void commit_open_with(Settings *state)
{
    if (!state->open_with_changed)
        return;
    guint selected = gtk_drop_down_get_selected(state->open_with);
    OpenerMode mode = selected < G_N_ELEMENTS(open_with_labels) ? (OpenerMode)selected : OPENER_EDITOR;
    g_autoptr(GError) error = NULL;
    if (!state->open_with_changed(mode, gtk_editable_get_text(GTK_EDITABLE(state->open_command)),
                                  &error, state->user_data) && error) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save settings");
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(state->open_with))));
    }
}

static void open_with_selected(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    Settings *state = user_data;
    if (state->updating)
        return;
    update_open_command_row(state);
    commit_open_with(state);
}

static void open_command_committed(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    Settings *state = user_data;
    if (state->updating)
        return;
    commit_open_with(state);
}

void settings_set_open_with(GtkWidget *page, OpenerMode mode, const char *command)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_drop_down_set_selected(state->open_with,
        mode < G_N_ELEMENTS(open_with_labels) ? (guint)mode : (guint)OPENER_EDITOR);
    gtk_editable_set_text(GTK_EDITABLE(state->open_command), command ? command : "");
    state->updating = FALSE;
    update_open_command_row(state);
}

static void update_exclude_summary(Settings *state)
{
    if (!state->exclude_button)
        return;
    gboolean have = state->profiles && state->profiles[0] && !state->stopped;
    gtk_widget_set_sensitive(state->exclude_button, have);
    if (!state->exclude_summary)
        return;
    if (!have) {
        gtk_label_set_text(state->exclude_summary, "Create a Profile to skip it here.");
        return;
    }
    g_autoptr(GPtrArray) skipped = g_ptr_array_new();
    for (guint i = 0; state->profiles[i]; ++i)
        if (state->excluded &&
            g_strv_contains((const char *const *)state->excluded, state->profiles[i]))
            g_ptr_array_add(skipped, state->profiles[i]);
    if (!skipped->len) {
        gtk_label_set_text(state->exclude_summary, "Every Profile is launched.");
        return;
    }
    g_ptr_array_add(skipped, NULL);
    g_autofree char *joined = g_strjoinv(", ", (char **)skipped->pdata);
    g_autofree char *text = g_strdup_printf("Skipped: %s", joined);
    gtk_label_set_text(state->exclude_summary, text);
}

static void commit_profiles(Settings *state)
{
    if (!state->profiles_changed)
        return;
    g_autoptr(GError) error = NULL;
    if (!state->profiles_changed(gtk_spin_button_get_value_as_int(state->launch_delay),
            (const char *const *)state->excluded, &error, state->user_data) && error) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save settings");
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(state->launch_delay))));
    }
}

static GtkWidget *exclude_row_new(const char *name, gboolean skipped)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = label_new(name, NULL);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *launch = gtk_switch_new();
    gtk_widget_add_css_class(launch, "exclude-profile");
    gtk_widget_set_valign(launch, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(launch), !skipped);
    g_object_set_data_full(G_OBJECT(launch), "profile-name", g_strdup(name), g_free);
    g_autofree char *accessible = g_strdup_printf("Launch %s with Launch all", name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(launch), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
    gtk_widget_set_tooltip_text(launch, "Include this Profile when Launch all runs");
    gtk_box_append(GTK_BOX(box), launch);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

static void exclude_saved(GtkButton *button, gpointer user_data)
{
    (void)button;
    Settings *state = user_data;
    g_autoptr(GPtrArray) skipped = g_ptr_array_new_with_free_func(g_free);
    for (GtkWidget *row = state->exclude_list ? gtk_widget_get_first_child(GTK_WIDGET(state->exclude_list)) : NULL;
         row; row = gtk_widget_get_next_sibling(row)) {
        GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        GtkWidget *launch = box ? gtk_widget_get_last_child(box) : NULL;
        if (!launch || !GTK_IS_SWITCH(launch) || gtk_switch_get_active(GTK_SWITCH(launch)))
            continue;
        const char *name = g_object_get_data(G_OBJECT(launch), "profile-name");
        if (name)
            g_ptr_array_add(skipped, g_strdup(name));
    }
    g_ptr_array_add(skipped, NULL);
    g_strfreev(state->excluded);
    state->excluded = (char **)g_ptr_array_free(g_steal_pointer(&skipped), FALSE);
    update_exclude_summary(state);
    commit_profiles(state);
    if (state->exclude_dialog)
        gtk_window_destroy(state->exclude_dialog);
}

static gboolean exclude_dialog_key(GtkEventControllerKey *controller, guint keyval,
                                   guint keycode, GdkModifierType modifiers, gpointer user_data)
{
    (void)controller; (void)keycode; (void)modifiers;
    if (keyval != GDK_KEY_Escape) return FALSE;
    gtk_window_close(GTK_WINDOW(user_data));
    return TRUE;
}

static void exclude_dialog_closed(GtkWidget *widget, gpointer user_data)
{
    Settings *state = user_data;
    if (state->exclude_dialog == GTK_WINDOW(widget))
        state->exclude_dialog = NULL;
    state->exclude_list = NULL;
}

static void open_exclude_dialog(GtkButton *button, gpointer user_data)
{
    (void)button;
    Settings *state = user_data;
    if (state->stopped || !state->profiles || !state->profiles[0])
        return;
    if (state->exclude_dialog) {
        gtk_window_present(state->exclude_dialog);
        return;
    }
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window, "Skip when launching all");
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
    if (parent)
        gtk_window_set_transient_for(window, parent);
    gtk_window_set_modal(window, TRUE);
    gtk_window_set_hide_on_close(window, TRUE);
    gtk_window_set_default_size(window, 340, 380);
    gtk_widget_add_css_class(GTK_WIDGET(window), "relapse-ui");
    gtk_widget_add_css_class(GTK_WIDGET(window), "exclude-dialog");
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    GtkWidget *title = gtk_label_new("Skip when launching all");
    gtk_widget_add_css_class(title, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    GtkWidget *close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close, "window-control");
    gtk_widget_set_tooltip_text(close, "Close without saving");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), close);
    gtk_window_set_titlebar(window, header);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(content, "profiles-content");
    GtkWidget *hint = label_new("Launch all starts every Profile whose switch is on.",
        "settings-description");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 40);
    gtk_box_append(GTK_BOX(content), hint);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 140);
    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "profiles-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    for (guint i = 0; state->profiles[i]; ++i) {
        gboolean skipped = state->excluded &&
            g_strv_contains((const char *const *)state->excluded, state->profiles[i]);
        gtk_list_box_append(GTK_LIST_BOX(list), exclude_row_new(state->profiles[i], skipped));
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(content), scroll);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_close), window);
    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "save-excluded");
    g_signal_connect(save, "clicked", G_CALLBACK(exclude_saved), state);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(content), actions);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(exclude_dialog_key), window);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);
    gtk_window_set_child(window, content);
    g_signal_connect(window, "destroy", G_CALLBACK(exclude_dialog_closed), state);
    state->exclude_dialog = window;
    state->exclude_list = GTK_LIST_BOX(list);
    gtk_window_present(window);
}

static void launch_delay_changed(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    Settings *state = user_data;
    if (!state->updating)
        commit_profiles(state);
}

void settings_set_profiles_options(GtkWidget *page, gint delay_ms, const char *const *excluded)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_spin_button_set_value(state->launch_delay,
        CLAMP(delay_ms, PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX));
    g_strfreev(state->excluded);
    state->excluded = g_strdupv((char **)excluded);
    if (!state->excluded)
        state->excluded = g_new0(char *, 1);
    state->updating = FALSE;
    update_exclude_summary(state);
}

void settings_set_profiles(GtkWidget *page, const char *const *names)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped)
        return;
    if (state->profiles && names &&
        g_strv_equal((const char *const *)state->profiles, names))
        return;
    g_strfreev(state->profiles);
    state->profiles = g_strdupv((char **)names);
    update_exclude_summary(state);
}

void settings_check_updates(GtkWidget *page)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (!state->stopped && state->check_updates)
        state->check_updates(state->user_data);
}

void settings_shutdown(GtkWidget *page)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->stopped = TRUE;
    if (state->picker) g_cancellable_cancel(state->picker);
}

static void free_settings(gpointer user_data)
{
    Settings *state = user_data;
    if (state->picker) g_cancellable_cancel(state->picker);
    g_clear_object(&state->picker);
    if (state->exclude_dialog) {
        gtk_window_destroy(state->exclude_dialog);
        state->exclude_dialog = NULL;
        state->exclude_list = NULL;
    }
    g_strfreev(state->excluded);
    g_strfreev(state->profiles);
    g_free(state);
}

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

static GtkWidget *label_new(const char *text, const char *style)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (style)
        gtk_widget_add_css_class(label, style);
    return label;
}

static GtkWidget *add_row(GtkWidget *group, const char *title, const char *description, GtkWidget *control)
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
    return row;
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
                       LauncherChangedFunc launcher_callback, OpenWithChangedFunc open_with_callback,
                       ProfilesChangedFunc profiles_callback, CheckUpdatesFunc check_updates,
                       const char *version, gpointer user_data)
{
    Settings *state = g_new0(Settings, 1);
    state->changed = changed;
    state->restore_changed = restore_callback;
    state->output_changed = output_callback;
    state->launcher_changed = launcher_callback;
    state->open_with_changed = open_with_callback;
    state->profiles_changed = profiles_callback;
    state->check_updates = check_updates;
    state->user_data = user_data;
    GtkWidget *page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_object_set_data_full(G_OBJECT(page), "settings", state, free_settings);
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

    GtkWidget *launcher_group = NULL;
    section_new(right, "Profiles", &launcher_group);
    GtkWidget *launcher = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    state->launcher_button = gtk_button_new_with_label("Select…");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->launcher_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Select launcher", -1);
    gtk_box_append(GTK_BOX(launcher), state->launcher_button);
    state->launcher_path = GTK_LABEL(gtk_label_new("None selected"));
    gtk_label_set_ellipsize(state->launcher_path, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(state->launcher_path, 16);
    gtk_widget_add_css_class(GTK_WIDGET(state->launcher_path), "settings-description");
    gtk_box_append(GTK_BOX(launcher), GTK_WIDGET(state->launcher_path));
    add_row(launcher_group, "Launcher", "Executable used to launch Profiles", launcher);
    g_signal_connect(state->launcher_button, "clicked", G_CALLBACK(select_launcher), page);

    GtkWidget *delay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->launch_delay = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(
        PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX, 100));
    gtk_widget_add_css_class(GTK_WIDGET(state->launch_delay), "launch-delay");
    gtk_spin_button_set_numeric(state->launch_delay, TRUE);
    gtk_spin_button_set_update_policy(state->launch_delay, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->launch_delay), 5);
    gtk_editable_set_alignment(GTK_EDITABLE(state->launch_delay), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->launch_delay),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Delay between launches in milliseconds", -1);
    gtk_box_append(GTK_BOX(delay), GTK_WIDGET(state->launch_delay));
    gtk_box_append(GTK_BOX(delay), label_new("ms", "settings-description"));
    add_row(launcher_group, "Launch all delay", "Pause between each Profile launch", delay);
    g_signal_connect(state->launch_delay, "value-changed", G_CALLBACK(launch_delay_changed), state);

    GtkWidget *exclude_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class(exclude_row, "settings-row");
    GtkWidget *exclude_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(exclude_labels, TRUE);
    gtk_widget_set_valign(exclude_labels, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(exclude_labels), label_new("Skip when launching all", "settings-label"));
    state->exclude_summary = GTK_LABEL(label_new("Create a Profile to skip it here.",
        "settings-description"));
    gtk_label_set_wrap(state->exclude_summary, TRUE);
    gtk_label_set_max_width_chars(state->exclude_summary, 33);
    gtk_label_set_ellipsize(state->exclude_summary, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(exclude_labels), GTK_WIDGET(state->exclude_summary));
    gtk_box_append(GTK_BOX(exclude_row), exclude_labels);
    GtkWidget *exclude_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(exclude_slot, 132, -1);
    gtk_widget_set_valign(exclude_slot, GTK_ALIGN_CENTER);
    state->exclude_button = gtk_button_new_with_label("Configure");
    gtk_widget_add_css_class(state->exclude_button, "configure-excluded");
    gtk_widget_set_halign(state->exclude_button, GTK_ALIGN_END);
    gtk_widget_set_sensitive(state->exclude_button, FALSE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->exclude_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Choose which Profiles Launch all starts", -1);
    gtk_box_append(GTK_BOX(exclude_slot), state->exclude_button);
    gtk_box_append(GTK_BOX(exclude_row), exclude_slot);
    g_signal_connect(state->exclude_button, "clicked", G_CALLBACK(open_exclude_dialog), state);
    gtk_box_append(GTK_BOX(launcher_group), exclude_row);

    GtkWidget *scripts_group = NULL;
    section_new(right, "Scripts", &scripts_group);
    GtkStringList *open_items = gtk_string_list_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(open_with_labels); ++i)
        gtk_string_list_append(open_items, open_with_labels[i]);
    state->open_with = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(open_items), NULL));
    gtk_widget_add_css_class(GTK_WIDGET(state->open_with), "open-with");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->open_with),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Open scripts with", -1);
    gtk_widget_set_valign(GTK_WIDGET(state->open_with), GTK_ALIGN_CENTER);
    add_row(scripts_group, "Open scripts", "Where clicking a tab name takes you", GTK_WIDGET(state->open_with));
    g_signal_connect(state->open_with, "notify::selected", G_CALLBACK(open_with_selected), state);

    state->open_command = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->open_command, "code %s");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->open_command),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "External editor command", -1);
    state->open_command_row = add_row(scripts_group, "Editor command",
        "Command run with the script path; %s is replaced", GTK_WIDGET(state->open_command));
    g_signal_connect(state->open_command, "activate", G_CALLBACK(open_command_committed), state);
    GtkEventController *command_focus = gtk_event_controller_focus_new();
    g_signal_connect(command_focus, "leave", G_CALLBACK(open_command_committed), state);
    gtk_widget_add_controller(GTK_WIDGET(state->open_command), command_focus);
    update_open_command_row(state);

    GtkWidget *about_group = NULL;
    section_new(right, "Updates", &about_group);
    GtkWidget *update_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(update_box, GTK_ALIGN_CENTER);
    g_autofree char *running = g_strdup_printf("You are running %s", version);
    gtk_box_append(GTK_BOX(update_box), label_new(running, "settings-description"));
    GtkWidget *check = gtk_button_new_with_label("Check now");
    gtk_widget_add_css_class(check, "check-updates");
    gtk_accessible_update_property(GTK_ACCESSIBLE(check),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Check for updates", -1);
    g_signal_connect_swapped(check, "clicked", G_CALLBACK(settings_check_updates), page);
    gtk_box_append(GTK_BOX(update_box), check);
    add_row(about_group, "Check for updates", "Compare with the latest stable release", update_box);

    return page;
}
