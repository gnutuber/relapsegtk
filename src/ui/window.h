#pragma once

#include <gtk/gtk.h>
#include "settings/editor.h"
#include "settings/mcp.h"

typedef struct {
    void (*execute)(gpointer user_data);
    void (*open_script)(GFile *file, const char *display_name, gpointer user_data);
    void (*active_script_changed)(gpointer user_data);
    gboolean (*editor_settings_changed)(const EditorSettings *settings, GError **error, gpointer user_data);
    gboolean (*restore_tabs_changed)(gboolean enabled, GError **error, gpointer user_data);
    gboolean (*output_changed)(gboolean enabled, GError **error, gpointer user_data);
    gboolean (*mcp_changed)(const McpSettings *settings, GError **error, gpointer user_data);
} WindowHandlers;

GtkWindow *window_new(GtkApplication *application, const char *scripts_path,
                      const WindowHandlers *handlers, gpointer user_data);
void window_set_execute_enabled(GtkWindow *window, gboolean enabled);
void window_set_status(GtkWindow *window, const char *label, const char *description, gboolean ready);
void window_open_script(GtkWindow *window, GFile *file, const char *display_name);
void window_set_editor_settings(GtkWindow *window, const EditorSettings *settings);
void window_set_mcp_settings(GtkWindow *window, const McpSettings *settings);
void window_set_output_enabled(GtkWindow *window, gboolean enabled);
void window_set_output_status(GtkWindow *window, const char *text);
void window_append_output(GtkWindow *window, const char *level, const char *text);
gboolean window_start_session(GtkWindow *window, gboolean enabled, GError **error);

GtkTextBuffer *window_get_buffer(GtkWindow *window);
const char *window_get_script_name(GtkWindow *window);
