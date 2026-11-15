#define _GNU_SOURCE
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/fs.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

gboolean profile_name_valid(const char *name)
{
    if (!name || !*name || strlen(name) > 64 || g_str_equal(name, ".") || g_str_equal(name, ".."))
        return FALSE;
    for (const char *p = name; *p; ++p)
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    return TRUE;
}

static char *profiles_directory(void)
{
    return g_build_filename(g_get_home_dir(), ".local", "share", "relapse", "profiles", NULL);
}

gboolean profile_create(const char *name, GError **error)
{
    if (!profile_name_valid(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Use 1–64 letters, digits, dots, underscores or hyphens (excluding . and ..).");
        return FALSE;
    }
    g_autofree char *directory = profiles_directory();
    g_autofree char *path = g_build_filename(directory, name, NULL);
    if (g_mkdir_with_parents(directory, 0700) != 0 || g_mkdir(path, 0700) != 0) {
        int failure = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(failure),
            "Could not add Profile: %s", failure == EEXIST ? "this name already exists" : g_strerror(failure));
        return FALSE;
    }
    return TRUE;
}

gboolean profile_rename(const char *name, const char *replacement, GError **error)
{
    if (!profile_name_valid(name) || !profile_name_valid(replacement)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Use 1–64 letters, digits, dots, underscores or hyphens (excluding . and ..).");
        return FALSE;
    }
    g_autofree char *directory = profiles_directory();
    int parent = g_open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    int source = parent < 0 ? -1 : openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int lock = source < 0 ? -1 : openat(source, ".lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    int failure = errno;
    gboolean renamed = FALSE;
    struct stat info;
    if (lock >= 0) {
        if (fstat(lock, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid()) {
            failure = EACCES;
        } else if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
            failure = errno;
        } else if (fstat(source, &info) != 0) {
            failure = errno;
        } else {
            struct stat current;
            if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) != 0) {
                failure = errno;
            } else if (current.st_dev != info.st_dev || current.st_ino != info.st_ino) {
                failure = ESTALE;
            } else {
                /* The lock moves with its directory. Never replace a name created
                 * by another UI or launcher, even if it appears during this call. */
                renamed = g_str_equal(name, replacement) ||
                    syscall(SYS_renameat2, parent, name, parent, replacement, RENAME_NOREPLACE) == 0;
                failure = errno;
            }
        }
    }
    if (lock >= 0) close(lock);
    if (source >= 0) close(source);
    if (parent >= 0) close(parent);
    if (renamed) return TRUE;
    if (failure == EWOULDBLOCK || failure == EAGAIN)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Close this Profile before renaming it.");
    else
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(failure), "Could not rename Profile: %s",
            failure == EEXIST ? "this name already exists" : g_strerror(failure));
    return FALSE;
}

static int open_overlay_work(int profile, GError **error)
{
    int current = dup(profile);
    if (current < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not inspect profile: %s", g_strerror(errno));
        return -1;
    }
    const char *parts[] = {"state", "assets-work", "work"};
    for (guint i = 0; i < G_N_ELEMENTS(parts); ++i) {
        int next = openat(current, parts[i], O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int failure = errno;
        close(current);
        if (next < 0) {
            if (failure != ENOENT)
                g_set_error(error, G_IO_ERROR, g_io_error_from_errno(failure),
                    "Could not inspect overlay work directory: %s", g_strerror(failure));
            return -1;
        }
        current = next;
    }
    return current;
}

static int set_work_permissions(int fd, mode_t mode)
{
    /* O_PATH opens a mode-000 directory without needing read access. The proc
     * descriptor keeps chmod tied to that inode even after the move to Trash. */
    char path[64];
    g_snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    return chmod(path, mode);
}

static void delete_profile(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    (void)source_object;
    const char *name = task_data;
    if (!profile_name_valid(name)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid profile name.");
        return;
    }
    g_autofree char *directory = profiles_directory();
    int parent = g_open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    int source = parent < 0 ? -1 : openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int lock = source < 0 ? -1 : openat(source, ".lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    int failure = errno;
    gboolean ready = FALSE, deleted = FALSE;
    g_autoptr(GError) error = NULL;
    struct stat info, current;
    if (lock >= 0) {
        if (fstat(lock, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid()) {
            failure = EACCES;
        } else if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
            failure = errno;
        } else if (fstat(source, &info) != 0 || fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) != 0) {
            failure = errno;
        } else if (info.st_uid != getuid() || current.st_dev != info.st_dev || current.st_ino != info.st_ino) {
            failure = ESTALE;
        } else {
            ready = TRUE;
        }
    }
    int work = -1;
    mode_t work_mode = 0;
    gboolean work_changed = FALSE;
    if (ready) {
        work = open_overlay_work(source, &error);
        if (work >= 0) {
            if (fstat(work, &info) != 0) {
                g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Could not inspect overlay work permissions: %s", g_strerror(errno));
            } else if (info.st_uid != getuid()) {
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "The overlay work directory must belong to you.");
            } else {
                work_mode = info.st_mode & 07777;
                if ((work_mode & 0700) != 0700) {
                    if (set_work_permissions(work, work_mode | 0700) == 0) work_changed = TRUE;
                    else g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "Could not unlock overlay work directory: %s", g_strerror(errno));
                }
            }
        }
    }
    if (ready && !error) {
        g_autofree char *path = g_build_filename(directory, name, NULL);
        g_autoptr(GFile) file = g_file_new_for_path(path);
        deleted = g_file_trash(file, cancellable, &error);
    } else if (!ready && (failure == EWOULDBLOCK || failure == EAGAIN)) {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY, "Close this profile before deleting it.");
    } else if (!ready) {
        g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(failure),
            "Could not delete profile: %s", g_strerror(failure));
    }
    if (!deleted && work_changed && set_work_permissions(work, work_mode) != 0)
        g_prefix_error(&error, "Could not restore overlay work permissions (%s). ", g_strerror(errno));
    if (work >= 0) close(work);
    if (lock >= 0) close(lock);
    if (source >= 0) close(source);
    if (parent >= 0) close(parent);
    if (deleted) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, g_steal_pointer(&error));
}

void profile_delete_async(const char *name, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, g_strdup(name), g_free);
    /* Report a successful move even if the dialog closes just after it finishes. */
    g_task_set_check_cancellable(task, FALSE);
    g_task_run_in_thread(task, delete_profile);
}

gboolean profile_delete_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static gint compare_names(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

static char **finish_list(GPtrArray *items)
{
    g_ptr_array_sort(items, compare_names);
    g_ptr_array_add(items, NULL);
    return (char **)g_ptr_array_free(items, FALSE);
}

char **profiles_list(void)
{
    g_autofree char *directory = profiles_directory();
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!profile_name_valid(name)) continue;
        g_autofree char *path = g_build_filename(directory, name, NULL);
        struct stat info;
        if (g_lstat(path, &info) == 0 && S_ISDIR(info.st_mode))
            g_ptr_array_add(items, g_strdup(name));
    }
    return finish_list(items);
}

static gboolean socket_is_listening(const char *path)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (g_strlcpy(address.sun_path, path, sizeof address.sun_path) >= sizeof address.sun_path)
        return FALSE;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return FALSE;
    int result = connect(fd, (struct sockaddr *)&address, sizeof address);
    /* A full Unix accept queue is still a live listener. Never block discovery. */
    gboolean listening = result == 0 || errno == EAGAIN || errno == EINPROGRESS;
    close(fd);
    return listening;
}

char *profile_socket_name(const char *path)
{
    g_autofree char *name = g_path_get_basename(path);
    return g_str_has_prefix(name, "relapse-") && g_str_has_suffix(name, ".sock")
        ? g_strndup(name + 8, strlen(name) - 13) : g_strdup("No Profile");
}

char **profile_sockets_list(const char *directory)
{
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!g_str_equal(name, "relapse.sock")) {
            if (!g_str_has_prefix(name, "relapse-") || !g_str_has_suffix(name, ".sock")) continue;
            g_autofree char *profile = g_strndup(name + 8, strlen(name) - 13);
            if (!profile_name_valid(profile)) continue;
        }
        g_autofree char *path = g_build_filename(directory, name, NULL);
        struct stat info;
        if (g_lstat(path, &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == getuid() &&
            socket_is_listening(path))
            g_ptr_array_add(items, g_steal_pointer(&path));
    }
    return finish_list(items);
}

gboolean profile_is_running(const char *name)
{
    if (!profile_name_valid(name)) return FALSE;
    g_autofree char *directory = profiles_directory();
    g_autofree char *path = g_build_filename(directory, name, ".lock", NULL);
    int fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (fd < 0) return FALSE;
    /* Status readers may overlap; only an exclusive launcher/maintenance lock
     * should make the profile appear busy. */
    gboolean busy = flock(fd, LOCK_SH | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    close(fd);
    return busy;
}

static pid_t process_parent(pid_t pid)
{
    g_autofree char *path = g_strdup_printf("/proc/%d/stat", (int)pid);
    g_autofree char *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL)) return 0;
    const char *close = strrchr(data, ')');
    if (!close || close[1] != ' ') return 0;
    int parent = 0;
    if (sscanf(close + 2, "%*c %d", &parent) != 1) return 0;
    return parent > 0 ? (pid_t)parent : 0;
}

static gboolean process_owns_lock(pid_t pid, const struct stat *lock_info)
{
    g_autofree char *fds_path = g_strdup_printf("/proc/%d/fd", (int)pid);
    g_autoptr(GDir) fds = g_dir_open(fds_path, 0, NULL);
    const char *fd;
    while (fds && (fd = g_dir_read_name(fds))) {
        g_autofree char *target_path = g_strdup_printf("%s/%s", fds_path, fd);
        struct stat target;
        if (stat(target_path, &target) == 0 && target.st_dev == lock_info->st_dev &&
            target.st_ino == lock_info->st_ino) return TRUE;
    }
    return FALSE;
}

GPtrArray *profile_processes(const char *name, pid_t root)
{
    /* Everything a profile starts stays a descendant of the launcher, even
     * when the sandbox calls setsid, so the parent chain identifies it. */
    GPtrArray *pids = g_ptr_array_new();
    if (!profile_name_valid(name)) return pids;
    g_autofree char *directory = profiles_directory();
    g_autofree char *lock_path = g_build_filename(directory, name, ".lock", NULL);
    struct stat lock_info;
    gboolean have_lock = stat(lock_path, &lock_info) == 0;
    GHashTable *parents = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *roots = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_autoptr(GDir) processes = g_dir_open("/proc", 0, NULL);
    const char *entry;
    pid_t self = getpid();
    while (processes && (entry = g_dir_read_name(processes))) {
        if (!*entry || strspn(entry, "0123456789") != strlen(entry)) continue;
        pid_t pid = (pid_t)g_ascii_strtoll(entry, NULL, 10);
        if (pid <= 1) continue;
        pid_t parent = process_parent(pid);
        if (parent > 0) g_hash_table_insert(parents, GINT_TO_POINTER(pid), GINT_TO_POINTER(parent));
        if (have_lock && process_owns_lock(pid, &lock_info))
            g_hash_table_add(roots, GINT_TO_POINTER(pid));
    }
    if (root > 1 && root != self) g_hash_table_add(roots, GINT_TO_POINTER(root));
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, parents);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        pid_t pid = GPOINTER_TO_INT(key);
        if (pid == self) continue;
        pid_t current = pid;
        for (guint depth = 0; depth < 64; ++depth) {
            gpointer parent = g_hash_table_lookup(parents, GINT_TO_POINTER(current));
            if (!parent) break;
            current = GPOINTER_TO_INT(parent);
            if (g_hash_table_contains(roots, GINT_TO_POINTER(current))) {
                g_ptr_array_add(pids, GINT_TO_POINTER(pid));
                break;
            }
        }
    }
    g_hash_table_iter_init(&iter, roots);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        pid_t pid = GPOINTER_TO_INT(key);
        if (pid != self) g_ptr_array_add(pids, GINT_TO_POINTER(pid));
    }
    g_hash_table_unref(parents);
    g_hash_table_unref(roots);
    return pids;
}

void profile_processes_kill(GPtrArray *pids, int signal_number)
{
    if (!pids) return;
    for (guint i = 0; i < pids->len; ++i) {
        pid_t pid = GPOINTER_TO_INT(g_ptr_array_index(pids, i));
        if (pid > 1) kill(pid, signal_number);
    }
}

void profile_processes_free(GPtrArray *pids)
{
    if (pids) g_ptr_array_unref(pids);
}

static void detach_launcher(gpointer user_data)
{
    (void)user_data;
    setsid();
}

char *profile_join_uri(const char *place_id, const char *job_id, GError **error)
{
    g_autofree char *place = g_strdup(place_id ? place_id : "");
    g_autofree char *job = g_strdup(job_id ? job_id : "");
    g_strstrip(place);
    g_strstrip(job);
    if (!*place) {
        if (*job) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "A Job ID needs a Place ID.");
        return NULL;
    }
    guint64 id;
    if (strspn(place, "0123456789") != strlen(place) ||
        !g_ascii_string_to_unsigned(place, 10, 1, G_MAXINT64, &id, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Place ID must be a positive number.");
        return NULL;
    }
    if (*job && !g_uuid_string_is_valid(job)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Enter a valid Job ID.");
        return NULL;
    }
    return g_strdup_printf("roblox://placeId=%" G_GUINT64_FORMAT "%s%s", id,
        *job ? "&gameInstanceId=" : "", job);
}

GSubprocess *profile_launch(const char *launcher, const char *name,
                            const char *place_id, const char *job_id, GError **error)
{
    if (!profile_name_valid(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Use 1–64 letters, digits, dots, underscores or hyphens for the Profile name (excluding . and ..).");
        return NULL;
    }
    if (!launcher || !g_path_is_absolute(launcher) ||
        !g_file_test(launcher, G_FILE_TEST_IS_REGULAR) || g_access(launcher, X_OK) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Select an executable launcher in Settings first.");
        return NULL;
    }
    if (profile_is_running(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "This Profile is already running or being maintained.");
        return NULL;
    }
    g_autoptr(GError) destination_error = NULL;
    g_autofree char *uri = profile_join_uri(place_id, job_id, &destination_error);
    if (destination_error) {
        g_propagate_error(error, g_steal_pointer(&destination_error));
        return NULL;
    }
    g_autoptr(GSubprocessLauncher) spawn = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_unsetenv(spawn, "LD_PRELOAD");
    g_subprocess_launcher_unsetenv(spawn, "LD_AUDIT");
    g_subprocess_launcher_unsetenv(spawn, "LD_LIBRARY_PATH");
    g_subprocess_launcher_set_child_setup(spawn, detach_launcher, NULL, NULL);
    const char *argv[] = {launcher, "--profile", name, "--no-launch-ui", uri ? "--" : NULL, uri, NULL};
    return g_subprocess_launcher_spawnv(spawn, argv, error);
}
