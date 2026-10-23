#pragma once

#include "socket/client.h"

typedef struct UiBridge UiBridge;
typedef enum {
    OUTPUT_OFF, OUTPUT_WAITING, OUTPUT_CONNECTING, OUTPUT_ACTIVE, OUTPUT_FAILED
} OutputState;
typedef struct {
    void (*state_changed)(OutputState state, const char *detail, gpointer user_data);
    void (*entry)(const char *level, const char *text, gpointer user_data);
} UiBridgeHandlers;

UiBridge *ui_bridge_new(SocketClient *client, const UiBridgeHandlers *handlers, gpointer user_data);
void ui_bridge_set_output_enabled(UiBridge *bridge, gboolean enabled);
void ui_bridge_sync(UiBridge *bridge);
gboolean ui_bridge_result(UiBridge *bridge, gint64 id, gboolean submitted, const char *detail);
gboolean ui_bridge_is_ready(UiBridge *bridge);
SocketClientSendResult ui_bridge_execute(UiBridge *bridge, const char *name, const char *source);
void ui_bridge_free(UiBridge *bridge);
