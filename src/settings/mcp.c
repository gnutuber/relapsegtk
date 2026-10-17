#include "mcp.h"
#include "store.h"

void mcp_settings_defaults(McpSettings *out)
{
    out->port = 7331;
    out->expose_execute = FALSE;
    out->expose_logs = FALSE;
    out->token = NULL;
}

void mcp_settings_clear(McpSettings *in)
{
    g_clear_pointer(&in->token, g_free);
}

gboolean mcp_settings_load(McpSettings *out, GError **error)
{
    mcp_settings_defaults(out);
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file) return FALSE;
    g_autoptr(GError) e = NULL;
    int port = g_key_file_get_integer(file, "Mcp", "port", &e);
    if (!e && port >= 1 && port <= 65535) out->port = port;
    g_clear_error(&e);
    gboolean b = g_key_file_get_boolean(file, "Mcp", "expose_execute", &e);
    if (!e) out->expose_execute = b;
    g_clear_error(&e);
    b = g_key_file_get_boolean(file, "Mcp", "expose_logs", &e);
    if (!e) out->expose_logs = b;
    g_clear_error(&e);
    char *tok = g_key_file_get_string(file, "Mcp", "token", &e);
    if (!e) out->token = tok;
    else g_free(tok);
    return TRUE;
}

gboolean mcp_settings_save(const McpSettings *in, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file) return FALSE;
    g_key_file_set_integer(file, "Mcp", "port", in->port);
    g_key_file_set_boolean(file, "Mcp", "expose_execute", in->expose_execute);
    g_key_file_set_boolean(file, "Mcp", "expose_logs", in->expose_logs);
    g_key_file_set_string(file, "Mcp", "token", in->token ? in->token : "");
    return store_save(file, error);
}
