#pragma once

#include <gio/gio.h>

typedef struct {
    char *name;
    char *uri;
    char *text;
    gboolean modified;
    int cursor;
} SessionTab;

typedef struct {
    GPtrArray *tabs;
    guint active_tab;
    guint next_untitled;
} Session;

Session *session_new(void);
void session_free(Session *session);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Session, session_free)

gboolean session_load(Session **session, GError **error);
gboolean session_save(const Session *session, GError **error);

typedef struct SessionWriter SessionWriter;
typedef void (*SessionErrorFunc)(const char *detail, gpointer user_data);

SessionWriter *session_writer_new(GApplication *application, SessionErrorFunc error, gpointer user_data);
void session_writer_queue(SessionWriter *writer, Session *session);
void session_writer_free(SessionWriter *writer);
