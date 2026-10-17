#pragma once

#include "socket/client.h"

typedef struct OutputController OutputController;
typedef enum {
    OUTPUT_OFF, OUTPUT_WAITING, OUTPUT_CONNECTING, OUTPUT_ACTIVE, OUTPUT_FAILED
} OutputState;
typedef struct {
    void (*state_changed)(OutputState state, const char *detail, gpointer user_data);
    void (*entry)(const char *level, const char *text, gpointer user_data);
} OutputHandlers;

OutputController *output_controller_new(SocketClient *client, const OutputHandlers *handlers, gpointer user_data);
void output_controller_set_enabled(OutputController *output, gboolean enabled);
void output_controller_sync(OutputController *output);
gboolean output_controller_result(OutputController *output, gint64 id, gboolean submitted, const char *detail);
void output_controller_free(OutputController *output);
