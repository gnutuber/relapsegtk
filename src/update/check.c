#include "check.h"
#include "version.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define RELEASES_URL "https://api.github.com/repos/vpu00000/relapsegtk/releases/latest"

const char *update_current_version(void)
{
    const char *override = g_getenv("RELAPSEGTK_VERSION");
    return override && *override ? override : RELAPSEGTK_VERSION;
}

static const char *tag_digits(const char *version)
{
    while (*version && !g_ascii_isdigit(*version))
        ++version;
    return version;
}

gboolean update_version_newer(const char *candidate, const char *current)
{
    if (!candidate || !current)
        return FALSE;
    const char *left = tag_digits(candidate);
    const char *right = tag_digits(current);
    for (int part = 0; part < 4; ++part) {
        char *left_end = NULL;
        char *right_end = NULL;
        guint64 a = g_ascii_strtoull(left, &left_end, 10);
        guint64 b = g_ascii_strtoull(right, &right_end, 10);
        if (a != b)
            return a > b;
        left = left_end;
        right = right_end;
        if (*left == '.') ++left;
        if (*right == '.') ++right;
    }
    return FALSE;
}

void update_info_free(UpdateInfo *info)
{
    if (!info)
        return;
    g_free(info->version);
    g_free(info->download_url);
    g_free(info->asset_name);
    g_free(info->release_url);
    g_free(info);
}

static char *curl_path(void)
{
    const char *curl = g_find_program_in_path("curl");
    return g_strdup(curl);
}

static UpdateInfo *parse_release(const char *payload, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, -1, error))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "GitHub returned an unexpected release description");
        return NULL;
    }
    JsonObject *release = json_node_get_object(root);
    if (!json_object_has_member(release, "tag_name")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "The latest GitHub release has no version tag");
        return NULL;
    }
    UpdateInfo *info = g_new0(UpdateInfo, 1);
    info->version = g_strdup(json_object_get_string_member(release, "tag_name"));
    if (json_object_has_member(release, "html_url"))
        info->release_url = g_strdup(json_object_get_string_member(release, "html_url"));
    if (json_object_has_member(release, "assets")) {
        JsonArray *assets = json_object_get_array_member(release, "assets");
        guint count = assets ? json_array_get_length(assets) : 0;
        for (guint i = 0; i < count; ++i) {
            JsonObject *asset = json_array_get_object_element(assets, i);
            const char *name = asset && json_object_has_member(asset, "name")
                ? json_object_get_string_member(asset, "name") : NULL;
            if (!name || !g_str_has_suffix(name, ".AppImage"))
                continue;
            info->asset_name = g_strdup(name);
            if (json_object_has_member(asset, "browser_download_url"))
                info->download_url = g_strdup(
                    json_object_get_string_member(asset, "browser_download_url"));
            break;
        }
    }
    if (!info->version || !*info->version) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "The latest GitHub release has no version tag");
        update_info_free(info);
        return NULL;
    }
    return info;
}

typedef struct {
    UpdateCheckFunc callback;
    gpointer user_data;
} CheckRequest;

static void check_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    CheckRequest *request = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree char *payload = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &payload, NULL, &error)) {
        request->callback(NULL, error->message, request->user_data);
        g_free(request);
        return;
    }
    if (!g_subprocess_get_successful(G_SUBPROCESS(source))) {
        request->callback(NULL, "GitHub did not return the latest release", request->user_data);
        g_free(request);
        return;
    }
    g_autoptr(GError) parse_error = NULL;
    UpdateInfo *info = parse_release(payload, &parse_error);
    if (!info) {
        request->callback(NULL, parse_error->message, request->user_data);
        g_free(request);
        return;
    }
    request->callback(info, NULL, request->user_data);
    update_info_free(info);
    g_free(request);
}

void update_check_async(UpdateCheckFunc callback, gpointer user_data)
{
    g_autofree char *curl = curl_path();
    if (!curl) {
        callback(NULL, "curl is required to check for updates", user_data);
        return;
    }
    const char *argv[] = {
        curl, "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json",
        RELEASES_URL, NULL
    };
    g_autoptr(GError) error = NULL;
    GSubprocess *process = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (!process) {
        callback(NULL, error->message, user_data);
        return;
    }
    CheckRequest *request = g_new0(CheckRequest, 1);
    request->callback = callback;
    request->user_data = user_data;
    g_subprocess_communicate_utf8_async(process, NULL, NULL, check_ready, request);
    g_object_unref(process);
}

char *update_download_directory(void)
{
    return g_build_filename(g_get_user_data_dir(), "relapsegtk", "updates", NULL);
}

typedef struct {
    UpdateDownloadFunc callback;
    gpointer user_data;
    char *path;
} DownloadRequest;

static void download_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DownloadRequest *request = user_data;
    g_autoptr(GError) error = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        request->callback(NULL, error->message, request->user_data);
    } else if (g_chmod(request->path, 0755) != 0) {
        request->callback(NULL, "Downloaded, but could not make the AppImage executable", request->user_data);
    } else {
        request->callback(request->path, NULL, request->user_data);
    }
    g_free(request->path);
    g_free(request);
}

void update_download_async(const char *url, const char *asset_name, const char *directory,
                           UpdateDownloadFunc callback, gpointer user_data)
{
    if (!url || !*url) {
        callback(NULL, "This release has no AppImage asset", user_data);
        return;
    }
    g_autofree char *curl = curl_path();
    if (!curl) {
        callback(NULL, "curl is required to download updates", user_data);
        return;
    }
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        callback(NULL, "Could not create the update directory", user_data);
        return;
    }
    g_autofree char *name = asset_name && *asset_name
        ? g_strdup(asset_name) : g_strdup("RelapseGtk-x86_64.AppImage");
    g_autofree char *path = g_build_filename(directory, name, NULL);
    const char *argv[] = {curl, "-fsSL", "--max-time", "600", "-o", path, url, NULL};
    g_autoptr(GError) spawn_error = NULL;
    GSubprocess *process = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &spawn_error);
    if (!process) {
        callback(NULL, spawn_error->message, user_data);
        return;
    }
    DownloadRequest *request = g_new0(DownloadRequest, 1);
    request->callback = callback;
    request->user_data = user_data;
    request->path = g_steal_pointer(&path);
    g_subprocess_wait_check_async(process, NULL, download_ready, request);
    g_object_unref(process);
}
