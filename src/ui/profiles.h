#pragma once

#include <gtk/gtk.h>

typedef void (*SocketSelectedFunc)(const char *path, gpointer user_data);
typedef void (*ProfileLaunchFunc)(const char *name, gpointer user_data);
typedef gboolean (*ProfileRunningFunc)(const char *name, gpointer user_data);

GtkWidget *socket_picker_new(SocketSelectedFunc selected, gpointer user_data);
void socket_picker_update(GtkWidget *picker, const char *const *paths, const char *selected);
GtkWidget *profile_picker_new(ProfileLaunchFunc launch, ProfileRunningFunc running, gpointer user_data);
void profile_picker_update(GtkWidget *picker, const char *const *names);
void profile_picker_shutdown(GtkWidget *picker);
