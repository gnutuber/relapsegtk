#include "launcher.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "store.h"

gboolean launcher_settings_load(char **path, GError **error)
{
    *path = NULL;
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file) return FALSE;
    *path = g_key_file_get_string(file, "Launcher", "path", NULL);
    return TRUE;
}

gboolean launcher_settings_save(const char *path, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file) return FALSE;
    g_key_file_set_string(file, "Launcher", "path", path ? path : "");
    return store_save(file, error);
}

typedef struct {
    char *path;
    gint64 mtime;
    gboolean executable;
} FoundLauncher;

static gboolean launcher_name_matches(const char *name)
{
    if (!g_str_has_prefix(name, "relapse-launcher"))
        return FALSE;
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name)
        return TRUE;
    return !(g_ascii_strcasecmp(ext, ".zip") == 0 || g_ascii_strcasecmp(ext, ".7z") == 0 ||
             g_ascii_strcasecmp(ext, ".gz") == 0 || g_ascii_strcasecmp(ext, ".tar") == 0 ||
             g_ascii_strcasecmp(ext, ".xz") == 0);
}

static gboolean is_x86_64_elf(const char *path)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, NULL);
    if (!stream)
        return FALSE;
    unsigned char header[20];
    gsize read = 0;
    if (!g_input_stream_read_all(G_INPUT_STREAM(stream), header, sizeof header, &read, NULL, NULL) ||
        read < sizeof header)
        return FALSE;
    return header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' &&
           header[4] == 2 && header[5] == 1 && header[18] == 0x3e && header[19] == 0;
}

static void scan_for_launcher(const char *dir_path, int depth, FoundLauncher *found)
{
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return;
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.')
            continue;
        g_autofree char *path = g_build_filename(dir_path, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            if (depth < 2)
                scan_for_launcher(path, depth + 1, found);
            continue;
        }
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) || !launcher_name_matches(name))
            continue;
        if (!is_x86_64_elf(path))
            continue;
        GStatBuf info;
        if (g_stat(path, &info) != 0)
            continue;
        gboolean executable = g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
        if (!found->path || info.st_mtime > found->mtime ||
            (info.st_mtime == found->mtime && executable && !found->executable)) {
            g_free(found->path);
            found->path = g_strdup(path);
            found->mtime = info.st_mtime;
            found->executable = executable;
        }
    }
    g_dir_close(dir);
}

char *launcher_find(void)
{
    FoundLauncher found = {0};
    g_autofree char *downloads = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    scan_for_launcher(downloads, 0, &found);
    if (!found.path)
        scan_for_launcher(g_get_home_dir(), 0, &found);
    if (found.path && !found.executable) {
        GStatBuf info;
        if (g_stat(found.path, &info) == 0)
            g_chmod(found.path, info.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
    }
    return found.path;
}
