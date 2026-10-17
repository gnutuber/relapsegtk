#pragma once
#include <gtk/gtk.h>

GtkWidget *output_new(void);
void output_set_status(GtkWidget *pane, const char *text);
void output_append(GtkWidget *pane, const char *level, const char *text);
