#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/circlebuf.h>

/* clang-format off */

#define S_PLAYLIST                     "playlist"
#define S_LOOP                         "loop"
#define S_SHUFFLE                      "shuffle"
#define S_BEHAVIOR                     "playback_behavior"
#define S_BEHAVIOR_STOP_RESTART        "stop_restart"
#define S_BEHAVIOR_PAUSE_UNPAUSE       "pause_unpause"
#define S_BEHAVIOR_ALWAYS_PLAY         "always_play"
#define S_NETWORK_CACHING              "network_caching"
#define S_AUDIO_TRACK                  "audio_track"
#define S_SUBTITLE_ENABLE              "subtitle_enable"
#define S_SUBTITLE_TRACK               "subtitle"
#define S_CURRENT_FILE_NAME            "current_file_name"

#define S_CURRENT_MEDIA_INDEX          "current_media_index"

#define T_(text) obs_module_text(text)
#define T_PLAYLIST                     T_("Playlist")
#define T_LOOP                         T_("LoopPlaylist")
#define T_SHUFFLE                      T_("Shuffle")
#define T_BEHAVIOR                     T_("PlaybackBehavior")
#define T_BEHAVIOR_STOP_RESTART        T_("PlaybackBehavior.StopRestart")
#define T_BEHAVIOR_PAUSE_UNPAUSE       T_("PlaybackBehavior.PauseUnpause")
#define T_BEHAVIOR_ALWAYS_PLAY         T_("PlaybackBehavior.AlwaysPlay")
#define T_NETWORK_CACHING              T_("NetworkCaching")
#define T_AUDIO_TRACK                  T_("AudioTrack")
#define T_SUBTITLE_ENABLE              T_("SubtitleEnable")
#define T_SUBTITLE_TRACK               T_("SubtitleTrack")
#define T_CURRENT_FILE_NAME            T_("CurrentFileName")

#define T_PLAY_PAUSE                   T_("PlayPause")
#define T_RESTART                      T_("Restart")
#define T_STOP                         T_("Stop")
#define T_PLAYLIST_NEXT                T_("Next")
#define T_PLAYLIST_PREV                T_("Previous")

/* clang-format on */

/* ------------------------------------------------------------------------- */

struct media_file_data {
	char *path;
	size_t id;
};

enum behavior {
	BEHAVIOR_STOP_RESTART,
	BEHAVIOR_PAUSE_UNPAUSE,
	BEHAVIOR_ALWAYS_PLAY,
};

struct media_playlist_source {
	obs_source_t *source;
	obs_source_t *current_media_source;

	bool shuffle;
	bool loop;
	bool paused;
	bool manual;
	pthread_mutex_t mutex;
	DARRAY(struct media_file_data) files;
	size_t current_media_id;
	size_t current_media_index;
	char *current_media_path;
	size_t last_id_count;

	obs_hotkey_id play_pause_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id prev_hotkey;

	enum obs_media_state state;
	enum behavior behavior;

	struct circlebuf audio_data[MAX_AUDIO_CHANNELS];
	struct circlebuf audio_frames;
	struct circlebuf audio_timestamps;
	size_t num_channels;
	pthread_mutex_t audio_mutex;
};

static const char *media_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.aac *.ogg *.wav);;";

/* ------------------------------------------------------------------------- */

static inline void set_current_media_index(struct media_playlist_source *mps,
					   size_t current_media_index)
{
	mps->current_media_index = current_media_index;
	if (mps->files.num) {
		mps->current_media_id =
			mps->files.array[current_media_index].id;
		mps->current_media_path =
			mps->files.array[mps->current_media_index].path;
	}
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
	enum obs_media_state media_state =
		obs_source_media_get_state(mps->current_media_source);
	UNUSED_PARAMETER(mps);
	return media_state;
}

static void mps_end_reached(void *data)
{
	struct media_playlist_source *mps = data;
	set_media_state(mps, OBS_MEDIA_STATE_ENDED);
	obs_source_media_ended(mps->source);
}

static void media_source_ended(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct media_playlist_source *mps = data;

	if (mps->current_media_index < mps->files.num - 1 || mps->loop)
		obs_source_media_next(mps->source);
	else
		mps_end_reached(mps);
}

void mps_audio_callback(void *data, obs_source_t *source,
			const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);
	UNUSED_PARAMETER(source);
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->audio_mutex);
	size_t size = audio_data->frames * sizeof(float);
	for (size_t i = 0; i < mps->num_channels; i++) {
		circlebuf_push_back(&mps->audio_data[i], audio_data->data[i],
				    size);
	}
	circlebuf_push_back(&mps->audio_frames, &audio_data->frames,
			    sizeof(audio_data->frames));
	circlebuf_push_back(&mps->audio_timestamps, &audio_data->timestamp,
			    sizeof(audio_data->timestamp));
	pthread_mutex_unlock(&mps->audio_mutex);
}

static bool play_selected_clicked(obs_properties_t *props,
				  obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	size_t count = obs_data_array_count(array);
	blog(LOG_DEBUG, obs_data_get_json(settings));

	for (int i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		bool selected = obs_data_get_bool(item, "selected");
		obs_data_release(item);
		if (selected) {
			mps->current_media_index = i;
			break;
		}
	}

	obs_data_array_release(array);
	obs_data_release(settings);
	obs_source_update_properties(mps->source);

	return false;
}

static bool playlist_modified(void *data, obs_properties_t *props,
			      obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	
	return false;
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
		bfree(files.array[i].path);
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

static void play_pause_hotkey(void *data, obs_hotkey_id id,
			      obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_play_pause(mps->source, !mps->paused);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			   bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_restart(mps->source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_stop(mps->source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (!mps->manual)
		return;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_next(mps->source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			    bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (!mps->manual)
		return;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_previous(mps->source);
}

static void mps_play_pause(void *data, bool pause)
{
	struct media_playlist_source *mps = data;

	obs_source_media_play_pause(mps->current_media_source, pause);

	if (pause)
		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	else
		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
}

static void mps_restart(void *data)
{
	struct media_playlist_source *mps = data;

	obs_source_media_restart(mps->current_media_source);
	set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
}

static void mps_stop(void *data)
{
	struct media_playlist_source *mps = data;

	obs_source_media_stop(mps->current_media_source);
	obs_source_output_video(mps->source, NULL);
	set_media_state(mps, OBS_MEDIA_STATE_STOPPED);
}

static void mps_playlist_next(void *data)
{
	struct media_playlist_source *mps = data;

	if (mps->current_media_index < mps->files.num - 1) {
		set_current_media_index(mps, mps->current_media_index + 1);
	} else if (mps->loop) {
		set_current_media_index(mps, 0);
	} else {
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "local_file",
			    mps->files.array[mps->current_media_index].path);
	obs_source_update(mps->current_media_source, settings);
	obs_source_save(mps->source);
	obs_source_update_properties(mps->source);
	obs_data_release(settings);
}

static void mps_playlist_prev(void *data)
{
	struct media_playlist_source *mps = data;

	if (mps->current_media_index > 0) {
		set_current_media_index(mps, mps->current_media_index - 1);
	} else if (mps->loop) {
		set_current_media_index(mps, mps->files.num - 1);
	} else {
		set_media_state(mps, OBS_MEDIA_STATE_ENDED);
		obs_source_media_ended(mps->source);
		obs_source_save(mps->source);
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "local_file",
			    mps->files.array[mps->current_media_index].path);
	obs_source_update(mps->current_media_source, settings);
	obs_source_save(mps->source);
	obs_source_update_properties(mps->source);
	obs_data_release(settings);
}

static void mps_activate(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_media_play_pause(mps->current_media_source, false);
	set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(mps->source);
	/*if (mps->behavior == BEHAVIOR_STOP_RESTART) {
		mps->restart_on_activate = true;
		mps->use_cut = true;
	} else if (mps->behavior == BEHAVIOR_PAUSE_UNPAUSE) {
		mps->pause_on_deactivate = false;
	}*/
}

static void mps_deactivate(void *data)
{
	struct media_playlist_source *mps = data;

	obs_source_media_play_pause(mps->current_media_source, true);
	set_media_state(mps, OBS_MEDIA_STATE_STOPPED);
	obs_source_media_ended(mps->source);
	/*if (mps->behavior == BEHAVIOR_PAUSE_UNPAUSE)
		mps->pause_on_deactivate = true;*/
}

static void mps_destroy(void *data)
{
	struct media_playlist_source *mps = data;

	obs_source_release(mps->current_media_source);
	free_files(&mps->files.da);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		circlebuf_free(&mps->audio_data[i]);
	}
	circlebuf_free(&mps->audio_frames);
	circlebuf_free(&mps->audio_timestamps);
	pthread_mutex_destroy(&mps->mutex);
	pthread_mutex_destroy(&mps->audio_mutex);
	bfree(mps);
}

static void *mps_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct media_playlist_source *mps = bzalloc(sizeof(*mps));
	mps->source = source;

	/* Internal media source */
	obs_data_t *media_source_data = obs_data_create();
	mps->current_media_source = obs_source_create_private(
		"ffmpeg_source", "current_media_source", media_source_data);
	obs_source_add_active_child(mps->source, mps->current_media_source);
	obs_source_add_audio_capture_callback(mps->current_media_source,
					      mps_audio_callback, mps);

	signal_handler_t *sh_media_source =
		obs_source_get_signal_handler(mps->current_media_source);
	signal_handler_connect(sh_media_source, "media_ended",
			       media_source_ended, mps);

	mps->last_id_count = 0;
	mps->manual = false;
	mps->paused = false;

	mps->play_pause_hotkey = obs_hotkey_register_source(
		source, "MediaPlaylistSource.PlayPause",
		obs_module_text("MediaPlaylistSource.PlayPause"),
		play_pause_hotkey, mps);

	mps->restart_hotkey = obs_hotkey_register_source(
		source, "MediaPlaylistSource.Restart",
		obs_module_text("MediaPlaylistSource.Restart"), restart_hotkey,
		mps);

	mps->stop_hotkey = obs_hotkey_register_source(
		source, "MediaPlaylistSource.Stop",
		obs_module_text("MediaPlaylistSource.Stop"), stop_hotkey, mps);

	mps->next_hotkey = obs_hotkey_register_source(
		source, "MediaPlaylistSource.NextItem",
		obs_module_text("MediaPlaylistSource.NextItem"), next_hotkey,
		mps);

	mps->prev_hotkey = obs_hotkey_register_source(
		source, "MediaPlaylistSource.PreviousItem",
		obs_module_text("MediaPlaylistSource.PreviousItem"),
		previous_hotkey, mps);

	// proc_handler_t *ph = obs_source_get_proc_handler(source);
	// proc_handler_add(ph, "int current_index()", current_slide_proc, mps);
	// proc_handler_add(ph, "int total_files()", total_slides_proc, mps);

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

static bool mps_audio_render(void *data, uint64_t *ts_out,
			     struct obs_source_audio_mix *audio_output,
			     uint32_t mixers, size_t channels,
			     size_t sample_rate)
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

			memcpy(out, in,
			       AUDIO_OUTPUT_FRAMES * MAX_AUDIO_CHANNELS *
				       sizeof(float));
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
		circlebuf_pop_front(&mps->audio_frames, &audio.frames,
				    sizeof(audio.frames));
		circlebuf_pop_front(&mps->audio_timestamps, &audio.timestamp,
				    sizeof(audio.timestamp));
		for (size_t i = 0; i < mps->num_channels; i++) {
			audio.data[i] = (uint8_t *)mps->audio_data[i].data +
					mps->audio_data[i].start_pos;
		}
		obs_source_output_audio(mps->source, &audio);
		for (size_t i = 0; i < mps->num_channels; i++) {
			circlebuf_pop_front(&mps->audio_data[i], NULL,
					    audio.frames * sizeof(float));
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

static void mps_enum_all_sources(void *data, obs_source_enum_proc_t cb,
				 void *param)
{
	struct media_playlist_source *mps = data;

	cb(mps->source, mps->current_media_source, param);
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
	obs_data_set_default_string(settings, S_BEHAVIOR,
				    S_BEHAVIOR_STOP_RESTART);
	obs_data_set_default_int(settings, S_NETWORK_CACHING, 400);
	obs_data_set_default_int(settings, S_AUDIO_TRACK, 1);
	obs_data_set_default_bool(settings, S_SUBTITLE_ENABLE, false);
	obs_data_set_default_int(settings, S_SUBTITLE_TRACK, 1);
}

static obs_properties_t *mps_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct media_playlist_source *mps = data;
	struct dstr filter = {0};
	struct dstr exts = {0};
	struct dstr path = {0};
	obs_property_t *p;

	/* Need deferred update; 
	 * otherwise mps_update would be called without the `id` field we inserted
	 * in each item whenever the list is modified.
	 */
	//obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
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

	p = obs_properties_add_list(props, S_BEHAVIOR, T_BEHAVIOR,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_BEHAVIOR_STOP_RESTART,
				     S_BEHAVIOR_STOP_RESTART);
	obs_property_list_add_string(p, T_BEHAVIOR_PAUSE_UNPAUSE,
				     S_BEHAVIOR_PAUSE_UNPAUSE);
	obs_property_list_add_string(p, T_BEHAVIOR_ALWAYS_PLAY,
				     S_BEHAVIOR_ALWAYS_PLAY);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	p = obs_properties_add_editable_list(
		props, S_PLAYLIST, T_PLAYLIST,
		OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS, filter.array,
		path.array);
	dstr_free(&path);
	dstr_free(&filter);
	dstr_free(&exts);

	obs_property_set_modified_callback2(p, playlist_modified, mps);

	p = obs_properties_add_text(props, S_CURRENT_FILE_NAME,
				    T_CURRENT_FILE_NAME, OBS_TEXT_INFO);
	obs_property_set_long_description(p, (mps) ? mps->current_media_path
						   : " ");
	obs_properties_add_button(props, "play_selected", "Play First Selected",
				  play_selected_clicked);

	p = obs_properties_add_int(props, S_NETWORK_CACHING, T_NETWORK_CACHING,
				   100, 60000, 10);
	obs_property_int_set_suffix(p, " ms");

	obs_properties_add_int(props, S_AUDIO_TRACK, T_AUDIO_TRACK, 1, 10, 1);
	obs_properties_add_bool(props, S_SUBTITLE_ENABLE, T_SUBTITLE_ENABLE);
	obs_properties_add_int(props, S_SUBTITLE_TRACK, T_SUBTITLE_TRACK, 1, 10,
			       1);

	return props;
}

static void add_file(struct darray *array, const char *path, size_t id)
{
	DARRAY(struct media_file_data) new_files;
	struct media_file_data data;

	new_files.da = *array;

	data.id = id;
	data.path = bstrdup(path);
	da_push_back(new_files, &data);
	*array = new_files.da;
}

static void mps_update(void *data, obs_data_t *settings)
{
	DARRAY(struct media_file_data) new_files;
	DARRAY(struct media_file_data) old_files;
	struct media_playlist_source *mps = data;
	obs_data_t *media_source_settings =
		obs_source_get_settings(mps->current_media_source);
	obs_data_array_t *array;
	size_t count;
	const char *behavior;
	bool is_first_update = false;
	size_t new_media_index = 0;
	bool media_index_changed = false;
	//const char *mode;

	/* ------------------------------------- */
	/* get settings data */

	da_init(new_files);

	behavior = obs_data_get_string(settings, S_BEHAVIOR);

	if (astrcmpi(behavior, S_BEHAVIOR_PAUSE_UNPAUSE) == 0)
		mps->behavior = BEHAVIOR_PAUSE_UNPAUSE;
	else if (astrcmpi(behavior, S_BEHAVIOR_ALWAYS_PLAY) == 0)
		mps->behavior = BEHAVIOR_ALWAYS_PLAY;
	else /* S_BEHAVIOR_STOP_RESTART */
		mps->behavior = BEHAVIOR_STOP_RESTART;

	mps->shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	mps->loop = obs_data_get_bool(settings, S_LOOP);

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);

	if (!mps->last_id_count) {
		is_first_update = false;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(array, i);
			size_t id = obs_data_get_int(item, "id");

			if (id > mps->last_id_count)
				mps->last_id_count = id;

			obs_data_release(item);
		}
	}

	new_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	media_index_changed = mps->current_media_index != new_media_index;
	mps->current_media_index = new_media_index;

	bool found = false;
	pthread_mutex_lock(&mps->mutex);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		size_t id = obs_data_get_int(item, "id");

		if (id == 0) {
			obs_data_set_int(item, "id", ++mps->last_id_count);
		} else if (!media_index_changed &&
			   id == mps->current_media_id) {
			// check for current_media_id only if media isn't changed, allowing scripts to set the index.
			mps->current_media_index = i;
			found = true;
		}
		add_file(&new_files.da, path, id);
		obs_data_release(item);
	}
	old_files.da = mps->files.da;
	mps->files.da = new_files.da;
	pthread_mutex_unlock(&mps->mutex);

	free_files(&old_files.da);

	if (media_index_changed && mps->current_media_index < mps->files.num)
		found = true;

	if (found) {
		set_current_media_index(mps, mps->current_media_index);
	} else {
		set_current_media_index(mps, 0);
	}

	if (mps->files.num) {
		const char *path = obs_data_get_string(media_source_settings,
						       "local_file");
		if (strcmp(path, mps->current_media_path) != 0) {
			obs_data_set_string(media_source_settings, "local_file",
					    mps->current_media_path);
			obs_source_update(mps->current_media_source,
					  media_source_settings);
			obs_source_media_started(mps->source);
		}
	}
	/* ------------------------------------- */
	/* create new list of sources */
	/*
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		os_dir_t *dir = os_opendir(path);

		if (!path || !*path) {
			obs_data_release(item);
			continue;
		}

		if (dir) {
			struct dstr dir_path = {0};
			struct os_dirent *ent;

			for (;;) {
				const char *ext;

				ent = os_readdir(dir);
				if (!ent)
					break;
				if (ent->directory)
					continue;

				ext = os_get_path_extension(ent->d_name);
				if (!valid_extension(ext))
					continue;

				dstr_copy(&dir_path, path);
				dstr_cat_ch(&dir_path, '/');
				dstr_cat(&dir_path, ent->d_name);
				add_file(mps, &new_files.da, dir_path.array, &cx,
					 &cy);

				if (mps->mem_usage >= MAX_MEM_USAGE)
					break;
			}

			dstr_free(&dir_path);
			os_closedir(dir);
		} else {
			add_file(mps, &new_files.da, path, &cx, &cy);
		}

		obs_data_release(item);

		if (mps->mem_usage >= MAX_MEM_USAGE)
			break;
	}
	*/

	/* ------------------------------------- */
	/* update settings data */

	/* ------------------------------------- */
	/* clean up and restart transition */

	//free_files(&old_files.da);

	/* ------------------------- */

	//if (mps->files.num) {
	//	do_transition(mps, false);

	//	if (mps->manual)
	//		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	//	else
	//		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);

	//	obs_source_media_started(mps->source);
	//}
	obs_data_release(media_source_settings);
	obs_data_array_release(array);
}

static void mps_save(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	obs_data_set_int(settings, S_CURRENT_MEDIA_INDEX,
			 mps->current_media_index);
}

static void mps_load(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	mps->current_media_index =
		obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	if (mps->files.num)
		blog(LOG_DEBUG, "%i",
		     mps->files.array[mps->current_media_index].id);
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
			obs_data_set_string(file, "value", new_path);

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
			if (!os_file_exists(path) &&
			    strstr(path, "://") == NULL) {
				obs_missing_file_t *file =
					obs_missing_file_create(
						path, missing_file_callback,
						OBS_MISSING_FILE_SOURCE, source,
						(void *)path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}

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
	.enum_all_sources = mps_enum_all_sources,
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
