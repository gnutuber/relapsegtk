#include "cache.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CACHE_LIMIT = 1024 * 1024, AVATAR_LIMIT = 512 * 1024 };

typedef struct {
    gboolean valid;
    gint64 checked_at;
    gint64 user_id;
    char *username;
    char *display_name;
} CacheEntry;

static void entry_free(gpointer data)
{
    CacheEntry *entry = data;
    g_free(entry->username);
    g_free(entry->display_name);
    g_free(entry);
}

char *profile_cache_directory(void)
{
    return g_build_filename(g_get_home_dir(), ".local", "share", "relapse", "relapsegtk", "cache", NULL);
}

static GBytes *read_bytes(const char *path, gsize limit)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        info.st_size <= 0 || (gsize)info.st_size > limit) {
        close(fd);
        return NULL;
    }
    g_autoptr(GByteArray) bytes = g_byte_array_sized_new(info.st_size);
    char buffer[4096];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || bytes->len + (gsize)MAX(count, 0) > limit) {
            close(fd);
            return NULL;
        }
        if (!count) break;
        g_byte_array_append(bytes, (const guint8 *)buffer, count);
    }
    close(fd);
    return g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
}

static char *read_text(const char *path, gsize limit)
{
    g_autoptr(GBytes) bytes = read_bytes(path, limit);
    gsize size;
    const char *data = bytes ? g_bytes_get_data(bytes, &size) : NULL;
    if (!data || memchr(data, 0, size)) return NULL;
    return g_strndup(data, size);
}

static gboolean write_bytes(const char *path, const void *data, gsize size)
{
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *temporary = g_strdup_printf("%s.%s", path, uuid);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return FALSE;
    gsize written = 0;
    while (written < size) {
        ssize_t count = write(fd, (const char *)data + written, size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += count;
    }
    gboolean ok = written == size && fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) unlink(temporary);
    return ok;
}

static char *avatar_path(const char *directory, gint64 user_id)
{
    g_autofree char *avatars = g_build_filename(directory, "avatars", NULL);
    g_autofree char *name = g_strdup_printf("%" G_GINT64_FORMAT ".png", user_id);
    return g_build_filename(avatars, name, NULL);
}

static gboolean cache_label(const char *text)
{
    return text && *text && strlen(text) <= 256 && g_utf8_validate(text, -1, NULL);
}

static GHashTable *cache_load(const char *directory)
{
    GHashTable *entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
    g_autofree char *path = g_build_filename(directory, "accounts.json", NULL);
    g_autofree char *data = read_text(path, CACHE_LIMIT);
    if (!data) return entries;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, -1, NULL)) return entries;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return entries;
    JsonObject *object = json_node_get_object(root);
    if (json_object_get_int_member(object, "version") != 1) return entries;
    JsonNode *node = json_object_get_member(object, "accounts");
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return entries;
    JsonObject *accounts = json_node_get_object(node);
    g_autoptr(GList) members = json_object_get_members(accounts);
    for (GList *item = members; item; item = item->next) {
        const char *hash = item->data;
        JsonNode *entry_node = json_object_get_member(accounts, hash);
        if (!hash || !*hash || !entry_node || !JSON_NODE_HOLDS_OBJECT(entry_node)) continue;
        JsonObject *entry_object = json_node_get_object(entry_node);
        const char *username = json_object_get_string_member_with_default(entry_object, "username", NULL);
        const char *display = json_object_get_string_member_with_default(entry_object, "display_name", NULL);
        CacheEntry *entry = g_new0(CacheEntry, 1);
        entry->valid = json_object_get_boolean_member_with_default(entry_object, "valid", FALSE);
        entry->checked_at = json_object_get_int_member_with_default(entry_object, "checked_at", 0);
        entry->user_id = json_object_get_int_member_with_default(entry_object, "user_id", 0);
        if (entry->valid && (entry->user_id <= 0 || !cache_label(username) || !cache_label(display))) {
            entry_free(entry);
            continue;
        }
        entry->username = g_strdup(username);
        entry->display_name = g_strdup(display);
        g_hash_table_replace(entries, g_strdup(hash), entry);
    }
    return entries;
}

static gboolean cache_save(const char *directory, GHashTable *entries)
{
    g_autoptr(JsonObject) accounts = json_object_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, entries);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        CacheEntry *entry = value;
        g_autoptr(JsonObject) object = json_object_new();
        json_object_set_boolean_member(object, "valid", entry->valid);
        json_object_set_int_member(object, "checked_at", entry->checked_at);
        if (entry->valid) {
            json_object_set_int_member(object, "user_id", entry->user_id);
            json_object_set_string_member(object, "username", entry->username);
            json_object_set_string_member(object, "display_name", entry->display_name);
        }
        json_object_set_object_member(accounts, key, g_steal_pointer(&object));
    }
    g_autoptr(JsonObject) root = json_object_new();
    json_object_set_int_member(root, "version", 1);
    json_object_set_object_member(root, "accounts", g_steal_pointer(&accounts));
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    g_autofree char *data = json_to_string(node, TRUE);
    g_autofree char *path = g_build_filename(directory, "accounts.json", NULL);
    return write_bytes(path, data, strlen(data));
}

static gboolean ensure_directory(const char *directory)
{
    return g_mkdir_with_parents(directory, 0700) == 0;
}

gboolean profile_cache_lookup(const char *cookie, gboolean *valid, ProfileAccount **account)
{
    if (valid) *valid = FALSE;
    if (account) *account = NULL;
    if (!cookie || !*cookie) return FALSE;
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    g_autofree char *directory = profile_cache_directory();
    g_autoptr(GHashTable) entries = cache_load(directory);
    CacheEntry *entry = g_hash_table_lookup(entries, hash);
    if (!entry) return FALSE;
    if (valid) *valid = entry->valid;
    if (!entry->valid || !account) return TRUE;
    ProfileAccount *loaded = g_new0(ProfileAccount, 1);
    loaded->user_id = entry->user_id;
    loaded->username = g_strdup(entry->username);
    loaded->display_name = g_strdup(entry->display_name);
    loaded->cookie_hash = g_strdup(hash);
    loaded->checked_at = entry->checked_at;
    g_autofree char *path = avatar_path(directory, entry->user_id);
    loaded->avatar = read_bytes(path, AVATAR_LIMIT);
    *account = loaded;
    return TRUE;
}

gboolean profile_cache_store(const ProfileAccount *account)
{
    if (!account || account->user_id <= 0 || !cache_label(account->username) ||
        !cache_label(account->display_name) || !account->cookie_hash) return FALSE;
    g_autofree char *directory = profile_cache_directory();
    if (!ensure_directory(directory)) return FALSE;
    g_autoptr(GHashTable) entries = cache_load(directory);
    CacheEntry *entry = g_new0(CacheEntry, 1);
    entry->valid = TRUE;
    entry->checked_at = account->checked_at > 0 ? account->checked_at : g_get_real_time() / G_USEC_PER_SEC;
    entry->user_id = account->user_id;
    entry->username = g_strdup(account->username);
    entry->display_name = g_strdup(account->display_name);
    g_hash_table_replace(entries, g_strdup(account->cookie_hash), entry);
    if (account->avatar) {
        gsize size;
        const guchar *data = g_bytes_get_data(account->avatar, &size);
        if (data && size && size <= AVATAR_LIMIT) {
            g_autofree char *avatars = g_build_filename(directory, "avatars", NULL);
            if (ensure_directory(avatars)) {
                g_autofree char *path = avatar_path(directory, account->user_id);
                write_bytes(path, data, size);
            }
        }
    }
    return cache_save(directory, entries);
}

gboolean profile_cache_store_invalid(const char *cookie)
{
    if (!cookie || !*cookie) return FALSE;
    g_autofree char *directory = profile_cache_directory();
    if (!ensure_directory(directory)) return FALSE;
    g_autoptr(GHashTable) entries = cache_load(directory);
    CacheEntry *entry = g_new0(CacheEntry, 1);
    entry->checked_at = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    g_hash_table_replace(entries, g_strdup(hash), entry);
    return cache_save(directory, entries);
}

/* Only one lookup per cookie may run at a time; the first caller owns it and
 * callers that lose the claim wait for the cache to be filled instead. */
static GHashTable *inflight;

gboolean profile_cache_claim(const char *cookie)
{
    if (!cookie || !*cookie) return FALSE;
    if (!inflight) inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    if (g_hash_table_contains(inflight, hash)) return FALSE;
    g_hash_table_add(inflight, g_strdup(hash));
    return TRUE;
}

void profile_cache_release(const char *cookie)
{
    if (!inflight || !cookie || !*cookie) return;
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    g_hash_table_remove(inflight, hash);
}
