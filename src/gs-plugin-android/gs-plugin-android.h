/*
 * Copyright (C) 2024 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <glib.h>
#include <gnome-software.h>

G_BEGIN_DECLS

#define GS_TYPE_PLUGIN_ANDROID (gs_plugin_android_get_type ())

G_DECLARE_FINAL_TYPE (GsPluginAndroid, gs_plugin_android, GS, PLUGIN_ANDROID, GsPlugin)

G_END_DECLS
