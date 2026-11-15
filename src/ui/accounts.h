#pragma once

#include "profiles.h"

GtkWidget *accounts_new(ProfileLaunchFunc launch, ProfileLaunchAllFunc launch_all,
                        ProfileRunningFunc running, ProfileKillFunc kill,
                        ProfileKillAllFunc kill_all, gpointer user_data);
void accounts_update(GtkWidget *accounts, const char *const *names);
void accounts_set_launch_error(GtkWidget *accounts, const char *name, const char *message);
void accounts_shutdown(GtkWidget *accounts);
