#include "portable.h"

#include "store.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#define TAR_BLOCK 512u
#define BUNDLE_MAX_BYTES (64u * 1024u * 1024u)
#define MEMBER_MAX_BYTES (48u * 1024u * 1024u)
#define MEMBER_SETTINGS "settings.ini"
#define MEMBER_BACKGROUNDS "backgrounds/"

static void tar_octal(char *field, gsize width, guint64 value)
{
    g_snprintf(field, width, "%0*lo", (int)width - 1, (unsigned long)value);
}

static void tar_checksum(guint8 *header)
{
    memset(header + 148, ' ', 8);
    unsigned sum = 0;
    for (guint i = 0; i < TAR_BLOCK; ++i)
        sum += header[i];
    g_snprintf((char *)header + 148, 7, "%06o", sum);
    header[155] = ' ';
}

static gboolean tar_add(GByteArray *tar, const char *name, const guint8 *data, gsize size,
                        GError **error)
{
    if (strlen(name) > 99) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "The name “%s” is too long for the bundle.", name);
        return FALSE;
    }
    guint8 header[TAR_BLOCK];
    memset(header, 0, sizeof header);
    memcpy(header, name, strlen(name));
    tar_octal((char *)header + 100, 8, 0644);
    tar_octal((char *)header + 108, 8, 0);
    tar_octal((char *)header + 116, 8, 0);
    tar_octal((char *)header + 124, 12, size);
    tar_octal((char *)header + 136, 12, 0);
    header[156] = '0';
    memcpy(header + 257, "ustar", 6);
    header[263] = '0';
    header[264] = '0';
    tar_checksum(header);
    g_byte_array_append(tar, header, TAR_BLOCK);
    if (size)
        g_byte_array_append(tar, data, size);
    gsize padding = (TAR_BLOCK - size % TAR_BLOCK) % TAR_BLOCK;
    if (padding) {
        guint8 zeros[TAR_BLOCK] = {0};
        g_byte_array_append(tar, zeros, padding);
    }
    return TRUE;
}

static GBytes *gzip_compress(GByteArray *tar, GError **error)
{
    GOutputStream *memory = g_memory_output_stream_new_resizable();
    GConverter *converter = G_CONVERTER(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1));
    GOutputStream *stream = g_converter_output_stream_new(memory, converter);
    gboolean ok = g_output_stream_write_all(stream, tar->data, tar->len, NULL, NULL, error) &&
        g_output_stream_close(stream, NULL, error);
    g_object_unref(stream);
    if (!ok) {
        g_object_unref(memory);
        return NULL;
    }
    gsize size = g_memory_output_stream_get_size(G_MEMORY_OUTPUT_STREAM(memory));
    g_autoptr(GBytes) bytes = g_bytes_new(g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(memory)), size);
    g_object_unref(memory);
    return g_steal_pointer(&bytes);
}

static GBytes *gzip_decompress(const guint8 *data, gsize size, GError **error)
{
    GInputStream *memory = g_memory_input_stream_new_from_data(data, size, NULL);
    GConverter *converter = G_CONVERTER(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP));
    GInputStream *stream = g_converter_input_stream_new(memory, converter);
    GByteArray *out = g_byte_array_new();
    guint8 buffer[65536];
    gboolean ok = TRUE;
    for (;;) {
        gssize read = g_input_stream_read(stream, buffer, sizeof buffer, NULL, error);
        if (read < 0) {
            ok = FALSE;
            break;
        }
        if (read == 0)
            break;
        if (out->len + (gsize)read > BUNDLE_MAX_BYTES) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "The bundle is larger than %u MiB.", BUNDLE_MAX_BYTES / (1024 * 1024));
            ok = FALSE;
            break;
        }
        g_byte_array_append(out, buffer, (guint)read);
    }
    g_object_unref(stream);
    g_object_unref(memory);
    if (!ok) {
        g_byte_array_free(out, TRUE);
        return NULL;
    }
    return g_byte_array_free_to_bytes(out);
}

gboolean portable_export(const char *path, GError **error)
{
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file)
        return FALSE;

    g_autofree char *image = g_key_file_get_string(file, "Appearance", "image", NULL);
    g_autofree char *member = NULL;
    g_autoptr(GBytes) image_bytes = NULL;
    if (image && *image) {
        g_autofree char *basename = g_path_get_basename(image);
        if (basename && *basename && strcmp(basename, ".") != 0 && strcmp(basename, "..") != 0) {
            member = g_strconcat(MEMBER_BACKGROUNDS, basename, NULL);
            g_autofree char *contents = NULL;
            gsize size = 0;
            if (g_file_get_contents(image, &contents, &size, NULL) && size > 0)
                image_bytes = g_bytes_new_take(g_steal_pointer(&contents), size);
        }
        if (image_bytes)
            g_key_file_set_string(file, "Appearance", "image", member);
        else
            g_key_file_remove_key(file, "Appearance", "image", NULL);
    }

    gsize settings_size = 0;
    g_autofree char *settings = g_key_file_to_data(file, &settings_size, NULL);
    g_autoptr(GByteArray) tar = g_byte_array_new();
    if (!tar_add(tar, MEMBER_SETTINGS, (const guint8 *)settings, settings_size, error))
        return FALSE;
    if (image_bytes) {
        gsize size = 0;
        const guint8 *data = g_bytes_get_data(image_bytes, &size);
        if (!tar_add(tar, member, data, size, error))
            return FALSE;
    }
    guint8 end[TAR_BLOCK] = {0};
    g_byte_array_append(tar, end, TAR_BLOCK);
    g_byte_array_append(tar, end, TAR_BLOCK);

    g_autoptr(GBytes) compressed = gzip_compress(tar, error);
    if (!compressed)
        return FALSE;
    gsize size = 0;
    const void *data = g_bytes_get_data(compressed, &size);
    g_autofree char *parent = g_path_get_dirname(path);
    g_mkdir_with_parents(parent, 0700);
    return g_file_set_contents_full(path, data, (gssize)size,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0644, error);
}

static gboolean tar_parse(GBytes *tar, GHashTable *members, GError **error)
{
    gsize length = 0;
    const guint8 *data = g_bytes_get_data(tar, &length);
    gsize offset = 0;
    while (offset + TAR_BLOCK <= length) {
        const guint8 *header = data + offset;
        gboolean empty = TRUE;
        for (guint i = 0; i < TAR_BLOCK; ++i)
            if (header[i]) {
                empty = FALSE;
                break;
            }
        if (empty)
            break;
        g_autofree char *name = g_strndup((const char *)header, strnlen((const char *)header, 100));
        char size_field[13];
        memcpy(size_field, header + 124, 12);
        size_field[12] = '\0';
        guint64 size = g_ascii_strtoull(size_field, NULL, 8);
        guint8 type = header[156];
        offset += TAR_BLOCK;
        if (offset + size > length) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "The bundle is truncated.");
            return FALSE;
        }
        if (type != '0' && type != '\0')
            goto next;
        if (!name || !*name || name[0] == '/' || strstr(name, ".."))
            goto next;
        if (strcmp(name, MEMBER_SETTINGS) != 0 && !g_str_has_prefix(name, MEMBER_BACKGROUNDS))
            goto next;
        if (size > MEMBER_MAX_BYTES) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "A file inside the bundle is larger than %u MiB.",
                        MEMBER_MAX_BYTES / (1024 * 1024));
            return FALSE;
        }
        g_hash_table_replace(members, g_strdup(name),
                             g_bytes_new(data + offset, (gsize)size));
    next:
        offset += (gsize)((size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK);
    }
    return TRUE;
}

static gboolean extract_background(const char *member, GBytes *bytes, char **path, GError **error)
{
    g_autofree char *basename = g_path_get_basename(member);
    if (!basename || !*basename || strcmp(basename, ".") == 0 || strcmp(basename, "..") == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The bundle contains an invalid background name.");
        return FALSE;
    }
    g_autofree char *config = g_build_filename(g_get_user_config_dir(), "relapsegtk", NULL);
    g_autofree char *directory = g_build_filename(config, "backgrounds", NULL);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not create the background folder: %s", g_strerror(saved_errno));
        return FALSE;
    }
    g_autofree char *target = g_build_filename(directory, basename, NULL);
    gsize size = 0;
    const void *data = g_bytes_get_data(bytes, &size);
    if (!g_file_set_contents_full(target, data, (gssize)size,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error))
        return FALSE;
    *path = g_steal_pointer(&target);
    return TRUE;
}

gboolean portable_import(const char *path, GError **error)
{
    g_autofree char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, error))
        return FALSE;
    if (length == 0 || length > BUNDLE_MAX_BYTES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "This file is not a RelapseGtk bundle.");
        return FALSE;
    }
    g_autoptr(GBytes) raw = g_bytes_new_take(g_steal_pointer(&contents), length);
    g_autoptr(GBytes) tar = gzip_decompress(g_bytes_get_data(raw, NULL), length, error);
    if (!tar)
        return FALSE;

    g_autoptr(GHashTable) members = g_hash_table_new_full(g_str_hash, g_str_equal,
        g_free, (GDestroyNotify)g_bytes_unref);
    if (!tar_parse(tar, members, error))
        return FALSE;
    GBytes *settings_bytes = g_hash_table_lookup(members, MEMBER_SETTINGS);
    if (!settings_bytes) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The bundle does not contain a settings file.");
        return FALSE;
    }
    gsize settings_size = 0;
    const guint8 *settings_data = g_bytes_get_data(settings_bytes, &settings_size);
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_data(file, (const char *)settings_data, settings_size,
            G_KEY_FILE_KEEP_COMMENTS, error))
        return FALSE;

    g_autofree char *image = g_key_file_get_string(file, "Appearance", "image", NULL);
    if (image && *image) {
        if (!g_str_has_prefix(image, MEMBER_BACKGROUNDS)) {
            g_key_file_remove_key(file, "Appearance", "image", NULL);
        } else {
            GBytes *image_bytes = g_hash_table_lookup(members, image);
            if (!image_bytes) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The bundle is missing its background image.");
                return FALSE;
            }
            g_autofree char *target = NULL;
            if (!extract_background(image, image_bytes, &target, error))
                return FALSE;
            g_key_file_set_string(file, "Appearance", "image", target);
        }
    }
    return store_save(file, error);
}
