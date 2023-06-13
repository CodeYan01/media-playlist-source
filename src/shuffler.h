#pragma once

#include <obs-module.h>
#include <util/darray.h>
#include "playlist.h"

struct media_file_data;

struct shuffler {
	// we only need pointers
	DARRAY(struct media_file_data *) shuffled_files;
	bool loop;
	size_t head;
	size_t next;
	size_t history;
};

void shuffler_init(struct shuffler *s);
void shuffler_destroy(struct shuffler *s);
void shuffler_reshuffle(struct shuffler *s);
static inline void shuffler_determine_one_(struct shuffler *s,
					   size_t avoid_last_n);
static inline void shuffler_determine_one(struct shuffler *s);
static void shuffler_auto_reshuffle(struct shuffler *s);
bool shuffler_has_prev(struct shuffler *s);
bool shuffler_has_next(struct shuffler *s);
struct media_file_data *shuffler_peek_prev(struct shuffler *s);
struct media_file_data *shuffler_peek_next(struct shuffler *s);
struct media_file_data *shuffler_prev(struct shuffler *s);
struct media_file_data *shuffler_next(struct shuffler *s);
static void shuffler_select_index(struct shuffler *s, size_t index);
void shuffler_select(struct shuffler *s, const struct media_file_data *data);
void shuffler_clear(struct shuffler *s);

// Utility functions
void build_shuffled_files(struct darray *src, struct darray *dst);
size_t find_media_index(struct darray *array, struct media_file_data *data,
			size_t offset);
void shuffler_update_files(struct shuffler *s, struct darray *array);
void shuffler_set_loop(struct shuffler *s, bool loop);

#ifdef TEST_SHUFFLER
int test_shuffler();
#endif // TEST_SHUFFLER
