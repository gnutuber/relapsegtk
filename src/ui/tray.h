#pragma once

#include <gtk/gtk.h>

typedef struct Tray Tray;

typedef void (*TrayActivateFunc)(gpointer user_data);

Tray *tray_new(GtkWindow *window, TrayActivateFunc activate, TrayActivateFunc quit, gpointer user_data);
void tray_free(Tray *tray);
