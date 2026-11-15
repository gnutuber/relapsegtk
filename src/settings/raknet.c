#include "raknet.h"

#include <errno.h>
#include <json-glib/json-glib.h>
#include <string.h>

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "relapse", "config.json", NULL);
}

static JsonObject *config_load(void)
{
    g_autofree char *path = config_path();
    g_autofree char *data = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &data, &length, NULL) || length > 1024 * 1024) return NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, length, NULL)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
    JsonObject *object = json_node_get_object(root);
    return json_object_ref(object);
}

gboolean raknet_settings_load(gboolean *enabled)
{
    if (enabled) *enabled = TRUE;
    g_autoptr(JsonObject) config = config_load();
    if (!config) return FALSE;
    JsonNode *node = json_object_get_member(config, "raknet");
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
    JsonObject *raknet = json_node_get_object(node);
    if (enabled)
        *enabled = json_object_get_boolean_member_with_default(raknet, "enabled", TRUE);
    return TRUE;
}

gboolean raknet_settings_save(gboolean enabled, GError **error)
{
    g_autoptr(JsonObject) config = config_load();
    if (!config) config = json_object_new();
    g_autoptr(JsonObject) raknet = json_object_new();
    JsonNode *existing = json_object_get_member(config, "raknet");
    if (existing && JSON_NODE_HOLDS_OBJECT(existing)) {
        JsonObject *current = json_node_get_object(existing);
        g_autoptr(GList) members = json_object_get_members(current);
        for (GList *item = members; item; item = item->next) {
            const char *name = item->data;
            if (!name || g_str_equal(name, "enabled")) continue;
            JsonNode *value = json_object_get_member(current, name);
            if (value) json_object_set_member(raknet, name, json_node_copy(value));
        }
    }
    json_object_set_boolean_member(raknet, "enabled", enabled);
    json_object_set_object_member(config, "raknet", g_steal_pointer(&raknet));
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, config);
    g_autofree char *data = json_to_string(root, TRUE);
    g_autofree char *path = config_path();
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0 && errno != EEXIST) {
        int saved = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
            "Could not create the Relapse config folder: %s", g_strerror(saved));
        return FALSE;
    }
    if (!g_file_set_contents(path, data, -1, error)) return FALSE;
    return TRUE;
}
