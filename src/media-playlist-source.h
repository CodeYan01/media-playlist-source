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

#pragma once

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/circlebuf.h>
#include <plugin-support.h>
#include "playlist.h"
#include "shuffler.h"

/* clang-format off */

enum visibility_behavior {
	VISIBILITY_BEHAVIOR_STOP_RESTART,
	VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE,
	VISIBILITY_BEHAVIOR_ALWAYS_PLAY,
};

enum restart_behavior {
	RESTART_BEHAVIOR_CURRENT_FILE,
	RESTART_BEHAVIOR_FIRST_FILE,
};

/* clang-format on */

struct media_playlist_source {
	obs_source_t *source;
	obs_source_t *current_media_source;

	struct shuffler shuffler;
	bool shuffle;
	bool loop;
	bool paused;
	bool user_stopped;
	bool close_when_inactive;
	pthread_mutex_t mutex;
	DARRAY(struct media_file_data) files;
	struct media_file_data *current_media; // only for file/folder in the list
	struct media_file_data *actual_media; // for both files and folder items
	size_t current_media_index;
	char *current_media_filename; // only used with folder_items
	// to know if current_folder_item_index will be used, check if current file is a folder
	size_t current_folder_item_index;
	size_t last_id_count;

	obs_hotkey_id play_pause_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id prev_hotkey;

	enum obs_media_state state;
	enum visibility_behavior visibility_behavior;
	enum restart_behavior restart_behavior;

	struct circlebuf audio_data[MAX_AUDIO_CHANNELS];
	struct circlebuf audio_frames;
	struct circlebuf audio_timestamps;
	size_t num_channels;
	pthread_mutex_t audio_mutex;
};

static const char *media_filter =
	" (*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm *.mp3 *.m4a *.ogg *.aac *.wav *.opus *.flac);;";
static const char *video_filter =
	" (*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter =
	" (*.mp3 *.m4a *.mka *.aac *.ogg *.wav *.opus *.flac);;";

static void set_current_media_index(struct media_playlist_source *mps,
				    size_t index);

static inline void reset_folder_item_index(struct media_playlist_source *mps);

static bool valid_extension(const char *ext);

static void clear_media_source(void *data);
static void update_media_source(void *data, bool forced);

static void select_index_proc_(struct media_playlist_source *mps,
			       size_t media_index, size_t folder_item_index);

static void select_index_proc(void *data, calldata_t *cd);
static void play_folder_item_at_index(void *data, size_t index);
static void play_media_at_index(void *data, size_t index,
				bool play_last_folder_item);

static size_t find_folder_item_index(struct darray *array,
				     const char *filename);

static void set_media_state(void *data, enum obs_media_state state);
static enum obs_media_state mps_get_state(void *data);

static void mps_end_reached(void *data);

static void media_source_ended(void *data, calldata_t *cd);
void mps_audio_callback(void *data, obs_source_t *source,
			const struct audio_data *audio_data, bool muted);
static bool play_selected_clicked(obs_properties_t *props,
				  obs_property_t *property, void *data);

static void play_pause_hotkey(void *data, obs_hotkey_id id,
			      obs_hotkey_t *hotkey, bool pressed);
static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			   bool pressed);
static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed);
static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed);
static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			    bool pressed);

static const char *mps_getname(void *unused);
static int64_t mps_get_duration(void *data);
static int64_t mps_get_time(void *data);
static void mps_set_time(void *data, int64_t ms);
static void mps_play_pause(void *data, bool pause);
static void mps_restart(void *data);
static void mps_stop(void *data);
static void mps_playlist_next(void *data);
static void mps_playlist_prev(void *data);
static void mps_activate(void *data);
static void mps_deactivate(void *data);
static void *mps_create(obs_data_t *settings, obs_source_t *source);
static void mps_destroy(void *data);
static void mps_video_render(void *data, gs_effect_t *effect);
static bool mps_audio_render(void *data, uint64_t *ts_out,
			     struct obs_source_audio_mix *audio_output,
			     uint32_t mixers, size_t channels,
			     size_t sample_rate);
static void mps_video_tick(void *data, float seconds);
static void mps_enum_sources(void *data, obs_source_enum_proc_t cb,
			     void *param);
static uint32_t mps_width(void *data);
static uint32_t mps_height(void *data);
static void mps_defaults(obs_data_t *settings);
static void update_current_filename_property(struct media_playlist_source *mps,
					     obs_property_t *p);
static obs_properties_t *mps_properties(void *data);
static void mps_update(void *data, obs_data_t *settings);
static void mps_save(void *data, obs_data_t *settings);
static void mps_load(void *data, obs_data_t *settings);
static void missing_file_callback(void *src, const char *new_path, void *data);
static obs_missing_files_t *mps_missingfiles(void *data);

static void set_parents(struct darray *array);
static void add_file(struct darray *array, const char *path, size_t id);
static void free_files(struct darray *array);

struct obs_source_info media_playlist_source_info = {
	.id = "media_playlist_source_codeyan",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = mps_getname,
	.create = mps_create,
	.destroy = mps_destroy,
	.update = mps_update,
	.save = mps_save,
	//.load = mps_load,
	.activate = mps_activate,
	.deactivate = mps_deactivate,
	.video_render = mps_video_render,
	.video_tick = mps_video_tick,
	//.audio_render = mps_audio_render,
	.enum_active_sources = mps_enum_sources,
	.get_width = mps_width,
	.get_height = mps_height,
	.get_defaults = mps_defaults,
	.get_properties = mps_properties,
	.missing_files = mps_missingfiles,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = mps_play_pause,
	.media_restart = mps_restart,
	.media_stop = mps_stop,
	.media_next = mps_playlist_next,
	.media_previous = mps_playlist_prev,
	.media_get_state = mps_get_state,
	.media_get_duration = mps_get_duration,
	.media_get_time = mps_get_time,
	.media_set_time = mps_set_time,
	//.video_get_color_space = mps_video_get_color_space,
};
