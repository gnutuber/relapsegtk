#pragma once

#include <gio/gio.h>

typedef enum {
    OPENER_EDITOR,
    OPENER_FOLDER,
    OPENER_SYSTEM,
    OPENER_COMMAND,
    OPENER_NOTHING,
} OpenerMode;

typedef struct {
    OpenerMode mode;
    char *command;
} OpenerSettings;

OpenerSettings opener_settings_defaults(void);
void opener_settings_clear(OpenerSettings *settings);
gboolean opener_settings_load(OpenerSettings *settings, GError **error);
gboolean opener_settings_save(const OpenerSettings *settings, GError **error);
gboolean opener_open(GFile *file, const OpenerSettings *settings, GError **error);
