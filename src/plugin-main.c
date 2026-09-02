/*
Canvas Clone
Copyright (C) 2025 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "canvas-clone-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* The canvas API this plugin is built on landed in OBS Studio 31.1. */
#define REQUIRED_OBS_VERSION MAKE_SEMANTIC_VERSION(31, 1, 0)

bool obs_module_load(void)
{
	const uint32_t version = obs_get_version();

	if (version < REQUIRED_OBS_VERSION) {
		obs_log(LOG_ERROR, "OBS Studio 31.1.0 or newer is required, found %u.%u.%u", version >> 24,
			(version >> 16) & 0xFF, version & 0xFFFF);
		return false;
	}

	obs_register_source(&canvas_clone_source_info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
