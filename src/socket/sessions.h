#pragma once

#include "client.h"
#include "socket/bridge.h"

typedef struct Sessions Sessions;
typedef struct {
    void (*state_changed)(const char *path, SocketClientState state, const char *detail, gpointer user_data);
    void (*output_changed)(const char *path, OutputState state, const char *detail, gpointer user_data);
    void (*entry)(const char *path, const char *level, const char *text, gpointer user_data);
    void (*result)(const char *path, gint64 id, gboolean submitted, const char *detail, gpointer user_data);
} SessionHandlers;

Sessions *sessions_new(const SessionHandlers *handlers, gpointer user_data);
void sessions_update(Sessions *sessions, const char *const *paths);
gboolean sessions_is_active(Sessions *sessions, const char *path);
void sessions_set_output_enabled(Sessions *sessions, gboolean enabled);
SocketClient *sessions_get_client(Sessions *sessions, const char *path);
SocketClientState sessions_get_state(Sessions *sessions, const char *path);
OutputState sessions_get_output_state(Sessions *sessions, const char *path);
gboolean sessions_can_execute(Sessions *sessions, const char *path);
SocketClientSendResult sessions_execute(Sessions *sessions, const char *path, const char *name, const char *source);
void sessions_free(Sessions *sessions);
