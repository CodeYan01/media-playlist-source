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
#include <obs-frontend-api.h>
#include <media-io/audio-resampler.h>
#include <util/config-file.h>
#include <util/circlebuf.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/platform.h>
#include "plugin-macros.generated.h"
#include "vosk_api.h"

#define TEST_VOSK

#define S_TEXT_SOURCE "text_source"
#define S_LINE_LENGTH "line_length"
#define S_LINE_COUNT "line_count"
#define S_MODEL "model"

#define T_(text) obs_module_text(text)
#define T_TEXT_SOURCE T_("TextSource")
#define T_LINE_LENGTH T_("LineLength")
#define T_LINE_COUNT T_("LineCount")
#define T_MODEL T_("VoskModel")

#define DEFAULT_VOSK_MODEL "vosk-models/vosk-model-small-en-us-0.15"
// can be changed, but might be cpu-intensive to recreate the recognizer
#define VOSK_SAMPLE_RATE 48000

#define MAX_BYTES sizeof(char) * 48000 * 2 * 5    /* 5s */
#define MIN_BYTES sizeof(char) * 48000 * 2 * 0.75 /* 0.75s */

static const char *media_change_signals[] = {"media_ended", "media_restart",
					     "media_stopped", "media_next",
					     "media_previous"};
static const size_t media_change_signals_count =
	OBS_COUNTOF(media_change_signals);

typedef DARRAY(struct dstr) dstr_array_t;

struct vosk_filter {
	obs_source_t *source;
	obs_weak_source_t *parent;
	int64_t sample_rate;
	struct circlebuf audio_buffer;
	audio_resampler_t *resampler;

	/* Text source */
	obs_weak_source_t *text_source;
	size_t line_length;
	size_t prev_line_length;
	size_t line_count;

	/* Vosk Model stuff*/
	char *model_path;
	VoskModel *model;
	VoskRecognizer *recognizer;
	uint64_t last_vosk_ts;
	dstr_array_t finalized_lines;
	struct dstr finalized_str;
	char *partial_result;

	/* threading */
	pthread_t vosk_feed_thread;
	pthread_t vosk_load_thread;
	bool vosk_feed_thread_active;
	bool vosk_load_thread_created;
	os_event_t *feed_model_event;
	os_event_t *vosk_loaded;
	pthread_mutex_t settings_mutex;
	pthread_mutex_t buffer_mutex;
};

#ifdef TEST_VOSK
void test_get_line_cutoff();
#endif // TEST_VOSK
