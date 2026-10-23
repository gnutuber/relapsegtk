#pragma once

#include <glib.h>

typedef struct SocketClient SocketClient;

typedef enum {
    SOCKET_CLIENT_DISCONNECTED,
    SOCKET_CLIENT_CONNECTING,
    SOCKET_CLIENT_WAITING,
    SOCKET_CLIENT_READY
} SocketClientState;

typedef enum {
    SOCKET_CLIENT_QUEUED,
    SOCKET_CLIENT_NOT_READY,
    SOCKET_CLIENT_TOO_LARGE,
    SOCKET_CLIENT_BACKPRESSURE,
    SOCKET_CLIENT_IDS_EXHAUSTED
} SocketClientSendResult;

typedef struct {
    void (*state_changed)(SocketClientState state, const char *detail, gpointer user_data);
    void (*result)(gint64 script_id, gboolean submitted, const char *detail, gpointer user_data);
    void (*log)(const char *level, const char *text, gpointer user_data);
    void (*error)(const char *detail, gpointer user_data);
} SocketClientHandlers;

SocketClient *socket_client_new(const char *path, const SocketClientHandlers *handlers, gpointer user_data);
void socket_client_start(SocketClient *client);
gboolean socket_client_is_ready(const SocketClient *client);
gint64 socket_client_get_session(const SocketClient *client);
const char *socket_client_get_path(const SocketClient *client);
GPid socket_client_get_peer_pid(const SocketClient *client);
SocketClientSendResult socket_client_execute(SocketClient *client, const char *name, const char *source,
                                             gint64 *request_id);
void socket_client_free(SocketClient *client);
