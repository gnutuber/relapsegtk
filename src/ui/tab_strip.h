#pragma once
#include <gtk/gtk.h>

GtkWidget *tab_strip_new(void);
GtkBox *tab_strip_content(GtkWidget *strip);
void tab_strip_reveal(GtkWidget *strip, GtkWidget *tab);
