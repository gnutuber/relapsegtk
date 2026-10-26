#pragma once

#include <gtk/gtk.h>

GtkWidget *shader_strip_new(void);
void shader_strip_set_light(GtkWidget *strip, double x);
