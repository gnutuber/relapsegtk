#pragma once

#include <gtk/gtk.h>
#include "settings/editor.h"

GtkWidget *editor_new(const EditorSettings *settings);
void editor_apply_settings(GtkWidget *editor, const EditorSettings *settings);
