#pragma once

#include <gdk/gdk.h>

#include "settings/appearance.h"

void style_install(GdkDisplay *display);
void style_set_editor_font_size(GdkDisplay *display, int size);
void style_set_appearance(GdkDisplay *display, const AppearanceSettings *settings);
gboolean style_animations_disabled(void);
