#pragma once

#include <glib.h>

char *log_directory(void);
char *log_path_for(const char *client, gboolean single);
char *log_client_name(const char *client);
gboolean log_append(const char *client, gboolean single, const char *level, const char *text,
                    GError **error);
