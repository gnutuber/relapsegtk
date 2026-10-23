#pragma once
#include <gtk/gtk.h>

GtkWidget *output_new(void);
void output_set_status(GtkWidget *pane, const char *path, const char *text, gboolean active);
void output_append(GtkWidget *pane, const char *path, const char *level, const char *text);
void output_set_target(GtkWidget *pane, const char *path);
