#pragma once

#include <gtk/gtk.h>

GtkWidget *autoexec_new(const char *scripts_path);
char *autoexec_directory_path(void);
