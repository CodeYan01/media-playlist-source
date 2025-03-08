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

#include "media-playlist-source.h"

#define S_PLAYLIST "playlist"
#define S_LOOP "loop"
#define S_SHUFFLE "shuffle"
#define S_VISIBILITY_BEHAVIOR "visibility_behavior"
#define S_RESTART_BEHAVIOR "restart_behavior"
#define S_CURRENT_FILE_NAME "current_file_name"
#define S_SELECT_FILE "select_file"

#define S_CURRENT_MEDIA_INDEX "current_media_index"
#define S_CURRENT_FOLDER_ITEM_FILENAME "current_folder_item_filename"
#define S_ID "uuid"
#define S_IS_URL "is_url"
#define S_SPEED "speed_percent"
#define S_REFRESH_FILENAME "refresh_filename"

/* Media Source Settings */
#define S_FFMPEG_LOCAL_FILE "local_file"
#define S_FFMPEG_INPUT "input"
#define S_FFMPEG_IS_LOCAL_FILE "is_local_file"
#define S_FFMPEG_HW_DECODE "hw_decode"
#define S_FFMPEG_CLOSE_WHEN_INACTIVE "close_when_inactive"
#define S_FFMPEG_RESTART_ON_ACTIVATE "restart_on_activate"

#define T_(text) obs_module_text(text)
#define T_PLAYLIST T_("Playlist")
#define T_LOOP T_("LoopPlaylist")
#define T_SHUFFLE T_("Shuffle")
#define T_VISIBILITY_BEHAVIOR T_("VisibilityBehavior")
#define T_VISIBILITY_BEHAVIOR_STOP_RESTART T_("VisibilityBehavior.StopRestart")
#define T_VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE T_("VisibilityBehavior.PauseUnpause")
#define T_VISIBILITY_BEHAVIOR_ALWAYS_PLAY T_("VisibilityBehavior.AlwaysPlay")
#define T_VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT T_("VisibilityBehavior.StopPlayNext")
#define T_RESTART_BEHAVIOR T_("RestartBehavior")
#define T_RESTART_BEHAVIOR_CURRENT_FILE T_("RestartBehavior.CurrentFile")
#define T_RESTART_BEHAVIOR_FIRST_FILE T_("RestartBehavior.FirstFile")
#define T_CURRENT_FILE_NAME T_("CurrentFileName")
#define T_SELECT_FILE T_("SelectFile")
#define T_NO_FILE_SELECTED T_("NoFileSelected")
#define T_USE_HARDWARE_DECODING T_("UseHardwareDecoding")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE T_("CloseFileWhenInactive")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP T_("CloseFileWhenInactive.Tooltip")
#define T_SPEED T_("Speed")
#define T_SPEED_WARNING T_("SpeedWarning")
#define T_REFRESH_FILENAME T_("RefreshFilename")

#define T_PLAY_PAUSE T_("PlayPause")
#define T_RESTART T_("Restart")
#define T_STOP T_("Stop")
#define T_PLAYLIST_NEXT T_("Next")
#define T_PLAYLIST_PREV T_("Previous")

static inline void set_current_media_index(struct media_playlist_source *mps, size_t index)
{
	if (mps->files.num) {
		if (index >= mps->files.num) {
			index = 0;
		}
		mps->current_media_index = index;
		mps->current_media = &mps->files.array[index];
	} else {
		mps->current_media_index = 0;
		mps->current_media = NULL;
	}
}

/* Requires setting current media index first
 */
static inline void set_current_folder_item_index(struct media_playlist_source *mps, size_t index)
{
	bfree(mps->current_media_filename);
	mps->current_media_filename = NULL;
	if (mps->current_media) {
		if (!mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;
			mps->actual_media = mps->current_media;
			return;
		}

		if (index < mps->current_media->folder_items.num) {
			mps->current_folder_item_index = index;
		} else {
			mps->current_folder_item_index = index = 0;
		}
		mps->actual_media = &mps->current_media->folder_items.array[index];
		mps->current_media_filename = bstrdup(mps->actual_media->filename);
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = NULL;
	}
}

static inline void reset_folder_item_index(struct media_playlist_source *mps)
{
	mps->current_folder_item_index = 0;
	bfree(mps->current_media_filename);
	mps->current_media_filename = NULL;
}

static bool valid_extension(const char *ext)
{
	struct dstr haystack = {0};
	struct dstr needle = {0};
	bool valid = false;

	if (!ext || !*ext)
		return false;

	dstr_copy(&haystack, media_filter);
	dstr_cat(&haystack, video_filter);
	dstr_cat(&haystack, audio_filter);

	dstr_cat_ch(&needle, '*');
	dstr_cat(&needle, ext);

	valid = dstr_find_i(&haystack, needle.array);

	dstr_free(&haystack);
	dstr_free(&needle);
	return valid;
}

static void update_current_filename_setting(struct media_playlist_source *mps, obs_data_t *data)
{
	struct dstr long_desc = {0};
	if (!mps || !data)
		return;
	if (!mps->actual_media) {
		obs_data_set_string(data, S_CURRENT_FILE_NAME, " ");
		return;
	} else if (mps->actual_media->parent) {
		dstr_catf(&long_desc, "%zu-%zu", mps->actual_media->parent->index + 1, mps->actual_media->index + 1);
	} else {
		dstr_catf(&long_desc, "%zu", mps->actual_media->index + 1);
	}
	dstr_catf(&long_desc, ": %s", mps->actual_media->path);
	obs_data_set_string(data, S_CURRENT_FILE_NAME, long_desc.array);
	dstr_free(&long_desc);
}

/* Empties path selected in media source,
 * for when there is no item in the list
 */
static void clear_media_source(void *data)
{
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, S_FFMPEG_IS_LOCAL_FILE, true);
	obs_data_set_string(settings, S_FFMPEG_INPUT, "");
	obs_data_set_string(settings, S_FFMPEG_LOCAL_FILE, "");
	obs_source_update(mps->current_media_source, settings);
	obs_data_release(settings);
	obs_source_media_stop(mps->source);
}

/* Checks if the media source has to be updated, because updating its
 * settings causes it to restart. Can also force update it.
 * Should first call set_current_media_index before calling this
 *
 * Forced updates:
 * * Using play_folder_item_at_index
 * * Using play_media_at_index
 * * during mps_update (files/folders can be changed)
 */
static void update_media_source(void *data, bool forced)
{
	struct media_playlist_source *mps = data;
	obs_source_t *media_source = mps->current_media_source;
	obs_data_t *settings = obs_source_get_settings(media_source);
	if (mps->current_media->is_folder) {
		assert(mps->current_folder_item_index < mps->current_media->folder_items.num);
		mps->actual_media = &mps->current_media->folder_items.array[mps->current_folder_item_index];
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = mps->current_media;
	}

	//bool current_is_url =
	//	!obs_data_get_bool(settings, S_FFMPEG_IS_LOCAL_FILE);
	const char *path_setting = mps->actual_media->is_url ? S_FFMPEG_INPUT : S_FFMPEG_LOCAL_FILE;

	/*forced = forced || current_is_url != mps->current_media->is_url;
	if (!forced) {
		const char *path = obs_data_get_string(settings, path_setting);
		forced = strcmp(path, mps->current_media->path) != 0;
	}*/

	if (forced) {
		obs_data_set_bool(settings, S_FFMPEG_IS_LOCAL_FILE, !mps->actual_media->is_url);
		obs_data_set_string(settings, path_setting, mps->actual_media->path);
		obs_data_set_int(settings, S_SPEED, mps->speed);
		obs_source_update(media_source, settings);
		mps->user_stopped = false;
	}

	obs_data_release(settings);
}

static void select_index_proc_(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index)
{
	if (media_index < mps->files.num) {
		pthread_mutex_lock(&mps->mutex);
		set_current_media_index(mps, media_index);
		set_current_folder_item_index(mps, folder_item_index);
		if (mps->actual_media) {
			update_media_source(mps, true);
			if (mps->shuffle) {
				shuffler_select(&mps->shuffler, mps->actual_media);
			}
		}
		pthread_mutex_unlock(&mps->mutex);
	}
}

static void select_index_proc(void *data, calldata_t *cd)
{
	struct media_playlist_source *mps = data;
	long long media_index = 0;
	long long folder_item_index = 0;
	calldata_get_int(cd, "media_index", &media_index);
	calldata_get_int(cd, "folder_item_index", &folder_item_index);
	select_index_proc_(mps, media_index, folder_item_index);
}

static void play_folder_item_at_index(void *data, size_t index)
{
	struct media_playlist_source *mps = data;
	if (mps->current_media->is_folder && index < mps->current_media->folder_items.num) {
		mps->current_folder_item_index = index;
		mps->actual_media = &mps->current_media->folder_items.array[index];
		bfree(mps->current_media_filename);
		mps->current_media_filename = bstrdup(mps->actual_media->filename);
		update_media_source(mps, true);
		obs_source_save(mps->source);
	}
}

static void play_media_at_index(void *data, size_t index, bool play_last_folder_item)
{
	struct media_playlist_source *mps = data;
	if (index >= mps->files.num)
		return;

	set_current_media_index(mps, index);
	if (mps->current_media->is_folder) {
		if (mps->current_media->folder_items.num) {
			// when Previous Item is clicked to go back to a folder
			if (play_last_folder_item) {
				mps->current_folder_item_index = mps->current_media->folder_items.num - 1;
			} else {
				mps->current_folder_item_index = 0;
			}

			play_folder_item_at_index(mps, mps->current_folder_item_index);
		} else {
			if (play_last_folder_item) {
				obs_source_media_previous(mps->source);
			} else {
				obs_source_media_next(mps->source);
			}
		}
		return;
	}
	update_media_source(mps, true);
	obs_source_save(mps->source);
}

static size_t find_folder_item_index(struct darray *array, const char *filename)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	for (size_t i = 0; i < files.num; i++) {
		if (strcmp(files.array[i].filename, filename) == 0) {
			return i;
		}
	}

	return DARRAY_INVALID;
}

/* ------------------------------------------------------------------------- */

static void set_media_state(void *data, enum obs_media_state state)
{
	struct media_playlist_source *mps = data;
	mps->state = state;
}

static enum obs_media_state mps_get_state(void *data)
{
	struct media_playlist_source *mps = data;
	enum obs_media_state media_state;
	if (mps->files.num) {
		media_state = obs_source_media_get_state(mps->current_media_source);
	} else {
		media_state = OBS_MEDIA_STATE_NONE;
	}
	UNUSED_PARAMETER(mps);
	return media_state;
}

static void mps_end_reached(void *data)
{
	struct media_playlist_source *mps = data;
	set_media_state(mps, OBS_MEDIA_STATE_ENDED);
	obs_source_media_ended(mps->source);
	set_current_media_index(mps, 0);
	obs_source_save(mps->source);
}

static void media_source_ended(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct media_playlist_source *mps = data;

	/* In OBS 29.1.3 and below, stopping a currently playing media source triggers
	 * both the STOPPED and ENDED signals. In the future, it should actually just
	 * be STOPPED. TODO: Remove `user_stopped` if PR #9218 gets merged.
	 *
	 * EDIT: Tested in OBS 31, now problem is deactivate is sending an ENDED signal
	 * rather than a STOPPED. So in mps_deactivate, we set user_stopped to true
	 */
	if (mps->user_stopped) {
		mps->user_stopped = false;
		return;
	} else if (mps->current_media_index < mps->files.num - 1 || mps->loop) {
		obs_source_media_next(mps->source);
	} else {
		mps_end_reached(mps);
	}
}

void mps_audio_callback(void *data, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);
	UNUSED_PARAMETER(source);
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->audio_mutex);
	size_t size = audio_data->frames * sizeof(float);
	for (size_t i = 0; i < mps->num_channels; i++) {
		deque_push_back(&mps->audio_data[i], audio_data->data[i], size);
	}
	deque_push_back(&mps->audio_frames, &audio_data->frames, sizeof(audio_data->frames));
	deque_push_back(&mps->audio_timestamps, &audio_data->timestamp, sizeof(audio_data->timestamp));
	pthread_mutex_unlock(&mps->audio_mutex);
}

static bool play_selected_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	signal_handler_t *sh = obs_source_get_signal_handler(mps->source);
	obs_data_t *settings = obs_source_get_settings(mps->source);
	size_t media_index = 0;
	size_t folder_item_index = 0;
	const char *index_str = obs_data_get_string(settings, S_SELECT_FILE);
	if (strcmp(index_str, "0") != 0) {
		char **indexes = strlist_split(index_str, '-', false);

		media_index = strtol(indexes[0], NULL, 10) - 1;
		if (indexes[1]) {
			folder_item_index = strtol(indexes[1], NULL, 10) - 1;
		}
		select_index_proc_(mps, media_index, folder_item_index);

		strlist_free(indexes);
	}
	obs_data_release(settings);
	/*update_current_filename_property(
		mps, obs_properties_get(props, S_CURRENT_FILE_NAME));*/
	update_current_filename_setting(mps, settings);
	//obs_source_update_properties(mps->source);

	signal_handler_signal(sh, "media_next", NULL);
	return true;
}

static bool refresh_filename_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	update_current_filename_setting(mps, settings);
	obs_source_update_properties(mps->source);
	obs_data_release(settings);
	return true;
}

/* ------------------------------------------------------------------------- */

static const char *mps_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("MediaPlaylistSource");
}

static void free_files(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *file = &files.array[i];
		bfree(file->filename);
		bfree(file->path);
		bfree(file->id);
		if (file->is_folder) {
			for (size_t j = 0; j < file->folder_items.num; j++) {
				struct media_file_data *folder_item = &file->folder_items.array[j];
				bfree(folder_item->filename);
				bfree(folder_item->path);
				bfree(folder_item->id);
			}
		}
		da_free(file->folder_items);
	}

	da_free(files);
}

static int64_t mps_get_duration(void *data)
{
	struct media_playlist_source *mps = data;

	return obs_source_media_get_duration(mps->current_media_source);
}

static int64_t mps_get_time(void *data)
{
	struct media_playlist_source *mps = data;

	return obs_source_media_get_time(mps->current_media_source);
}

static void mps_set_time(void *data, int64_t ms)
{
	struct media_playlist_source *mps = data;

	obs_source_media_set_time(mps->current_media_source, ms);
}

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_play_pause(mps->source, !mps->paused);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_restart(mps->source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_stop(mps->source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_next(mps->source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_previous(mps->source);
}

static void mps_play_pause(void *data, bool pause)
{
	struct media_playlist_source *mps = data;

	obs_source_media_play_pause(mps->current_media_source, pause);
	mps->paused = pause;

	if (pause)
		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	else
		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
}

static void mps_restart(void *data)
{
	struct media_playlist_source *mps = data;

	mps->user_stopped = false;

	if (mps->restart_behavior == RESTART_BEHAVIOR_FIRST_FILE) {
		play_media_at_index(mps, 0, false);
	} else if (mps->restart_behavior == RESTART_BEHAVIOR_CURRENT_FILE) {
		if (mps->state == OBS_MEDIA_STATE_ENDED) {
			// Make sure that the first file is selected
			// We do it here, because updating a media source will restart it
			update_media_source(mps, true);
		}
		obs_source_media_restart(mps->current_media_source);
		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
	}
}

static void mps_stop(void *data)
{
	struct media_playlist_source *mps = data;

	mps->user_stopped = true;
	obs_source_media_stop(mps->current_media_source);
	set_media_state(mps, OBS_MEDIA_STATE_STOPPED);
}

static void mps_playlist_next(void *data)
{
	struct media_playlist_source *mps = data;
	bool last_folder_item_reached = false;

	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		if (shuffler_has_next(&mps->shuffler)) {
			mps->actual_media = shuffler_next(&mps->shuffler);
			bfree(mps->current_media_filename);
			if (mps->actual_media->parent_id) {
				mps->current_media = mps->actual_media->parent;
				mps->current_media_filename = bstrdup(mps->actual_media->filename);
				mps->current_folder_item_index = mps->actual_media->index;
			} else {
				mps->current_media = mps->actual_media;
				mps->current_media_filename = NULL;
				mps->current_folder_item_index = 0;
			}
			mps->current_media_index = mps->current_media->index;
			update_media_source(mps, true);
			obs_source_save(mps->source);
		}
		goto end;
	}

	if (mps->current_media->is_folder) {
		if (mps->current_media->folder_items.num > 0 &&
		    mps->current_folder_item_index < mps->current_media->folder_items.num - 1) {
			++mps->current_folder_item_index;
			play_folder_item_at_index(mps, mps->current_folder_item_index);
			goto end;
		} else {
			last_folder_item_reached = true;
			mps->current_folder_item_index = 0;
		}
	}

	if (!mps->current_media->is_folder || last_folder_item_reached) {
		if (mps->current_media_index < mps->files.num - 1) {
			++mps->current_media_index;
		} else if (mps->loop) {
			mps->current_media_index = 0;
		} else {
			goto end;
		}
		play_media_at_index(mps, mps->current_media_index, false);
	}

end:
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_playlist_prev(void *data)
{
	struct media_playlist_source *mps = data;
	bool is_first_folder_item = false;

	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		if (shuffler_has_prev(&mps->shuffler)) {
			mps->actual_media = shuffler_prev(&mps->shuffler);
			bfree(mps->current_media_filename);
			if (mps->actual_media->parent_id) {
				mps->current_media = mps->actual_media->parent;
				mps->current_media_filename = bstrdup(mps->actual_media->filename);
				mps->current_folder_item_index = mps->actual_media->index;
			} else {
				mps->current_media = mps->actual_media;
				mps->current_media_filename = NULL;
				mps->current_folder_item_index = 0;
			}
			mps->current_media_index = mps->current_media->index;
			update_media_source(mps, true);
			obs_source_save(mps->source);
		}
		goto end;
	}

	if (mps->current_media->is_folder) {
		if (mps->current_folder_item_index > 0) {
			--mps->current_folder_item_index;
			play_folder_item_at_index(mps, mps->current_folder_item_index);
			goto end;
		} else {
			is_first_folder_item = true;
		}
	}

	if (!mps->current_media->is_folder || is_first_folder_item) {
		if (mps->current_media_index > 0) {
			--mps->current_media_index;
		} else if (mps->loop) {
			mps->current_media_index = mps->files.num - 1;
		} else {
			goto end;
		}
		play_media_at_index(mps, mps->current_media_index, is_first_folder_item);
	}

end:
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_activate(void *data)
{
	struct media_playlist_source *mps = data;
	if (!mps->files.num)
		return;

	mps->user_stopped = true;
	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_RESTART) {
		obs_source_media_restart(mps->source);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		obs_source_media_play_pause(mps->source, false);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT) {
		// we only play next when the source is deactivated so we don't do anything here
	}
}

static void mps_deactivate(void *data)
{
	struct media_playlist_source *mps = data;

	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_RESTART) {
		mps->user_stopped = true;
		obs_source_media_stop(mps->source);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		obs_source_media_play_pause(mps->source, true);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT) {
		mps->user_stopped = true;
		obs_source_media_stop(mps->source);
		obs_source_media_next(mps->source);
	}
}

static void mps_destroy(void *data)
{
	struct media_playlist_source *mps = data;

	obs_source_release(mps->current_media_source);
	shuffler_destroy(&mps->shuffler);
	free_files(&mps->files.da);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		deque_free(&mps->audio_data[i]);
	}
	deque_free(&mps->audio_frames);
	deque_free(&mps->audio_timestamps);
	pthread_mutex_destroy(&mps->mutex);
	pthread_mutex_destroy(&mps->audio_mutex);
	bfree(mps->current_media_filename);
	bfree(mps);
}

static void *mps_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct media_playlist_source *mps = bzalloc(sizeof(*mps));

	mps->first_update = true;
	mps->source = source;

	shuffler_init(&mps->shuffler);

	/* Internal media source */
	obs_data_t *media_source_data = obs_data_create();
	obs_data_set_bool(media_source_data, "log_changes", false);
	mps->current_media_source =
		obs_source_create_private("ffmpeg_source", "current_media_source", media_source_data);
	obs_source_add_active_child(mps->source, mps->current_media_source);
	obs_source_add_audio_capture_callback(mps->current_media_source, mps_audio_callback, mps);

	signal_handler_t *sh_media_source = obs_source_get_signal_handler(mps->current_media_source);
	signal_handler_connect(sh_media_source, "media_ended", media_source_ended, mps);

	mps->paused = false;

	mps->play_pause_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.PlayPause",
							    obs_module_text("PlayPause"), play_pause_hotkey, mps);

	mps->restart_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.Restart",
							 obs_module_text("Restart"), restart_hotkey, mps);

	mps->stop_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.Stop", obs_module_text("Stop"),
						      stop_hotkey, mps);

	mps->next_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.PlaylistNext",
						      obs_module_text("PlaylistNext"), next_hotkey, mps);

	mps->prev_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.PlaylistPrev",
						      obs_module_text("PlaylistPrev"), previous_hotkey, mps);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void select_index(int media_index, int folder_item_index)", select_index_proc, mps);

	pthread_mutex_init_value(&mps->mutex);
	if (pthread_mutex_init(&mps->mutex, NULL) != 0)
		goto error;

	pthread_mutex_init_value(&mps->audio_mutex);
	if (pthread_mutex_init(&mps->audio_mutex, NULL) != 0)
		goto error;

	obs_source_update(source, NULL);

	obs_data_release(media_source_data);
	return mps;

error:
	mps_destroy(mps);
	return NULL;
}

static void mps_video_render(void *data, gs_effect_t *effect)
{
	struct media_playlist_source *mps = data;
	obs_source_video_render(mps->current_media_source);

	UNUSED_PARAMETER(effect);
}

static bool mps_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			     size_t channels, size_t sample_rate)
{
	struct media_playlist_source *mps = data;
	if (!mps->current_media_source)
		return false;

	struct obs_source_audio_mix child_audio;
	uint64_t source_ts;

	/*if (obs_source_audio_pending(mps->current_media_source))
		return false;*/

	source_ts = obs_source_get_audio_timestamp(mps->current_media_source);
	if (!source_ts)
		return false;

	obs_source_get_audio_mix(mps->current_media_source, &child_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			float *in = child_audio.output[mix].data[ch];

			memcpy(out, in, AUDIO_OUTPUT_FRAMES * MAX_AUDIO_CHANNELS * sizeof(float));
		}
	}

	*ts_out = source_ts;

	UNUSED_PARAMETER(sample_rate);
	return true;
}

static void mps_video_tick(void *data, float seconds)
{
	struct media_playlist_source *mps = data;
	//UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);

	const audio_t *a = obs_get_audio();
	const struct audio_output_info *aoi = audio_output_get_info(a);
	pthread_mutex_lock(&mps->audio_mutex);
	while (mps->audio_frames.size > 0) {
		struct obs_source_audio audio;
		audio.format = aoi->format;
		audio.samples_per_sec = aoi->samples_per_sec;
		audio.speakers = aoi->speakers;
		deque_pop_front(&mps->audio_frames, &audio.frames, sizeof(audio.frames));
		deque_pop_front(&mps->audio_timestamps, &audio.timestamp, sizeof(audio.timestamp));
		for (size_t i = 0; i < mps->num_channels; i++) {
			audio.data[i] = (uint8_t *)mps->audio_data[i].data + mps->audio_data[i].start_pos;
		}
		obs_source_output_audio(mps->source, &audio);
		for (size_t i = 0; i < mps->num_channels; i++) {
			deque_pop_front(&mps->audio_data[i], NULL, audio.frames * sizeof(float));
		}
	}
	mps->num_channels = audio_output_get_channels(a);
	pthread_mutex_unlock(&mps->audio_mutex);

	//if (mps->restart_on_activate && mps->use_cut) {
	//	mps->elapsed = 0.0f;
	//	mps->cur_item = mps->randomize ? random_file(mps) : 0;
	//	do_transition(mps, false);
	//	mps->restart_on_activate = false;
	//	mps->use_cut = false;
	//	mps->stop = false;
	//	return;
	//}

	//if (mps->pause_on_deactivate || mps->manual || mps->stop || mps->paused)
	//	return;

	///* ----------------------------------------------------- */
	///* do transition when slide time reached                 */
	//mps->elapsed += seconds;

	//if (mps->elapsed > mps->slide_time) {
	//	mps->elapsed -= mps->slide_time;

	//	if (!mps->loop && mps->cur_item == mps->files.num - 1) {
	//		if (mps->hide)
	//			do_transition(mps, true);
	//		else
	//			do_transition(mps, false);

	//		return;
	//	}

	//	if (mps->randomize) {
	//		size_t next = mps->cur_item;
	//		if (mps->files.num > 1) {
	//			while (next == mps->cur_item)
	//				next = random_file(mps);
	//		}
	//		mps->cur_item = next;

	//	} else if (++mps->cur_item >= mps->files.num) {
	//		mps->cur_item = 0;
	//	}

	//	if (mps->files.num)
	//		do_transition(mps, false);
	//}
}

static void mps_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct media_playlist_source *mps = data;

	pthread_mutex_lock(&mps->mutex);
	cb(mps->source, mps->current_media_source, param);
	pthread_mutex_unlock(&mps->mutex);
}

static uint32_t mps_width(void *data)
{
	struct media_playlist_source *mps = data;
	return obs_source_get_width(mps->current_media_source);
}

static uint32_t mps_height(void *data)
{
	struct media_playlist_source *mps = data;
	return obs_source_get_height(mps->current_media_source);
}

static void mps_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_SHUFFLE, false);
	obs_data_set_default_int(settings, S_VISIBILITY_BEHAVIOR, VISIBILITY_BEHAVIOR_STOP_RESTART);
	obs_data_set_default_int(settings, S_RESTART_BEHAVIOR, RESTART_BEHAVIOR_CURRENT_FILE);
	obs_data_set_default_string(settings, S_CURRENT_FILE_NAME, " ");
	obs_data_set_default_int(settings, S_SPEED, 100);
}

static void add_media_to_selection(obs_property_t *list, struct media_file_data *data)
{
	struct dstr key = {0};
	struct dstr name = {0};

	if (data->parent) {
		dstr_catf(&key, "%zu-%zu", data->parent->index + 1, data->index + 1);
	} else if (data->folder_items.num) {
		for (size_t i = 0; i < data->folder_items.num; i++) {
			add_media_to_selection(list, &data->folder_items.array[i]);
		}
		return;
	} else {
		dstr_catf(&key, "%zu", data->index + 1);
	}
	dstr_copy_dstr(&name, &key);
	dstr_cat(&name, ": ");
	dstr_cat(&name, data->path);
	obs_property_list_add_string(list, name.array, key.array);
	dstr_free(&name);
	dstr_free(&key);
}

static void update_current_filename_property(struct media_playlist_source *mps, obs_property_t *p)
{
	struct dstr long_desc = {0};
	if (!mps || !p)
		return;
	if (!mps->actual_media) {
		obs_property_set_long_description(p, " ");
		return;
	} else if (mps->actual_media->parent) {
		dstr_catf(&long_desc, "%zu-%zu", mps->actual_media->parent->index + 1, mps->actual_media->index + 1);
	} else {
		dstr_catf(&long_desc, "%zu", mps->actual_media->index + 1);
	}
	dstr_catf(&long_desc, ": %s", mps->actual_media->path);
	obs_property_set_long_description(p, long_desc.array);
	dstr_free(&long_desc);
}

static obs_properties_t *mps_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	struct dstr filter = {0};
	struct dstr exts = {0};
	struct dstr path = {0};
	obs_property_t *p;

	obs_properties_add_bool(props, S_LOOP, T_LOOP);
	obs_properties_add_bool(props, S_SHUFFLE, T_SHUFFLE);

	// get last directory opened for editable list
	if (mps) {
		pthread_mutex_lock(&mps->mutex);
		if (mps->files.num) {
			struct media_file_data *last = da_end(mps->files);
			const char *slash;

			dstr_copy(&path, last->path);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&mps->mutex);
	}

	p = obs_properties_add_list(props, S_VISIBILITY_BEHAVIOR, T_VISIBILITY_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_STOP_RESTART, VISIBILITY_BEHAVIOR_STOP_RESTART);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT, VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE, VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_ALWAYS_PLAY, VISIBILITY_BEHAVIOR_ALWAYS_PLAY);

	p = obs_properties_add_list(props, S_RESTART_BEHAVIOR, T_RESTART_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_RESTART_BEHAVIOR_CURRENT_FILE, RESTART_BEHAVIOR_CURRENT_FILE);
	obs_property_list_add_int(p, T_RESTART_BEHAVIOR_FIRST_FILE, RESTART_BEHAVIOR_FIRST_FILE);

	obs_properties_add_bool(props, S_FFMPEG_HW_DECODE, T_USE_HARDWARE_DECODING);

	p = obs_properties_add_bool(props, S_FFMPEG_CLOSE_WHEN_INACTIVE, T_FFMPEG_CLOSE_WHEN_INACTIVE);
	obs_property_set_long_description(p, T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	p = obs_properties_add_editable_list(props, S_PLAYLIST, T_PLAYLIST, OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS,
					     filter.array, path.array);
	dstr_free(&path);
	dstr_free(&filter);
	dstr_free(&exts);

	p = obs_properties_add_text(props, S_CURRENT_FILE_NAME, T_CURRENT_FILE_NAME, OBS_TEXT_INFO);
	obs_property_set_long_description(p, "Due to OBS limitations, this will only update if any settings"
					     " are changed, the selected file is played, or the Properties "
					     "window is reopened. It will not update when the video ends.");
	obs_properties_add_button(props, S_REFRESH_FILENAME, T_REFRESH_FILENAME, refresh_filename_clicked);
	//update_current_filename_property(mps, p);

	p = obs_properties_add_list(props, S_SELECT_FILE, T_SELECT_FILE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_NO_FILE_SELECTED, "0");
	if (mps) {
		for (size_t i = 0; i < mps->files.num; i++) {
			add_media_to_selection(p, &mps->files.array[i]);
		}
	}

	obs_properties_add_button(props, "play_selected", "Play Selected File", play_selected_clicked);

	p = obs_properties_add_int_slider(props, S_SPEED, T_SPEED, 1, 200, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_text(props, "", T_SPEED_WARNING, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);

	obs_data_array_release(array);
	obs_data_release(settings);

	return props;
}

/* Sets the parent field of each media_file_data. This is necessary to be
 * called AFTER the darray is size is modified (because of array reallocation)
 */
static void set_parents(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;
	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *item = &files.array[i];
		for (size_t j = 0; j < item->folder_items.num; j++) {
			struct media_file_data *folder_item = &item->folder_items.array[j];
			folder_item->parent = item;
		}
	}
}

static void add_file(struct darray *array, const char *path, const char *id)
{
	DARRAY(struct media_file_data) new_files;
	new_files.da = *array;
	struct media_file_data *data = da_push_back_new(new_files);

	data->id = bstrdup(id);
	data->index = new_files.num - 1;
	data->path = bstrdup(path);
	data->is_url = strstr(path, "://") != NULL;
	da_init(data->folder_items);

	os_dir_t *dir = os_opendir(path);

	if (dir) {
		struct dstr dir_path = {0};
		struct os_dirent *ent;

		data->is_folder = true;

		while (true) {
			const char *ext;

			ent = os_readdir(dir);
			if (!ent)
				break;
			if (ent->directory)
				continue;

			ext = os_get_path_extension(ent->d_name);
			if (!valid_extension(ext))
				continue;

			struct media_file_data folder_item = {0};
			folder_item.filename = bstrdup(ent->d_name);
			folder_item.parent_id = data->id;
			folder_item.index = data->folder_items.num;
			dstr_copy(&dir_path, path);
			dstr_cat_ch(&dir_path, '/');
			dstr_cat(&dir_path, ent->d_name);
			folder_item.path = bstrdup(dir_path.array);

			da_push_back(data->folder_items, &folder_item);
		}

		dstr_free(&dir_path);
		os_closedir(dir);
	}

	*array = new_files.da;
}

static void mps_update(void *data, obs_data_t *settings)
{
	DARRAY(struct media_file_data) new_files;
	DARRAY(struct media_file_data) old_files;
	struct media_playlist_source *mps = data;
	obs_data_array_t *array;
	size_t count;
	bool shuffle = false;
	bool shuffle_changed = false;
	enum visibility_behavior visibility_behavior = mps->visibility_behavior;
	bool visibility_behavior_changed = false;
	bool item_edited = false;
	bool restart_on_activate = true;
	const char *old_media_path = NULL;
	long long new_speed;
	//const char *mode;

	/* ------------------------------------- */
	/* get settings data */

	da_init(new_files);

	mps->visibility_behavior = obs_data_get_int(settings, S_VISIBILITY_BEHAVIOR);
	if (mps->visibility_behavior != visibility_behavior) {
		visibility_behavior_changed = true;
	}
	mps->restart_behavior = obs_data_get_int(settings, S_RESTART_BEHAVIOR);
	shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	shuffle_changed = mps->shuffle != shuffle;
	mps->shuffle = shuffle;
	mps->loop = obs_data_get_bool(settings, S_LOOP);
	shuffler_set_loop(&mps->shuffler, mps->loop);
	new_speed = obs_data_get_int(settings, S_SPEED);
	if (mps->speed != new_speed) {
		mps->user_stopped = true;
	}
	mps->speed = new_speed;

	/* Internal media source settings */
	mps->use_hw_decoding = obs_data_get_bool(settings, S_FFMPEG_HW_DECODE);
	mps->close_when_inactive = obs_data_get_bool(settings, S_FFMPEG_CLOSE_WHEN_INACTIVE);
	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_ALWAYS_PLAY ||
	    mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		restart_on_activate = false;
	}
	obs_data_t *media_source_settings = obs_data_create();
	obs_data_set_bool(media_source_settings, S_FFMPEG_RESTART_ON_ACTIVATE, restart_on_activate);
	obs_data_set_bool(media_source_settings, S_FFMPEG_HW_DECODE, mps->use_hw_decoding);
	obs_data_set_bool(media_source_settings, S_FFMPEG_CLOSE_WHEN_INACTIVE, mps->close_when_inactive);
	obs_data_set_int(media_source_settings, S_SPEED, mps->speed);
	obs_source_update(mps->current_media_source, media_source_settings);
	obs_data_release(media_source_settings);
	mps->state = obs_source_media_get_state(mps->source);
	if (visibility_behavior_changed && !obs_source_active(mps->source) && (mps->state == OBS_MEDIA_STATE_PLAYING || mps->state == OBS_MEDIA_STATE_PAUSED)) {
		mps_deactivate(mps);
	}

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);

	if (!mps->first_update && mps->current_media) {
		old_media_path = mps->current_media->path;
	}
	if (mps->first_update) {
		mps->current_media_filename = bstrdup(obs_data_get_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME));
		mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	}

	bool found = false;
	pthread_mutex_lock(&mps->mutex);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		const char *id = obs_data_get_string(item, S_ID);

		if (!path || !*path) {
			obs_data_release(item);
			continue;
		}

		if (!mps->first_update && mps->current_media && strcmp(id, mps->current_media->id) == 0) {
			// check for current_media->id only if media isn't changed, allowing scripts to set the index.
			mps->current_media_index = i;
			found = true;
			if (old_media_path)
				item_edited = strcmp(old_media_path, path) != 0;
		}
		add_file(&new_files.da, path, id);
		obs_data_release(item);
	}
	set_parents(&new_files.da);

	if (mps->shuffle) {
		if (shuffle_changed) {
			shuffler_reshuffle(&mps->shuffler);
		}
		shuffler_update_files(&mps->shuffler, &new_files.da);
	} else if (shuffle_changed) {
		bfree(mps->current_media_filename);
		if (mps->actual_media && mps->actual_media->parent_id)
			mps->current_media_filename = bstrdup(mps->actual_media->filename);
		else
			mps->current_media_filename = NULL;
	}
	old_files.da = mps->files.da;
	mps->files.da = new_files.da;
	pthread_mutex_unlock(&mps->mutex);

	free_files(&old_files.da);

	if (found || mps->first_update) {
		set_current_media_index(mps, mps->current_media_index);
	} else {
		set_current_media_index(mps, 0);
	}

	if (mps->files.num) {
		if (item_edited) {
			mps->current_folder_item_index = 0;
		} else if (mps->current_media && mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;

			/* Find that file in the folder */
			if (mps->current_media_filename) {
				// some files may have been added/deleted so current file index is changed
				mps->current_folder_item_index = find_folder_item_index(
					&mps->current_media->folder_items.da, mps->current_media_filename);
				if (mps->current_folder_item_index == DARRAY_INVALID) {
					mps->current_folder_item_index = 0;
					found = false;
				}
			}

			if (mps->current_media->folder_items.num == 0) {
				mps_playlist_next(mps);
			} else {
				set_current_folder_item_index(mps, mps->current_folder_item_index);
				if (mps->shuffle)
					shuffler_select(&mps->shuffler, mps->actual_media);
			}
		} else {
			mps->actual_media = mps->current_media;
			if (mps->shuffle)
				shuffler_select(&mps->shuffler, mps->actual_media);
		}

		if (mps->first_update || !found || item_edited) {
			/* Clear if last file is a folder and is empty */
			if (mps->current_media->is_folder && mps->current_media->folder_items.num == 0) {
				clear_media_source(mps);
			} else {
				update_media_source(mps, true);
			}
		}
	} else if (!mps->first_update) {
		bfree(mps->current_media_filename);
		mps->current_media_filename = NULL;
		clear_media_source(mps);
		mps->actual_media = NULL;
	}
	obs_source_save(mps->source);

	/* So Current File Name is updated */
	update_current_filename_setting(mps, settings);
	/* ------------------------- */

	//if (mps->files.num) {
	//	do_transition(mps, false);

	//	if (mps->manual)
	//		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	//	else
	//		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);

	//	obs_source_media_started(mps->source);
	//}
	obs_data_array_release(array);
	mps->first_update = false;
}

static void mps_save(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	obs_data_set_int(settings, S_CURRENT_MEDIA_INDEX, mps->current_media_index);
	obs_data_set_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME, mps->current_media_filename);
	update_current_filename_setting(mps, settings);
}

static void mps_load(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	if (mps->files.num)
		obs_log(LOG_DEBUG, "%s", mps->files.array[mps->current_media_index].id);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct media_playlist_source *mps = src;
	const char *orig_path = data;

	obs_source_t *source = mps->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *file = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(file, "value");

		if (strcmp(path, orig_path) == 0) {
			if (new_path && *new_path)
				obs_data_set_string(file, "value", new_path);
			else
				obs_data_array_erase(files, i);

			obs_data_release(file);
			break;
		}

		obs_data_release(file);
	}

	obs_source_update(source, settings);

	obs_data_array_release(files);
	obs_data_release(settings);
}

static obs_missing_files_t *mps_missingfiles(void *data)
{
	struct media_playlist_source *mps = data;
	obs_missing_files_t *missing_files = obs_missing_files_create();

	obs_source_t *source = mps->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (strcmp(path, "") != 0) {
			if (!os_file_exists(path) && strstr(path, "://") == NULL) {
				obs_missing_file_t *file = obs_missing_file_create(
					path, missing_file_callback, OBS_MISSING_FILE_SOURCE, source, (void *)path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}
