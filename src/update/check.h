#pragma once

#include <gio/gio.h>

typedef struct {
    char *version;
    char *download_url;
    char *asset_name;
    char *release_url;
} UpdateInfo;

typedef void (*UpdateCheckFunc)(const UpdateInfo *info, const char *error, gpointer user_data);
typedef void (*UpdateDownloadFunc)(const char *path, const char *error, gpointer user_data);

const char *update_current_version(void);
gboolean update_version_newer(const char *candidate, const char *current);
void update_info_free(UpdateInfo *info);
void update_check_async(UpdateCheckFunc callback, gpointer user_data);
void update_download_async(const char *url, const char *asset_name, const char *directory,
                           UpdateDownloadFunc callback, gpointer user_data);
char *update_download_directory(void);
