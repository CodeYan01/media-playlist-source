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

#include "vosk-filter.h"

/* Forward declarations */

static void vf_frontend_event_cb(enum obs_frontend_event event, void *data);

/************************/

static inline const char *dstr_find_offset_r(const struct dstr *str,
					     const char *find, size_t offset)
{
	size_t len = strlen(find);
	while (offset >= 0) {
		if (strncmp(str->array + offset, find, len) == 0)
			return str->array + offset;
		if (offset == 0)
			break;
		offset--;
	}
	return NULL;
}

static void vf_frontend_event_cb(enum obs_frontend_event event, void *data)
{
	struct vosk_filter *vf = data;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		obs_source_update(vf->source, NULL);
		obs_frontend_remove_event_callback(vf_frontend_event_cb, vf);
	}
}

static const char *vf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VoskFilter");
}

/* Returns true if a word separator was found within the maximum line_length.
 * Returns false otherwise, and a dash (-) should be added at the end.
 * The cutoff_point is the index AFTER the dash or BEFORE the space.
 */
static bool get_line_cutoff(const struct dstr *str, size_t line_length,
			    size_t *cutoff_point)
{
	const char *last_space = NULL, *last_dash = NULL;
	bool last_sep_found = true;
	if (str->len <= line_length) {
		last_space = &str->array[str->len];
	} else if (str->array[line_length] == ' ') {
		last_space = &str->array[line_length];
	} else if (str->array[line_length - 1] == '-') {
		last_dash = &str->array[line_length - 1];
	} else {
		last_space = dstr_find_offset_r(str, " ", line_length - 1);
		last_dash = dstr_find_offset_r(str, "-", line_length - 1);
	}

	if (!last_space && !last_dash) {
		/* Force string split, but requires that '-' is added at the end. */
		*cutoff_point = line_length - 1;
		last_sep_found = false;
	} else if (!last_dash || last_space > last_dash) {
		*cutoff_point = last_space - str->array;
	} else if (!last_space || last_dash > last_space) {
		*cutoff_point = last_dash - str->array + 1;
	}
	return last_sep_found;
}

/* Splits `str` into lines and pushes them at the back of `array`.
 * Returns true if the last line is finalized. Returns false otherwise,
 * meaning the last line won't have a line break and is incomplete.
 * Will reduce length of `str` to 0.
 */
static bool split_into_lines(struct dstr *str, dstr_array_t *array,
			     size_t line_length)
{
	size_t cutoff_point;
	while (str->len > 0) {
		struct dstr *new_line = da_push_back_new((*array));
		if (str->len <= line_length - 2) {
			/* An incomplete line would need a space and a letter
			 * in order to be able to add words to it, so 
			 * if there's no space for that, consider it final
			 */
			dstr_copy_dstr(new_line, str);
			dstr_free(str);
			return false;
		} else if (get_line_cutoff(str, line_length, &cutoff_point)) {
			dstr_ncopy(new_line, str->array, cutoff_point);
			if (str->array[cutoff_point] == ' ') {
				/* so the space gets deleted */
				cutoff_point++;
			}
		} else {
			dstr_ncopy(new_line, str->array, cutoff_point);
			dstr_cat_ch(new_line, '-');
		}
		dstr_cat_ch(new_line, '\n');
		assert(str->len >= cutoff_point);
		dstr_remove(str, 0, cutoff_point);
	}

	return true;
}

/* Rebuild to account for new line length.
 * Returns true if the last line is finalized. Returns false otherwise,
 * meaning the last line won't have a line break and is incomplete.
 */
static bool rebuild_finalized_lines(dstr_array_t *array, size_t line_length)
{
	bool result;
	struct dstr flattened = {0};
	for (size_t i = 0; i < array->num; i++) {
		dstr_ncopy_dstr(&flattened, &array->array[i],
				array->array[i].len - 1);
		dstr_free(&array->array[i]);
	}
	da_free((*array));
	result = split_into_lines(&flattened, array, line_length);
	dstr_free(&flattened);
	return result;
};

static void update_text_source(struct vosk_filter *vf)
{
	pthread_mutex_lock(&vf->settings_mutex);
	obs_source_t *source = obs_weak_source_get_source(vf->text_source);
	struct dstr new_text = {0};
	size_t new_lines = 0;
	obs_data_t *settings = obs_data_create();
	obs_data_t *partial = NULL;
	if (vf->partial_result)
		partial = obs_data_create_from_json(vf->partial_result);
	const char *partial_str = obs_data_get_string(partial, "partial");
	dstr_array_t temp_lines = {0};
	size_t finalized_lines_offset;
	bool last_line_finalized = false;

	/* Rebuild finalized lines if necessary */
	if (vf->prev_line_length != vf->line_length &&
	    vf->finalized_lines.num) {
		last_line_finalized = rebuild_finalized_lines(
			&vf->finalized_lines, vf->line_length);
		if (!last_line_finalized) {
			struct dstr *last = da_end(vf->finalized_lines);
			if (last->len && vf->finalized_str.len)
				dstr_cat_ch(last, ' ');
			dstr_insert_dstr(&vf->finalized_str, 0, last);
			dstr_free(last);
			da_pop_back(vf->finalized_lines);
		}
	}
	vf->prev_line_length = vf->line_length;

	/* Add to vf->finalized_lines */
	last_line_finalized = split_into_lines(
		&vf->finalized_str, &vf->finalized_lines, vf->line_length);
	if (!last_line_finalized) {
		struct dstr *last = da_end(vf->finalized_lines);
		dstr_insert_dstr(&new_text, 0, last);

		/* Save it back so it is still there on next iteration */
		dstr_cat_dstr(&vf->finalized_str, last);

		dstr_free(last);
		da_pop_back(vf->finalized_lines);
	}

	/* Build temporary lines */
	if (new_text.len && *partial_str)
		dstr_cat_ch(&new_text, ' ');
	dstr_cat(&new_text, partial_str);
	split_into_lines(&new_text, &temp_lines, vf->line_length);
	assert(new_text.len == 0);

	/* Delete unused finalized lines. 
	 * We have to compare because size_t has no negative range.
	 */
	if (vf->line_count > temp_lines.num) {
		size_t remaining = vf->line_count - temp_lines.num;
		if (vf->finalized_lines.num > remaining)
			finalized_lines_offset =
				vf->finalized_lines.num - remaining;
		else
			finalized_lines_offset = 0;
	} else {
		finalized_lines_offset = vf->finalized_lines.num;
	}
	for (size_t i = 0; i < finalized_lines_offset; i++) {
		dstr_free(&vf->finalized_lines.array[i]);
	}
	if (finalized_lines_offset > 0)
		da_erase_range(vf->finalized_lines, 0, finalized_lines_offset);

	/* Delete unused temp lines */
	if (vf->line_count < temp_lines.num) {
		size_t num = temp_lines.num - vf->line_count;
		for (size_t i = 0; i < num; i++) {
			dstr_free(&temp_lines.array[i]);
		}
		da_erase_range(temp_lines, 0, num);
	}

	/* Build text */
	for (size_t i = 0; i < vf->finalized_lines.num; i++) {
		dstr_cat_dstr(&new_text, &vf->finalized_lines.array[i]);
		new_lines++;
	}
	for (size_t i = 0; i < temp_lines.num; i++) {
		dstr_cat_dstr(&new_text, &temp_lines.array[i]);
		new_lines++;
	}

	/* Remove trailing space */

	/* Make sure lines are consistent by filling in empty lines */
	for (; new_lines < vf->line_count; new_lines++) {
		dstr_cat_ch(&new_text, '\n');
	}

	if (source) {
		obs_data_set_string(settings, "text", new_text.array);
		obs_source_update(source, settings);
	}

	/* Cleanup */
	for (size_t i = 0; i < temp_lines.num; i++) {
		dstr_free(&temp_lines.array[i]);
	}
	da_free(temp_lines);
	dstr_free(&new_text);
	obs_data_release(partial);
	obs_data_release(settings);
	obs_source_release(source);
	pthread_mutex_unlock(&vf->settings_mutex);
}

static void feed_model(struct vosk_filter *vf)
{
	char *audio_data;
	const char *result = NULL;
	size_t data_len = vf->audio_buffer.size;

	pthread_mutex_lock(&vf->settings_mutex);

	if (data_len < MIN_BYTES || !vf->model || !vf->recognizer) {
		pthread_mutex_unlock(&vf->settings_mutex);
		return;
	}

	if (data_len > MAX_BYTES) {
		data_len = MAX_BYTES;
	}
	audio_data = bmalloc(data_len);
	pthread_mutex_lock(&vf->buffer_mutex);
	circlebuf_pop_front(&vf->audio_buffer, audio_data, data_len);
	pthread_mutex_unlock(&vf->buffer_mutex);

	if (vosk_recognizer_accept_waveform(vf->recognizer, audio_data,
					    (int)data_len)) {
		result = vosk_recognizer_result(vf->recognizer);
		obs_data_t *data = obs_data_create_from_json(result);
		const char *text = obs_data_get_string(data, "text");

		if (vf->finalized_str.len > 0 && *text)
			dstr_cat_ch(&vf->finalized_str, ' ');
		dstr_cat(&vf->finalized_str, text);
		obs_data_release(data);
		vosk_recognizer_reset(vf->recognizer);
		bfree(vf->partial_result);
		vf->partial_result = NULL;
		blog(LOG_DEBUG, "{'result':'%s'}", vf->finalized_str.array);
	} else {
		result = vosk_recognizer_partial_result(vf->recognizer);
		bfree(vf->partial_result);
		vf->partial_result = bstrdup(result);
		blog(LOG_DEBUG, "%s", result);
	}
	pthread_mutex_unlock(&vf->settings_mutex);
	update_text_source(vf);
	bfree(audio_data);
}

static void *feed_model_thread(void *data)
{
	struct vosk_filter *vf = data;
	while (vf->vosk_feed_thread_active) {
		feed_model(vf);
		os_event_wait(vf->feed_model_event);
	}

	return NULL;
}

static void *vosk_load_thread(void *data)
{
	struct vosk_filter *vf = data;
	VoskModel *model = NULL;
	VoskRecognizer *recognizer = NULL;
	char *model_path = NULL;
	bool success = false;

	pthread_mutex_lock(&vf->settings_mutex);
	model_path = bstrdup(vf->model_path);
	pthread_mutex_unlock(&vf->settings_mutex);

	blog(LOG_INFO, "Creating vosk model from '%s'", model_path);
	model = vosk_model_new(model_path);
	if (model) {
		recognizer = vosk_recognizer_new(model, VOSK_SAMPLE_RATE);
		if (recognizer) {
			success = true;
		} else {
			blog(LOG_ERROR,
			     "Failed to create vosk recognizer from '%s'",
			     model_path);
			vosk_model_free(model);
			model = NULL;
		}
	} else {
		blog(LOG_ERROR, "Failed to create vosk model from '%s'",
		     model_path);
	}

	if (success) {
		pthread_mutex_lock(&vf->settings_mutex);
		vf->model = model;
		vf->recognizer = recognizer;
		blog(LOG_INFO, "%s", "Successfully created vosk model.");
		pthread_mutex_unlock(&vf->settings_mutex);
	}

	os_event_signal(vf->vosk_loaded);

	bfree(model_path);
	return NULL;
}

static bool populate_text_source(void *prop, obs_source_t *source)
{
	obs_property_t *list = prop;
	const char *id = obs_source_get_unversioned_id(source);

	if (strncmp(id, "text_", 5) == 0) {
		const char *name = obs_source_get_name(source);
		obs_property_list_add_string(list, name, name);
	}

	return true;
}

static obs_properties_t *vf_get_properties(void *data)
{
	struct vosk_filter *vf = data;
	UNUSED_PARAMETER(vf);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = NULL;
	struct dstr default_dir = {0};

	pthread_mutex_lock(&vf->settings_mutex);
	if (vf->model_path) {
		const char *slash = NULL;

		dstr_copy(&default_dir, vf->model_path);
		dstr_replace(&default_dir, "\\", "/");
		if (default_dir.array)
			slash = strrchr(default_dir.array, '/');
		if (slash)
			dstr_resize(&default_dir,
				    slash - default_dir.array + 1);
	}
	pthread_mutex_unlock(&vf->settings_mutex);

	obs_properties_add_path(props, S_MODEL, T_MODEL, OBS_PATH_DIRECTORY,
				NULL, default_dir.array);
	prop = obs_properties_add_list(props, S_TEXT_SOURCE, T_TEXT_SOURCE,
				       OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(populate_text_source, prop);
	obs_properties_add_int(props, S_LINE_COUNT, T_LINE_COUNT, 0, INT_MAX,
			       1);
	obs_properties_add_int(props, S_LINE_LENGTH, T_LINE_LENGTH, 2, INT_MAX,
			       1);

	dstr_free(&default_dir);
	return props;
}

void vf_get_defaults(obs_data_t *settings)
{
	char *other = obs_module_file(DEFAULT_VOSK_MODEL);
	obs_data_set_default_string(settings, S_MODEL, other);
	obs_data_set_default_int(settings, S_LINE_COUNT, 2);
	obs_data_set_default_int(settings, S_LINE_LENGTH, 80);
	bfree(other);
}

/* Should only be called in an audio thread */
static void check_sample_rate_change(struct vosk_filter *vf)
{
	int64_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
	if (sample_rate != vf->sample_rate) {
		vf->sample_rate = sample_rate;
		vosk_recognizer_free(vf->recognizer);
		vf->recognizer = vosk_recognizer_new(
			vf->model, (float)sample_rate / 1000.0f);
	}
}

static void set_text_proc_(void *data, const char *text)
{
	struct vosk_filter *vf = data;

	pthread_mutex_lock(&vf->buffer_mutex);
	circlebuf_free(&vf->audio_buffer);
	pthread_mutex_unlock(&vf->buffer_mutex);

	pthread_mutex_lock(&vf->settings_mutex);
	for (size_t i = 0; i < vf->finalized_lines.num; i++)
		dstr_free(&vf->finalized_lines.array[i]);
	da_free(vf->finalized_lines);

	if (text)
		dstr_copy(&vf->finalized_str, text);
	else /* Prevent dstr_free, to avoid reallocation */
		dstr_copy(&vf->finalized_str, "");

	bfree(vf->partial_result);
	vf->partial_result = NULL;
	if (vf->recognizer)
		vosk_recognizer_final_result(vf->recognizer);
	pthread_mutex_unlock(&vf->settings_mutex);

	update_text_source(vf);
}

static void set_text_proc(void *data, calldata_t *cd)
{
	const char *text = calldata_string(cd, "text");
	set_text_proc_(data, text);
}

static void clear_text_on_media_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	set_text_proc_(data, "");
}

static void connect_signal_handlers(struct vosk_filter *vf,
				    obs_source_t *source)
{
	if (!vf || !source)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	for (size_t i = 0; i < media_change_signals_count; i++)
		signal_handler_connect(sh, media_change_signals[i],
				       clear_text_on_media_signal, vf);
}

static void disconnect_signal_handlers(struct vosk_filter *vf,
				       obs_source_t *source)
{
	if (!vf || !source)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	for (size_t i = 0; i < media_change_signals_count; i++)
		signal_handler_disconnect(sh, media_change_signals[i],
					  clear_text_on_media_signal, vf);
}

static struct obs_audio_data *vf_filter_audio(void *data,
					      struct obs_audio_data *audio)
{
	if (!audio || !audio->frames)
		return audio;

	struct vosk_filter *vf = data;
	obs_source_t *new_parent, *old_parent;
	uint8_t *resample_data[1]; // we are expecting mono
	uint32_t resample_frames;
	uint64_t ts_offset; // unused
	size_t size;

	/* Old parent */
	old_parent = obs_weak_source_get_source(vf->parent);
	new_parent = obs_filter_get_parent(vf->source);
	if (old_parent != new_parent) {
		disconnect_signal_handlers(vf, old_parent);
		obs_weak_source_release(vf->parent);
		if (new_parent) {
			vf->parent = obs_source_get_weak_source(new_parent);
			connect_signal_handlers(vf, new_parent);
		} else {
			vf->parent = NULL;
		}
	}

	audio_resampler_resample(vf->resampler, resample_data, &resample_frames,
				 &ts_offset, audio->data, audio->frames);
	/* 2 bytes per frame because 16bit */
	size = sizeof(char) * resample_frames * 2;
	pthread_mutex_lock(&vf->buffer_mutex);
	circlebuf_push_back(&vf->audio_buffer, (char *)resample_data[0], size);
	pthread_mutex_unlock(&vf->buffer_mutex);

	if (vf->last_vosk_ts == 0) {
		vf->last_vosk_ts = audio->timestamp;
	} else if (vf->audio_buffer.size > MIN_BYTES) {
		vf->last_vosk_ts = audio->timestamp;
		os_event_signal(vf->feed_model_event);
	}

	obs_source_release(old_parent);
	return audio;
}

static void vf_update(void *data, obs_data_t *settings)
{
	struct vosk_filter *vf = data;
	const char *name = obs_data_get_string(settings, S_TEXT_SOURCE);
	const char *model_path = obs_data_get_string(settings, S_MODEL);
	obs_source_t *source;
	bool should_reload_vosk = false;

	pthread_mutex_lock(&vf->settings_mutex);
	if (model_path && *model_path &&
	    (!vf->model_path || strcmp(model_path, vf->model_path) != 0)) {
		should_reload_vosk = true;
		vosk_model_free(vf->model);
		vosk_recognizer_free(vf->recognizer);
		os_event_reset(vf->vosk_loaded);
		vf->model = NULL;
		vf->recognizer = NULL;
		bfree(vf->model_path);
		vf->model_path = bstrdup(model_path);
	}
	vf->line_length = obs_data_get_int(settings, S_LINE_LENGTH);
	vf->line_count = obs_data_get_int(settings, S_LINE_COUNT);

	source = obs_get_source_by_name(name);
	obs_weak_source_release(vf->text_source);
	if (source) {
		vf->text_source = obs_source_get_weak_source(source);
	} else {
		vf->text_source = NULL;
	}
	pthread_mutex_unlock(&vf->settings_mutex);

	if (should_reload_vosk) {
		set_text_proc_(vf, "");
	} else if (source) {
		update_text_source(vf);
	}

	if (should_reload_vosk) {
		if (vf->vosk_load_thread_created) {
			pthread_join(vf->vosk_load_thread, NULL);
			vf->vosk_load_thread_created = false;
		}
		if (pthread_create(&vf->vosk_load_thread, NULL,
				   vosk_load_thread, vf) != 0) {
			blog(LOG_ERROR, "%s", "Failed to create vosk thread");
			vf->vosk_load_thread_created = false;
		} else {
			vf->vosk_load_thread_created = true;
		}
	}

	obs_source_release(source);
}

static void vf_filter_remove(void *data, obs_source_t *source)
{
	struct vosk_filter *vf = data;
	disconnect_signal_handlers(vf, source);
}

static void vf_destroy(void *data)
{
	struct vosk_filter *vf = data;

	/* Clean up threads */
	if (vf->vosk_load_thread_created)
		pthread_join(vf->vosk_load_thread, NULL);
	os_event_destroy(vf->vosk_loaded);

	vf->vosk_feed_thread_active = false;
	os_event_signal(vf->feed_model_event);
	pthread_join(vf->vosk_feed_thread, NULL);
	os_event_destroy(vf->feed_model_event);

	vosk_recognizer_free(vf->recognizer);
	vosk_model_free(vf->model);
	audio_resampler_destroy(vf->resampler);
	obs_weak_source_release(vf->parent);
	obs_weak_source_release(vf->text_source);
	for (size_t i = 0; i < vf->finalized_lines.num; i++) {
		dstr_free(&vf->finalized_lines.array[i]);
	}
	da_free(vf->finalized_lines);
	dstr_free(&vf->finalized_str);
	//pthread_mutex_unlock(&vf->settings_mutex);

	//pthread_mutex_lock(&vf->buffer_mutex);
	circlebuf_free(&vf->audio_buffer);
	//pthread_mutex_unlock(&vf->buffer_mutex);

	pthread_mutex_destroy(&vf->buffer_mutex);
	pthread_mutex_destroy(&vf->settings_mutex);
	bfree(vf->model_path);
	bfree(vf->partial_result);
	bfree(vf);
}

static void *vf_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct vosk_filter *vf = bzalloc(sizeof(*vf));

	vf->source = source;
	//vf->sample_rate = 0;
	const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());
	da_init(vf->finalized_lines);

	struct resample_info from = {.samples_per_sec = info->samples_per_sec,
				     .speakers = info->speakers,
				     .format = info->format};
	struct resample_info to = {.samples_per_sec = VOSK_SAMPLE_RATE,
				   .speakers = SPEAKERS_MONO,
				   .format = AUDIO_FORMAT_16BIT};
	vf->resampler = audio_resampler_create(&to, &from);

	if (os_event_init(&vf->feed_model_event, OS_EVENT_TYPE_AUTO) != 0) {
		blog(LOG_ERROR, "%s", "Failed to create os_event_t");
		goto error;
	}

	if (os_event_init(&vf->vosk_loaded, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR, "%s", "Failed to create os_event_t");
		goto error;
	}

	vf->vosk_feed_thread_active = true;
	if (pthread_create(&vf->vosk_feed_thread, NULL, feed_model_thread,
			   vf) != 0) {
		blog(LOG_ERROR, "%s", "Failed to create vosk thread");
		goto error;
	}

	pthread_mutex_init_value(&vf->settings_mutex);
	pthread_mutex_init_value(&vf->buffer_mutex);
	if (pthread_mutex_init(&vf->settings_mutex, NULL) != 0 ||
	    pthread_mutex_init(&vf->buffer_mutex, NULL) != 0) {
		blog(LOG_ERROR, "%s", "Failed to create mutex");
		goto error;
	}

	/* Proc handlers */
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void set_text(string text)", set_text_proc, vf);

	vf_update(vf, settings);

	obs_frontend_add_event_callback(vf_frontend_event_cb, vf);
	return vf;

error:
	vf_destroy(vf);
	return NULL;
}

#ifdef TEST_VOSK

void test_get_line_cutoff()
{
	struct dstr str = {0};
	size_t cutoff = 0;
	bool found_sep = false;

	/* Test word longer than word length */
	dstr_copy(&str, "abcdefg");
	found_sep = get_line_cutoff(&str, 5, &cutoff);
	assert(!found_sep);
	assert(cutoff == 4);

	/* Test exact length */
	found_sep = get_line_cutoff(&str, 7, &cutoff);
	assert(found_sep);
	assert(cutoff == 7);

	/* Test find previous space */
	dstr_copy(&str, "abc abc");
	found_sep = get_line_cutoff(&str, 5, &cutoff);
	assert(found_sep);
	assert(cutoff == 3);

	/* Test find previous dash */
	dstr_copy(&str, "abc-abc");
	found_sep = get_line_cutoff(&str, 5, &cutoff);
	assert(found_sep);
	assert(cutoff == 4);

	/* Test find previous dash even with space */
	dstr_copy(&str, "abc abc-abc");
	found_sep = get_line_cutoff(&str, 10, &cutoff);
	assert(found_sep);
	assert(cutoff == 8);

	/* Test find previous space even with dash */
	dstr_copy(&str, "abc-abc abc");
	found_sep = get_line_cutoff(&str, 10, &cutoff);
	assert(found_sep);
	assert(cutoff == 7);

	/* */
	dstr_copy(
		&str,
		"listen below my mother an hour make way they are you know he yeah  no whoa man ");
	found_sep = get_line_cutoff(&str, 80, &cutoff);
	assert(found_sep);
	assert(cutoff == 79);

	/* Test exact length but with a space */
	dstr_copy(&str, "yeah well");
	found_sep = get_line_cutoff(&str, 5, &cutoff);
	assert(found_sep);
	assert(cutoff == 4);
	found_sep = get_line_cutoff(&str, 4, &cutoff);
	assert(found_sep);
	assert(cutoff == 4);

	/* Cleanup */
	dstr_free(&str);
}

#endif // TEST_VOSK

struct obs_source_info vosk_filter_info = {
	.id = "vosk_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.get_name = vf_get_name,
	.output_flags = OBS_SOURCE_AUDIO,
	.create = vf_create,
	.destroy = vf_destroy,
	.filter_remove = vf_filter_remove,
	.filter_audio = vf_filter_audio,
	.get_properties = vf_get_properties,
	.get_defaults = vf_get_defaults,
	.update = vf_update,
};