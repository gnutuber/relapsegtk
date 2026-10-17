#pragma once
#include <glib.h>

typedef struct {
    int port;
    gboolean expose_execute;
    gboolean expose_logs;
    char *token;
} McpSettings;

void     mcp_settings_defaults(McpSettings *out);
gboolean mcp_settings_load(McpSettings *out, GError **error);
gboolean mcp_settings_save(const McpSettings *in, GError **error);
void     mcp_settings_clear(McpSettings *in);
