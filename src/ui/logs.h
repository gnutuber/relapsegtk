#pragma once

#include <gtk/gtk.h>

GtkWidget *logs_new(void);
void logs_set_single_file(GtkWidget *logs, gboolean single);
void logs_refresh(GtkWidget *logs);
