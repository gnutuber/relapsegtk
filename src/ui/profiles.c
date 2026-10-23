#include "profiles.h"
#include "profiles/store.h"

#include <string.h>

typedef struct {
    GtkDropDown *dropdown;
    char **paths;
    gboolean updating;
    SocketSelectedFunc selected;
    gpointer user_data;
} SocketPicker;

typedef struct ProfileRow ProfileRow;

typedef struct {
    GtkWidget *widget;
    GtkWindow *dialog;
    GtkListBox *list;
    GtkLabel *error;
    GtkWidget *add;
    ProfileRow *editing;
    char **names;
    ProfileLaunchFunc launch;
    ProfileRunningFunc running;
    gpointer user_data;
} ProfilePicker;

struct ProfileRow {
    ProfilePicker *picker;
    GtkWidget *widget;
    GtkStack *name_stack;
    GtkWidget *label;
    GtkEntry *entry;
    GtkWidget *play;
    GtkWidget *rename;
    GtkWidget *save;
    GtkWidget *cancel;
    char *name;
};

static void item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
    gtk_list_item_set_child(item, label);
}

static void item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory;
    const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));
    g_autofree char *display = user_data && !g_str_equal(text, "No Instances")
        ? g_strdup_printf("Instance: %s", text) : g_strdup(text);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), display);
}

static GtkDropDown *dropdown_new(const char *label)
{
    GtkDropDown *dropdown = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_drop_down_set_enable_search(dropdown, FALSE);
    g_autoptr(GtkListItemFactory) factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(item_bind), GINT_TO_POINTER(TRUE));
    gtk_drop_down_set_factory(dropdown, factory);
    g_autoptr(GtkListItemFactory) list_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(list_factory, "setup", G_CALLBACK(item_setup), NULL);
    g_signal_connect(list_factory, "bind", G_CALLBACK(item_bind), NULL);
    gtk_drop_down_set_list_factory(dropdown, list_factory);
    gtk_accessible_update_property(GTK_ACCESSIBLE(dropdown), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    return dropdown;
}

static void socket_selected(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object; (void)property;
    SocketPicker *state = user_data;
    guint selected = gtk_drop_down_get_selected(state->dropdown);
    if (!state->updating && state->selected && state->paths && selected < g_strv_length(state->paths))
        state->selected(state->paths[selected], state->user_data);
}

static void free_socket_picker(gpointer user_data)
{
    SocketPicker *state = user_data;
    g_strfreev(state->paths);
    g_free(state);
}

GtkWidget *socket_picker_new(SocketSelectedFunc selected, gpointer user_data)
{
    SocketPicker *state = g_new0(SocketPicker, 1);
    state->selected = selected;
    state->user_data = user_data;
    state->dropdown = dropdown_new("Instance to execute scripts in");
    GtkWidget *widget = GTK_WIDGET(state->dropdown);
    gtk_widget_add_css_class(widget, "socket-picker");
    g_object_set_data_full(G_OBJECT(widget), "socket-picker", state, free_socket_picker);
    g_signal_connect(widget, "notify::selected", G_CALLBACK(socket_selected), state);
    return widget;
}

void socket_picker_update(GtkWidget *picker, const char *const *paths, const char *selected)
{
    SocketPicker *state = g_object_get_data(G_OBJECT(picker), "socket-picker");
    g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; paths[i]; ++i) g_ptr_array_add(items, g_strdup(paths[i]));
    if (paths[0] && selected && !g_strv_contains(paths, selected)) g_ptr_array_add(items, g_strdup(selected));
    g_ptr_array_add(items, NULL);
    char **next = (char **)items->pdata;
    guint position = 0;
    for (guint i = 0; next[i]; ++i)
        if (g_strcmp0(next[i], selected) == 0) position = i;
    state->updating = TRUE;
    if (!state->paths || !g_strv_equal((const char *const *)state->paths, (const char *const *)next)) {
        g_autoptr(GtkStringList) model = gtk_string_list_new(NULL);
        if (!next[0]) gtk_string_list_append(model, "No Instances");
        for (guint i = 0; next[i]; ++i) {
            g_autofree char *profile = profile_socket_name(next[i]);
            gtk_string_list_append(model, profile);
        }
        g_strfreev(state->paths);
        state->paths = g_strdupv(next);
        gtk_drop_down_set_model(state->dropdown, G_LIST_MODEL(model));
    }
    gtk_drop_down_set_selected(state->dropdown, position);
    gtk_widget_set_sensitive(picker, next[0] != NULL);
    if (next[0]) gtk_widget_remove_css_class(picker, "empty");
    else gtk_widget_add_css_class(picker, "empty");
    gtk_widget_set_tooltip_text(picker, next[0]
        ? "Choose which running instance receives scripts."
        : "No running instances. Launch a Profile to get started.");
    state->updating = FALSE;
}

static void update_profile_list(ProfilePicker *state);

static gboolean row_running(ProfileRow *row)
{
    return row->name && row->picker->running &&
        row->picker->running(row->name, row->picker->user_data);
}

static void show_error(ProfilePicker *state, const char *message)
{
    gtk_label_set_text(state->error, message ? message : "");
    gtk_widget_set_visible(GTK_WIDGET(state->error), message != NULL);
}

static void refresh_rows(ProfilePicker *state)
{
    g_auto(GStrv) names = profiles_list();
    g_strfreev(state->names);
    state->names = g_steal_pointer(&names);
    update_profile_list(state);
}

static void update_row(ProfileRow *row)
{
    gboolean running = row_running(row);
    gboolean editing = row->picker->editing == row;
    gtk_widget_set_sensitive(row->play, !running && !row->picker->editing);
    gtk_widget_set_sensitive(row->rename, !running && !row->picker->editing);
    gtk_widget_set_sensitive(row->save, !running);
    gtk_widget_set_visible(row->play, !editing);
    gtk_widget_set_visible(row->rename, !editing);
    gtk_widget_set_visible(row->save, editing);
    gtk_widget_set_visible(row->cancel, editing);
    gtk_widget_set_tooltip_text(row->play, running ? "Profile is running" : "Launch Profile");
    gtk_widget_set_tooltip_text(row->rename, running ? "Close this Profile to rename it" : "Rename Profile");
}

static void update_rows(ProfilePicker *state)
{
    for (GtkWidget *widget = gtk_widget_get_first_child(GTK_WIDGET(state->list)); widget;
         widget = gtk_widget_get_next_sibling(widget)) {
        ProfileRow *row = g_object_get_data(G_OBJECT(widget), "profile-row");
        if (row) update_row(row);
    }
    gtk_widget_set_sensitive(state->add, state->editing == NULL);
}

static void cancel_edit(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    ProfileRow *row = user_data;
    ProfilePicker *state = row->picker;
    state->editing = NULL;
    show_error(state, NULL);
    refresh_rows(state);
}

static void save_name(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    ProfileRow *row = user_data;
    ProfilePicker *state = row->picker;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(row->entry));
    if (row_running(row)) {
        show_error(state, "Close this Profile before renaming it.");
        return;
    }
    g_autoptr(GError) error = NULL;
    gboolean saved = row->name ? profile_rename(row->name, name, &error) : profile_create(name, &error);
    if (!saved) {
        show_error(state, error->message);
        return;
    }
    state->editing = NULL;
    show_error(state, NULL);
    refresh_rows(state);
}

static gboolean edit_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                         GdkModifierType modifiers, gpointer user_data)
{
    (void)controller; (void)keycode; (void)modifiers;
    if (keyval != GDK_KEY_Escape) return FALSE;
    cancel_edit(NULL, user_data);
    return TRUE;
}

static void begin_edit(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    ProfileRow *row = user_data;
    ProfilePicker *state = row->picker;
    if (state->editing || row_running(row)) return;
    state->editing = row;
    gtk_widget_add_css_class(row->widget, "editing-profile");
    show_error(state, NULL);
    gtk_editable_set_text(GTK_EDITABLE(row->entry), row->name ? row->name : "");
    gtk_stack_set_visible_child(row->name_stack, GTK_WIDGET(row->entry));
    update_rows(state);
    gtk_widget_grab_focus(GTK_WIDGET(row->entry));
    gtk_editable_select_region(GTK_EDITABLE(row->entry), 0, -1);
}

static void launch_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProfileRow *row = user_data;
    ProfilePicker *state = row->picker;
    if (!row->name || !state->launch || row_running(row) || state->editing) return;
    g_autofree char *name = g_strdup(row->name);
    gtk_widget_set_visible(GTK_WIDGET(state->dialog), FALSE);
    state->launch(name, state->user_data);
}

static void free_row(gpointer user_data)
{
    ProfileRow *row = user_data;
    g_free(row->name);
    g_free(row);
}

static GtkWidget *row_button(const char *label, const char *icon, const char *style)
{
    GtkWidget *button = gtk_button_new();
    if (icon) {
        GtkWidget *image = gtk_image_new_from_resource(icon);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 14);
        gtk_button_set_child(GTK_BUTTON(button), image);
    } else {
        gtk_button_set_label(GTK_BUTTON(button), label);
    }
    gtk_widget_add_css_class(button, "profile-action");
    gtk_widget_add_css_class(button, style);
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    return button;
}

static ProfileRow *row_new(ProfilePicker *state, const char *name)
{
    ProfileRow *row = g_new0(ProfileRow, 1);
    row->picker = state;
    row->name = g_strdup(name);
    row->widget = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row->widget), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row->widget), FALSE);
    g_object_set_data_full(G_OBJECT(row->widget), "profile-row", row, free_row);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    row->name_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_hhomogeneous(row->name_stack, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(row->name_stack), TRUE);
    row->label = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(row->label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(row->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(row->label), 28);
    row->entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(row->entry, 64);
    gtk_entry_set_placeholder_text(row->entry, "Profile name…");
    gtk_editable_set_width_chars(GTK_EDITABLE(row->entry), 12);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Profile name", -1);
    gtk_stack_add_child(row->name_stack, row->label);
    gtk_stack_add_child(row->name_stack, GTK_WIDGET(row->entry));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(row->name_stack));
    row->play = row_button("Launch Profile", "/org/relapse/RelapseGtk/icons/play.svg", "launch-profile");
    row->rename = row_button("Rename Profile", "/org/relapse/RelapseGtk/icons/rename.svg", "rename-profile");
    row->save = row_button("✓", NULL, "save-profile");
    row->cancel = row_button("×", NULL, "cancel-profile");
    gtk_widget_set_tooltip_text(row->save, "Save Profile name (Enter)");
    gtk_widget_set_tooltip_text(row->cancel, "Cancel (Escape)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->save), GTK_ACCESSIBLE_PROPERTY_LABEL, "Save Profile name", -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->cancel), GTK_ACCESSIBLE_PROPERTY_LABEL, "Cancel name edit", -1);
    GtkWidget *buttons[] = {row->play, row->rename, row->save, row->cancel};
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) gtk_box_append(GTK_BOX(box), buttons[i]);
    g_signal_connect(row->play, "clicked", G_CALLBACK(launch_clicked), row);
    g_signal_connect(row->rename, "clicked", G_CALLBACK(begin_edit), row);
    g_signal_connect(row->save, "clicked", G_CALLBACK(save_name), row);
    g_signal_connect(row->entry, "activate", G_CALLBACK(save_name), row);
    g_signal_connect(row->cancel, "clicked", G_CALLBACK(cancel_edit), row);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(edit_key), row);
    gtk_widget_add_controller(GTK_WIDGET(row->entry), keys);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row->widget), box);
    update_row(row);
    return row;
}

static void add_profile(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProfilePicker *state = user_data;
    if (state->editing) return;
    ProfileRow *row = row_new(state, NULL);
    gtk_list_box_insert(state->list, row->widget, g_strv_length(state->names));
    begin_edit(NULL, row);
}

static void update_profile_list(ProfilePicker *state)
{
    if (!state->dialog || state->editing) return;
    gtk_list_box_remove_all(state->list);
    for (guint i = 0; state->names && state->names[i]; ++i) {
        ProfileRow *row = row_new(state, state->names[i]);
        gtk_list_box_append(state->list, row->widget);
    }
    state->add = gtk_button_new_with_label("+  Add Profile");
    gtk_widget_add_css_class(state->add, "add-profile");
    g_signal_connect(state->add, "clicked", G_CALLBACK(add_profile), state);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), state->add);
    gtk_list_box_append(state->list, row);
}

static gboolean dialog_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                           GdkModifierType modifiers, gpointer user_data)
{
    (void)controller; (void)keycode; (void)modifiers;
    ProfilePicker *state = user_data;
    if (keyval != GDK_KEY_Escape) return FALSE;
    if (state->editing) cancel_edit(NULL, state->editing);
    else gtk_window_close(state->dialog);
    return TRUE;
}

static void open_profiles(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProfilePicker *state = user_data;
    if (state->dialog) {
        gtk_window_present(state->dialog);
        return;
    }
    state->dialog = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(state->dialog, "Profiles");
    gtk_window_set_transient_for(state->dialog, GTK_WINDOW(gtk_widget_get_root(state->widget)));
    gtk_window_set_modal(state->dialog, TRUE);
    gtk_window_set_hide_on_close(state->dialog, TRUE);
    gtk_window_set_default_size(state->dialog, 360, 320);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(dialog_key), state);
    gtk_widget_add_controller(GTK_WIDGET(state->dialog), keys);
    gtk_widget_add_css_class(GTK_WIDGET(state->dialog), "relapse-ui");
    gtk_widget_add_css_class(GTK_WIDGET(state->dialog), "profiles-dialog");
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    GtkWidget *title = gtk_label_new("Profiles");
    gtk_widget_add_css_class(title, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    GtkWidget *close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close, "window-control");
    gtk_widget_set_tooltip_text(close, "Close");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), state->dialog);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), close);
    gtk_window_set_titlebar(state->dialog, header);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(content, "profiles-content");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);
    state->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->list), "profiles-list");
    gtk_list_box_set_selection_mode(state->list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->list));
    gtk_box_append(GTK_BOX(content), scroll);
    state->error = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_wrap(state->error, TRUE);
    gtk_label_set_max_width_chars(state->error, 38);
    gtk_label_set_xalign(state->error, 0);
    gtk_widget_add_css_class(GTK_WIDGET(state->error), "script-rename-error");
    gtk_widget_set_visible(GTK_WIDGET(state->error), FALSE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(state->error));
    gtk_window_set_child(state->dialog, content);
    update_profile_list(state);
    gtk_window_present(state->dialog);
}

void profile_picker_update(GtkWidget *picker, const char *const *names)
{
    ProfilePicker *state = g_object_get_data(G_OBJECT(picker), "profile-picker");
    if (!state->editing && (!state->names || !g_strv_equal((const char *const *)state->names, names))) {
        g_strfreev(state->names);
        state->names = g_strdupv((char **)names);
        update_profile_list(state);
    }
    if (state->dialog) update_rows(state);
}

void profile_picker_shutdown(GtkWidget *picker)
{
    ProfilePicker *state = g_object_get_data(G_OBJECT(picker), "profile-picker");
    if (state->dialog) {
        gtk_window_destroy(state->dialog);
        state->dialog = NULL;
        state->editing = NULL;
    }
}

static void free_profile_picker(gpointer user_data)
{
    ProfilePicker *state = user_data;
    if (state->dialog) gtk_window_destroy(state->dialog);
    g_strfreev(state->names);
    g_free(state);
}

GtkWidget *profile_picker_new(ProfileLaunchFunc launch, ProfileRunningFunc running, gpointer user_data)
{
    ProfilePicker *state = g_new0(ProfilePicker, 1);
    state->launch = launch;
    state->running = running;
    state->user_data = user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    state->widget = box;
    gtk_widget_add_css_class(box, "profile-picker");
    g_object_set_data_full(G_OBJECT(box), "profile-picker", state, free_profile_picker);
    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "profiles-button");
    gtk_widget_set_hexpand(button, TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Profiles", -1);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Profiles");
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(content), label);
    GtkWidget *icon = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/profiles.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_box_append(GTK_BOX(content), icon);
    gtk_button_set_child(GTK_BUTTON(button), content);
    gtk_box_append(GTK_BOX(box), button);
    g_signal_connect(button, "clicked", G_CALLBACK(open_profiles), state);
    return box;
}
