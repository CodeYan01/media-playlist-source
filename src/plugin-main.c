/*
Media Playlist Source
Copyright (C) 2023 Ian Rodriguez ianlemuelr@gmail.com

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

#include "plugin-macros.generated.h"

#ifdef TEST_SHUFFLER
#include "shuffler.h"
#endif
#define TEST_VOSK
#ifdef TEST_VOSK
#include "vosk-filter.h"
#endif // TEST_VOSK

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info media_playlist_source_info;
extern struct obs_source_info vosk_filter_info;
bool loaded = false;

static void vf_frontend_event_cb(enum obs_frontend_event event,
					      void *data)
{
	UNUSED_PARAMETER(data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		loaded = true;
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		loaded = false;
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "plugin loaded successfully (version %s)",
	     PLUGIN_VERSION);
	obs_register_source(&media_playlist_source_info);
	obs_register_source(&vosk_filter_info);
	obs_frontend_add_event_callback(vf_frontend_event_cb,
					NULL);
#ifdef TEST_SHUFFLER
	test_shuffler();
#endif
#ifdef TEST_VOSK
	test_get_line_cutoff();
#endif
	return true;
}

void obs_module_unload()
{
	blog(LOG_INFO, "plugin unloaded");
}
