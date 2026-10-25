#pragma once

#include <gio/gio.h>

gboolean profile_name_valid(const char *name);
gboolean profile_create(const char *name, GError **error);
gboolean profile_rename(const char *name, const char *replacement, GError **error);
gboolean profile_delete(const char *name, GError **error);
char **profiles_list(void);
char **profile_sockets_list(const char *directory);
char *profile_socket_name(const char *path);
gboolean profile_is_running(const char *name);
GSubprocess *profile_launch(const char *launcher, const char *name, GError **error);
