#pragma once
#include <glib.h>

typedef struct McpServer McpServer;

typedef void (*McpExecuteFunc)(const char *name, const char *source, gpointer user_data);

typedef struct {
    int port;
    gboolean expose_execute;
    gboolean expose_logs;
    const char *token;
} McpServerConfig;

McpServer *mcp_server_new(McpExecuteFunc execute, gpointer user_data);
gboolean   mcp_server_apply(McpServer *server, const McpServerConfig *config, GError **error);
void       mcp_server_record_log(McpServer *server, const char *level, const char *text);
void       mcp_server_free(McpServer *server);
