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

/*
This uses the Fisher-Yates shuffle algorithm mostly based on VLC's
implementation of it. I adjusted it to be compatible with OBS and this plugin.
A very detailed explanation on the concept is laid out at
https://github.com/videolan/vlc/blob/f7bb59d9f51cc10b25ff86d34a3eff744e60c46e/src/playlist/randomizer.c

I am very grateful for the work that they have done.

This lets us keep the shuffling order (previous items) even when the user
selects a specific file in the playlist. Also lets us do reshuffling keeping the
history.
*/

#include "shuffler.h"

/* On auto-reshuffle, avoid selecting the same item before at least
 * NOT_SAME_BEFORE other items have been selected (between the end of the
 * previous shuffle and the start of the new shuffle). */
#define NOT_SAME_BEFORE 1

void shuffler_init(struct shuffler *s)
{
	s->head = 0;
	s->next = 0;
	s->history = 0;
	s->loop = false;
	da_init(s->shuffled_files);
}

void shuffler_destroy(struct shuffler *s)
{
	da_free(s->shuffled_files);
}

void shuffler_set_loop(struct shuffler *s, bool loop)
{
	s->loop = loop;
}

void shuffler_reshuffle(struct shuffler *s)
{
	s->head = 0;
	s->next = 0;
	s->history = s->shuffled_files.num;
}

static inline void shuffler_determine_one_(struct shuffler *s, size_t avoid_last_n)
{
	assert(s->head < s->shuffled_files.num);
	assert(s->shuffled_files.num - s->head > avoid_last_n);
	size_t range_len = s->shuffled_files.num - s->head - avoid_last_n;
	size_t selected = s->head + (rand() % range_len);
	da_swap(s->shuffled_files, s->head, selected);

	if (s->head == s->history)
		s->history++;
	s->head++;
}

static inline void shuffler_determine_one(struct shuffler *s)
{
	shuffler_determine_one_(s, 0);
}

static void shuffler_auto_reshuffle(struct shuffler *s)
{
	assert(s->shuffled_files.num > 0);
	s->head = 0;
	s->next = 0;
	s->history = 0; /* the whole content is history */
	size_t avoid_last_n = NOT_SAME_BEFORE;
	if (avoid_last_n > s->shuffled_files.num - 1)
		/* cannot ignore all */
		avoid_last_n = s->shuffled_files.num - 1;
	while (avoid_last_n)
		shuffler_determine_one_(s, avoid_last_n--);
}

bool shuffler_has_prev(struct shuffler *s)
{
	if (!s->loop)
		/* a previous exists if the current is > 0, i.e. next > 1 */
		return s->next > 1;

	if (!s->shuffled_files.num)
		/* avoid modulo 0 */
		return false;

	/* there is no previous only if (current - history) == 0 (modulo size),
     * i.e. (next - history) == 1 (modulo size) */
	return (s->next + s->shuffled_files.num - s->history) % s->shuffled_files.num != 1;
}

bool shuffler_has_next(struct shuffler *s)
{
	return s->shuffled_files.num && (s->loop || s->next < s->shuffled_files.num);
}

struct media_file_data *shuffler_peek_prev(struct shuffler *s)
{
	assert(shuffler_has_prev(s));
	size_t index = (s->next + s->shuffled_files.num - 2) % s->shuffled_files.num;
	return s->shuffled_files.array[index];
}

struct media_file_data *shuffler_peek_next(struct shuffler *s)
{
	assert(shuffler_has_next(s));

	if (s->next == s->shuffled_files.num && s->next == s->history) {
		assert(s->loop);
		shuffler_auto_reshuffle(s);
	}

	if (s->next == s->head)
		/* execute 1 step of the Fisher-Yates shuffle */
		shuffler_determine_one(s);

	return s->shuffled_files.array[s->next];
}

struct media_file_data *shuffler_prev(struct shuffler *s)
{
	assert(shuffler_has_prev(s));
	struct media_file_data *item = shuffler_peek_prev(s);
	s->next = s->next ? s->next - 1 : s->shuffled_files.num - 1;
	return item;
}

struct media_file_data *shuffler_next(struct shuffler *s)
{
	assert(shuffler_has_next(s));
	struct media_file_data *item = shuffler_peek_next(s);
	s->next++;
	if (s->next == s->shuffled_files.num && s->next != s->head)
		s->next = 0;
	return item;
}

bool shuffler_add(struct shuffler *s, struct media_file_data items[], size_t count)
{
	for (size_t i = 0; i < count; i++) {
		struct media_file_data *ptr = &items[i];
		da_insert(s->shuffled_files, s->history + i, &ptr);
	}
	/* the insertion shifted history (and possibly next) */
	if (s->next > s->history)
		s->next += count;
	s->history += count;
	return true;
}

static void shuffler_select_index(struct shuffler *s, size_t index)
{
	struct media_file_data *selected = s->shuffled_files.array[index];
	if (s->history && index >= s->history) {
		if (index > s->history) {
			memmove(&s->shuffled_files.array[s->history + 1], &s->shuffled_files.array[s->history],
				(index - s->history) * sizeof(selected));
			index = s->history;
		}
		s->history = (s->history + 1) % s->shuffled_files.num;
	}

	if (index >= s->head) {
		s->shuffled_files.array[index] = s->shuffled_files.array[s->head];
		s->shuffled_files.array[s->head] = selected;
		s->head++;
	} else if (index < s->shuffled_files.num - 1) {
		memmove(&s->shuffled_files.array[index], &s->shuffled_files.array[index + 1],
			(s->head - index - 1) * sizeof(selected));
		s->shuffled_files.array[s->head - 1] = selected;
	}

	s->next = s->head;
}

void shuffler_select(struct shuffler *s, const struct media_file_data *data)
{
	size_t idx = da_find(s->shuffled_files, &data, 0);
	assert(idx != DARRAY_INVALID);
	shuffler_select_index(s, idx);
}

static void shuffler_remove_at(struct shuffler *s, size_t index)
{
	/* All this memmove magic is just an optimized way of removing the 
	 * element from the array, so you don't have to shift the whole array
	 * to the left when it is not needed (as there is an unordered part)
	 * Instead of shifting the unordered part, this code just takes the last 
	 * element from the unordered part and puts it into `index`.
	 */
	/*
	 * 0          head                                history   next  size
	 * |-----------|...................................|---------|-----|
	 * |<--------->|                                   |<------------->|
	 *    ordered            order irrelevant               ordered
	 */

	/* update next before index may be updated */
	if (index < s->next)
		s->next--;

	if (index < s->head) {
		/* item was selected, keep the selected part ordered */
		memmove(&s->shuffled_files.array[index], &s->shuffled_files.array[index + 1],
			(s->head - index - 1) * sizeof(*s->shuffled_files.array));
		s->head--;
		index = s->head; /* the new index to remove */
	}

	if (index < s->history) {
		/* this part is unordered, no need to shift all items */
		s->shuffled_files.array[index] = s->shuffled_files.array[s->history - 1];
		index = s->history - 1;
		s->history--;
	}

	if (index < s->shuffled_files.num - 1) {
		/* shift the ordered history part by one */
		memmove(&s->shuffled_files.array[index], &s->shuffled_files.array[index + 1],
			(s->shuffled_files.num - index - 1) * sizeof(*s->shuffled_files.array));
	}

	s->shuffled_files.num--;
}

static void shuffler_remove_one(struct shuffler *s, const struct media_file_data *item)
{
	size_t index = da_find(s->shuffled_files, &item, 0);
	assert(index >= 0 && index != DARRAY_INVALID); /* item must exist */
	shuffler_remove_at(s, index);
}

void shuffler_remove(struct shuffler *s, struct media_file_data *const items[], size_t count)
{
	for (size_t i = 0; i < count; ++i)
		shuffler_remove_one(s, items[i]);
}

void shuffler_clear(struct shuffler *s)
{
	da_free(s->shuffled_files);
	s->head = 0;
	s->next = 0;
	s->history = 0;
}

void build_shuffled_files(struct darray *src, struct darray *dst)
{
	DARRAY(struct media_file_data) src_files;
	DARRAY(struct media_file_data *) dst_files;
	src_files.da = *src;
	dst_files.da = *dst;

	for (size_t i = 0; i < src_files.num; i++) {
		struct media_file_data *data = &src_files.array[i];
		if (data->is_folder) {
			for (size_t j = 0; j < data->folder_items.num; j++) {
				struct media_file_data *item;
				item = &data->folder_items.array[j];
				da_push_back(dst_files, &item);
			}
		} else {
			da_push_back(dst_files, &data);
		}
	}
	*dst = dst_files.da;
}

size_t find_media_index(struct darray *array, struct media_file_data *search_data, size_t offset)
{
	DARRAY(struct media_file_data *) files;
	files.da = *array;

	for (size_t i = offset; i < files.num; i++) {
		struct media_file_data *current_data = files.array[i];
		if (search_data->parent_id) { // folder item
			/* used to try to check until the parent id is now
			 * different, thus reaching the last folder item in
			 * the same folder, but it doesn't make sense because
			 * the found media will be swapped with `offset`, 
			 * so there will be non-sibling folder items mixed in.
			 * This function will also be necessary if user plays
			 * a specific file while shuffle is on.
			 * 
			 * In other words, don't break this code.
			 */

			if (strcmp(current_data->parent_id, search_data->parent_id) == 0) {
				int match = strcmp(search_data->filename, current_data->filename);
				if (match == 0) {
					return i;
				}
			}
		} else if (strcmp(search_data->id, current_data->id) == 0) {
			return i;
		}
	}
	return DARRAY_INVALID;
}

void shuffler_update_files(struct shuffler *s, struct darray *array)
{
	DARRAY(struct media_file_data) new_files;
	DARRAY(struct media_file_data *) shuffled_files;
	new_files.da = *array; // sequential
	da_init(shuffled_files);

	if (new_files.num) {
		// build new shuffled files (really just flattened)
		build_shuffled_files(&new_files.da, &shuffled_files.da);

		if (s->shuffled_files.num == 0) {
			s->history = shuffled_files.num; // no history
		} else {
			size_t new_head = 0;
			size_t new_next = s->next;
			size_t new_history = shuffled_files.num;

			// Find determined media
			for (size_t i = 0; i < s->head; i++) {
				struct media_file_data *old_data = s->shuffled_files.array[i];
				size_t new_idx = find_media_index(&shuffled_files.da, old_data, new_head);
				if (new_idx != DARRAY_INVALID) {
					da_swap(shuffled_files, new_head++, new_idx);
				} else {
					if (i < s->next)
						new_next--;
				}
			}
			// history can never be lower than head, it represents the first
			// element of the previous cycle history
			for (size_t i = s->shuffled_files.num - 1; i >= s->history; i--) {
				struct media_file_data *old_data = s->shuffled_files.array[i];
				size_t new_idx = find_media_index(&shuffled_files.da, old_data, new_head);
				if (new_idx != DARRAY_INVALID) {
					da_swap(shuffled_files, --new_history, new_idx);
				} else {
					if (i < s->next)
						new_next--;
				}
			}
			s->head = new_head;
			s->next = new_next;
			s->history = new_history;
		}
		da_free(s->shuffled_files);
		s->shuffled_files.da = shuffled_files.da;
	} else {
		shuffler_clear(s);
		da_free(shuffled_files);
	}
}

#ifdef TEST_SHUFFLER
#include <util/dstr.h>
static void ArrayInitOffset(struct darray *main_array, size_t len, size_t offset)
{
	DARRAY(struct media_file_data) main_files;
	main_files.da = *main_array;

	for (size_t i = offset; i < len + offset; ++i) {
		struct media_file_data data = {0};
		struct dstr id = {0};
		dstr_printf(&id, "%zu", i + 1);
		data.id = bstrdup(id.array);
		data.index = i;
		da_init(data.folder_items);
		da_push_back(main_files, &data);
		dstr_free(&id);
	}
	*main_array = main_files.da;
}

static void ArrayCreateFolderItems(struct media_file_data *media, size_t len, size_t offset)
{
	media->is_folder = true;
	for (size_t i = offset; i < len + offset; i++) {
		struct media_file_data folder_item = {0};
		struct dstr filename = {0};
		dstr_catf(&filename, "%zu", i);
		folder_item.parent_id = media->id;
		folder_item.filename = bstrdup(filename.array);
		da_push_back(media->folder_items, &folder_item);
		dstr_free(&filename);
	}
}

static void ArrayInit(struct darray *main_array, size_t len)
{
	ArrayInitOffset(main_array, len, 0);
}

static void ArrayDeepCopy(struct darray *src, struct darray *dst)
{
	DARRAY(struct media_file_data) orig;
	DARRAY(struct media_file_data) copy;
	orig.da = *src;
	copy.da = *dst;
	da_copy(copy, orig);

	for (size_t i = 0; i < orig.num; i++) {
		struct media_file_data *media = &copy.array[i];
		if (media->filename) {
			media->filename = bstrdup(orig.array[i].filename);
		}
		da_init(media->folder_items);
		da_copy(media->folder_items, orig.array[i].folder_items);
		for (size_t j = 0; j < media->folder_items.num; j++) {
			struct media_file_data *folder_item = &media->folder_items.array[j];
			if (folder_item->filename) {
				folder_item->filename = bstrdup(folder_item->filename);
			}
		}
	}
	*dst = copy.da;
}

static void ArrayEraseRange(struct darray *array, size_t from, size_t to)
{
	DARRAY(struct media_file_data *) files;
	files.da = *array;
	for (size_t i = from; i < to; i++) {
		struct media_file_data *data = files.array[i];
		bfree(data->id);
		for (size_t j = 0; j < data->folder_items.num; j++) {
			bfree(data->folder_items.array[j].filename);
			bfree(data->folder_items.array[j].id);
		}
		da_free(data->folder_items);
	}
	da_erase_range(files, from, to);
}

static void ArrayDestroy(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;
	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *data = &files.array[i];
		bfree(data->id);
		for (size_t j = 0; j < data->folder_items.num; j++) {
			bfree(data->folder_items.array[j].filename);
			bfree(data->folder_items.array[j].id);
		}
		da_free(data->folder_items);
	}
	da_free(files);
	*array = files.da;
}

/* Unlike in vlc, the shuffler files are recreated every time the properties
 * are changed, so we can't just do pointer equality tests. 
 */
static bool media_equal(struct media_file_data *data1, struct media_file_data *data2)
{
	if (data1->parent_id) {
		return strcmp(data1->parent_id, data2->parent_id) == 0 && strcmp(data1->filename, data2->filename) == 0;
	} else {
		return strcmp(data1->id, data2->id) == 0;
	}
}

static void test_all_items_selected_exactly_once(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	bool selected[SIZE] = {0};
	for (int i = 0; i < SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
		assert(!selected[item->index]); /* never selected twice */
		selected[item->index] = true;
	}

	assert(!shuffler_has_next(&shuffler)); /* no more items */

	for (int i = 0; i < SIZE; ++i)
		assert(selected[i]); /* all selected */

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_all_items_selected_exactly_once_per_cycle(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	for (int cycle = 0; cycle < 4; ++cycle) {
		bool selected[SIZE] = {0};
		for (int i = 0; i < SIZE; ++i) {
			assert(shuffler_has_next(&shuffler));
			struct media_file_data *item = shuffler_next(&shuffler);
			assert(item);
			assert(!selected[item->index]); /* never selected twice */
			selected[item->index] = true;
		}

		assert(shuffler_has_next(&shuffler)); /* still has items in loop */

		for (int i = 0; i < SIZE; ++i)
			assert(selected[i]); /* all selected */
	}

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_all_items_selected_exactly_once_with_additions(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, 75);
	assert(ok);

	bool selected[SIZE] = {0};
	for (int i = 0; i < 50; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
		assert(!selected[item->index]); /* never selected twice */
		selected[item->index] = true;
	}

	ok = shuffler_add(&shuffler, &items.array[75], 25);
	assert(ok);
	for (int i = 50; i < SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
		assert(!selected[item->index]); /* never selected twice */
		selected[item->index] = true;
	}

	assert(!shuffler_has_next(&shuffler)); /* no more items */

	for (int i = 0; i < SIZE; ++i)
		assert(selected[i]); /* all selected */

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_all_items_selected_exactly_once_with_removals(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	bool selected[SIZE] = {0};
	for (int i = 0; i < 50; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
		assert(!selected[item->index]); /* never selected twice */
		selected[item->index] = true;
	}

	struct media_file_data *to_remove[20];
	/* copy 10 items already selected */
	memcpy(to_remove, &shuffler.shuffled_files.array[20], 10 * sizeof(*to_remove));
	/* copy 10 items not already selected */
	memcpy(&to_remove[10], &shuffler.shuffled_files.array[70], 10 * sizeof(*to_remove));

	shuffler_remove(&shuffler, to_remove, 20);

	for (int i = 50; i < SIZE - 10; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
		assert(!selected[item->index]); /* never selected twice */
		selected[item->index] = true;
	}

	assert(!shuffler_has_next(&shuffler)); /* no more items */

	int count = 0;
	for (int i = 0; i < SIZE; ++i)
		if (selected[i])
			count++;

	assert(count == SIZE - 10);

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_cycle_after_manual_selection(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, 100);
	assert(ok);

	/* force selection of the first item */
	shuffler_select(&shuffler, shuffler.shuffled_files.array[0]);

	for (int i = 0; i < 2 * SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
	}

	assert(shuffler_has_next(&shuffler)); /* still has items in loop */

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_cycle_with_additions_and_removals(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, 80);
	assert(ok);

	for (int i = 0; i < 30; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
	}

	struct media_file_data *to_remove[20];
	/* copy 10 items already selected */
	memcpy(to_remove, &shuffler.shuffled_files.array[15], 10 * sizeof(*to_remove));
	/* copy 10 items not already selected */
	memcpy(&to_remove[10], &shuffler.shuffled_files.array[60], 10 * sizeof(*to_remove));

	shuffler_remove(&shuffler, to_remove, 20);

	/* it remains 40 items in the first cycle (30 already selected, and 10
     * removed from the 50 remaining) */
	for (int i = 0; i < 40; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
	}

	/* the first cycle is complete */
	assert(shuffler_has_next(&shuffler));
	/* force the determination of the first item of the next cycle */
	struct media_file_data *item = shuffler_peek_next(&shuffler);
	assert(item);

	assert(shuffler.shuffled_files.num == 60);
	assert(shuffler.history == 1);

	/* save current history */
	struct media_file_data *history[59];
	memcpy(history, &shuffler.shuffled_files.array[1], 59 * sizeof(*history));

	/* insert 20 new items */
	ok = shuffler_add(&shuffler, &items.array[80], 20);
	assert(ok);

	assert(shuffler.shuffled_files.num == 80);
	assert(shuffler.history == 21);

	for (int i = 0; i < 59; ++i)
		assert(history[i] == shuffler.shuffled_files.array[21 + i]);

	/* remove 10 items in the history part */
	memcpy(to_remove, &shuffler.shuffled_files.array[30], 10 * sizeof(*to_remove));
	shuffler_remove(&shuffler, to_remove, 10);

	assert(shuffler.shuffled_files.num == 70);
	assert(shuffler.history == 21);

	/* the other items in the history must be kept in order */
	for (int i = 0; i < 9; ++i)
		assert(history[i] == shuffler.shuffled_files.array[21 + i]);
	for (int i = 0; i < 40; ++i)
		assert(history[i + 19] == shuffler.shuffled_files.array[30 + i]);

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_force_select_new_item(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	bool selected[SIZE] = {0};
	for (int i = 0; i < SIZE; ++i) {
		struct media_file_data *item;
		if (i != 50) {
			assert(shuffler_has_next(&shuffler));
			item = shuffler_next(&shuffler);
		} else {
			/* force the selection of a new item not already selected */
			item = shuffler.shuffled_files.array[62];
			shuffler_select(&shuffler, item);
			/* the item should now be the last selected one */
			assert(shuffler.shuffled_files.array[shuffler.next - 1] == item);
		}
		assert(item);
		assert(!selected[item->index]); /* never selected twice */
		selected[item->index] = true;
	}

	assert(!shuffler_has_next(&shuffler)); /* no more items */

	for (int i = 0; i < SIZE; ++i)
		assert(selected[i]); /* all selected */

	shuffler_destroy(&shuffler);
	da_free(items);
}

static void test_force_select_item_already_selected(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 100
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	bool selected[SIZE] = {0};
	/* we need an additional loop cycle, since we select the same item twice */
	for (int i = 0; i < SIZE + 1; ++i) {
		struct media_file_data *item;
		if (i != 50) {
			assert(shuffler_has_next(&shuffler));
			item = shuffler_next(&shuffler);
		} else {
			/* force the selection of an item already selected */
			item = shuffler.shuffled_files.array[42];
			shuffler_select(&shuffler, item);
			/* the item should now be the last selected one */
			assert(shuffler.shuffled_files.array[shuffler.next - 1] == item);
		}
		assert(item);
		/* never selected twice, except for item 50 */
		assert((i != 50) ^ selected[item->index]);
		selected[item->index] = true;
	}

	assert(!shuffler_has_next(&shuffler)); /* no more items */

	for (int i = 0; i < SIZE; ++i)
		assert(selected[i]); /* all selected */

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_prev(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 10
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	assert(!shuffler_has_prev(&shuffler));

	struct media_file_data *actual[SIZE];
	for (int i = 0; i < SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		actual[i] = shuffler_next(&shuffler);
		assert(actual[i]);
	}

	assert(!shuffler_has_next(&shuffler));

	for (int i = SIZE - 2; i >= 0; --i) {
		assert(shuffler_has_prev(&shuffler));
		struct media_file_data *item = shuffler_prev(&shuffler);
		assert(item == actual[i]);
	}

	assert(!shuffler_has_prev(&shuffler));

	for (int i = 1; i < SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item == actual[i]);
	}

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_prev_with_select(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 10
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	assert(!shuffler_has_prev(&shuffler));

	struct media_file_data *actual[SIZE];
	for (int i = 0; i < 5; ++i) {
		assert(shuffler_has_next(&shuffler));
		actual[i] = shuffler_next(&shuffler);
		assert(actual[i]);
	}

	shuffler_select(&shuffler, actual[2]);

	struct media_file_data *item;

	assert(shuffler_has_prev(&shuffler));
	item = shuffler_prev(&shuffler);
	assert(item == actual[4]);

	assert(shuffler_has_prev(&shuffler));
	item = shuffler_prev(&shuffler);
	assert(item == actual[3]);

	assert(shuffler_has_prev(&shuffler));
	item = shuffler_prev(&shuffler);
	assert(item == actual[1]);

	assert(shuffler_has_prev(&shuffler));
	item = shuffler_prev(&shuffler);
	assert(item == actual[0]);

	assert(!shuffler_has_prev(&shuffler));

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_prev_across_reshuffle_loops(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

#define SIZE 10
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	assert(!shuffler_has_prev(&shuffler));

	struct media_file_data *actual[SIZE];
	for (int i = 0; i < SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		actual[i] = shuffler_next(&shuffler);
		assert(actual[i]);
	}

	assert(!shuffler_has_next(&shuffler));
	shuffler_set_loop(&shuffler, true);
	assert(shuffler_has_next(&shuffler));

	struct media_file_data *actualnew[4];
	/* determine the 4 first items */
	for (int i = 0; i < 4; ++i) {
		assert(shuffler_has_next(&shuffler));
		actualnew[i] = shuffler_next(&shuffler);
		assert(actualnew[i]);
	}

	/* go back to the first */
	for (int i = 2; i >= 0; --i) {
		assert(shuffler_has_prev(&shuffler));
		actualnew[i] = shuffler_prev(&shuffler);
		assert(actualnew[i]);
	}

	assert(actualnew[0] == shuffler.shuffled_files.array[0]);

	/* from now, any "prev" goes back to the history */

	int index_in_actual = 9;
	for (int i = 0; i < 6; ++i) {
		assert(shuffler_has_prev(&shuffler));
		struct media_file_data *item = shuffler_prev(&shuffler);

		int j;
		for (j = 3; j >= 0; --j)
			if (item == actualnew[j])
				break;
		bool in_actualnew = j != 0;

		if (in_actualnew)
			/* the item has been selected for the new order, it is not in the
	     * history anymore */
			index_in_actual--;
		else
			/* the remaining previous items are retrieved in reverse order in
	     * the history */
			assert(item == actual[index_in_actual]);
	}

	/* no more history: 4 in the current shuffle, 6 in the history */
	assert(!shuffler_has_prev(&shuffler));

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

/* when loop is enabled, we must take care that the last items of the
 * previous order are not the same as the first items of the new order */
static void test_loop_respect_not_same_before(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE (NOT_SAME_BEFORE + 2)
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	struct media_file_data *actual[SIZE];
	for (int i = 0; i < SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		actual[i] = shuffler_next(&shuffler);
	}

	for (int cycle = 0; cycle < 20; cycle++) {
		/* check that the first items are not the same as the last ones of the
	 * previous order */
		for (int i = 0; i < NOT_SAME_BEFORE; ++i) {
			assert(shuffler_has_next(&shuffler));
			actual[i] = shuffler_next(&shuffler);
			for (int j = (i + SIZE - NOT_SAME_BEFORE) % SIZE; j != i; j = (j + 1) % SIZE) {
				assert(actual[i] != actual[j]);
			}
		}
		for (int i = NOT_SAME_BEFORE; i < SIZE; ++i) {
			assert(shuffler_has_next(&shuffler));
			actual[i] = shuffler_next(&shuffler);
		}
	}

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

/* if there are less items than NOT_SAME_BEFORE, obviously we can't avoid
 * repeating last items in the new order, but it must still work */
static void test_loop_respect_not_same_before_impossible(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE NOT_SAME_BEFORE
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);

	bool ok = shuffler_add(&shuffler, items.array, SIZE);
	assert(ok);

	for (int i = 0; i < 10 * SIZE; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
	}

	shuffler_destroy(&shuffler);
	da_free(items);
#undef SIZE
}

static void test_has_prev_next_empty(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);

	assert(!shuffler_has_prev(&shuffler));
	assert(!shuffler_has_next(&shuffler));

	shuffler_set_loop(&shuffler, true);

	assert(!shuffler_has_prev(&shuffler));

	/* there are always next items in loop mode */
	assert(shuffler_has_next(&shuffler));

	shuffler_destroy(&shuffler);
}

static void test_update_files_with_additions_and_removals(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE 100
	DARRAY(struct media_file_data) all_items;
	da_init(all_items);
	ArrayInit(&all_items.da, SIZE);
	DARRAY(struct media_file_data) items;
	// copy, since removals affect the shuffled_files of the shuffler
	// because shuffled_files are only pointers
	DARRAY(struct media_file_data) items1;
	da_init(items);
	da_init(items1);
	da_copy_array(items, all_items.array, 80);

	shuffler_update_files(&shuffler, &items.da);

	for (int i = 0; i < 30; i++) {
		assert(shuffler_has_next(&shuffler));
		shuffler_select_index(&shuffler, i);
		assert(shuffler.next == i + 1);
		struct media_file_data *item = shuffler.shuffled_files.array[shuffler.next - 1];
		assert(item);
	}

	assert(shuffler.next == 30);

	da_move(items1, items);
	da_copy(items, items1);
	da_erase_range(items, 60, 70);
	da_erase_range(items, 15, 25);
	shuffler_update_files(&shuffler, &items.da);

	assert(shuffler.next == 20);

	/* it remains 40 items in the first cycle (30 already selected, and 10
     * removed from the 50 remaining) */
	for (int i = 0; i < 40; ++i) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item);
	}

	/* the first cycle is complete */
	assert(shuffler.next == shuffler.shuffled_files.num);
	assert(shuffler_has_next(&shuffler));
	/* force the determination of the first item of the next cycle */
	struct media_file_data *item = shuffler_peek_next(&shuffler);
	assert(item);

	assert(shuffler.shuffled_files.num == 60);
	assert(shuffler.history == 1);

	/* save current history */
	struct media_file_data *history[59];
	memcpy(history, &shuffler.shuffled_files.array[1], 59 * sizeof(*history));

	/* insert 20 new items */
	da_push_back_array(items, &all_items.array[80], 20);
	shuffler_update_files(&shuffler, &items.da);

	assert(shuffler.shuffled_files.num == 80);
	assert(shuffler.history == 21);

	for (int i = 0; i < 59; ++i)
		assert(history[i] == shuffler.shuffled_files.array[21 + i]);

	/* remove 10 items in the history part */
	da_move(items1, items);
	da_copy(items, items1);
	for (size_t i = 39; i >= 30; i--) {
		struct media_file_data *data = shuffler.shuffled_files.array[i];
		da_erase_item(items, data);
	}
	shuffler_update_files(&shuffler, &items.da);

	assert(shuffler.shuffled_files.num == 70);
	assert(shuffler.history == 21);

	/* the other items in the history must be kept in order */
	for (int i = 0; i < 9; ++i)
		assert(strcmp(history[i]->id, shuffler.shuffled_files.array[21 + i]->id) == 0);
	for (int i = 0; i < 40; ++i)
		assert(strcmp(history[i + 19]->id, shuffler.shuffled_files.array[30 + i]->id) == 0);

	shuffler_destroy(&shuffler);
	da_free(items);
	da_free(items1);
	da_free(all_items);
#undef SIZE
}

static void test_update_files_folders_with_additions_and_removals(void)
{
	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);

#define SIZE 40
	DARRAY(struct media_file_data) items;
	da_init(items);
	ArrayInit(&items.da, SIZE);
	// folder media aren't counted in shuffled_files, as they are flattened
	// so we make the folder items 5+1 so it's still a round number
	for (size_t i = 0; i < SIZE; i += 5)
		ArrayCreateFolderItems(&items.array[i], 6, 0);
	// copy, since removals affect the shuffled_files of the shuffler
	// because shuffled_files are only pointers
	DARRAY(struct media_file_data) items2;
	DARRAY(struct media_file_data *) history;
	da_init(items2);
	da_init(history);

	shuffler_update_files(&shuffler, &items.da);

	assert(shuffler.shuffled_files.num == 80);

	// 6 (folder) + 4 + 6 (folder) + 4 + 4 (half folder)
	shuffler.head = 24;
	shuffler.next = 24;

	/* remove 2 folder items each from the first 3 folders */
	da_init(items2);
	da_move(items2, items);
	ArrayDeepCopy(&items2.da, &items.da);
	for (size_t i = 0; i < 11; i += 5) {
		struct media_file_data *data = &items.array[i];
		for (size_t j = 0; j < 2; j++) {
			bfree(data->folder_items.array[j].filename);
		}
		da_erase_range(data->folder_items, 0, 2);

		ArrayCreateFolderItems(data, 2, 6);
	}
	shuffler_update_files(&shuffler, &items.da);
	ArrayDestroy(&items2.da);

	assert(shuffler.shuffled_files.num == 80);
	assert(shuffler.head == 18); // added items should not be before head
	assert(shuffler.next == 18);

	shuffler_clear(&shuffler);
	shuffler_update_files(&shuffler, &items.da);
	shuffler.head = 1;
	shuffler.history = 1;

	/* remove 2 folder items each from the 3 folders after the first one */
	da_init(items2);
	da_move(items2, items);
	ArrayDeepCopy(&items2.da, &items.da);

	for (size_t i = 5; i < 16; i += 5) {
		struct media_file_data *data = &items.array[i];
		for (size_t j = 0; j < 2; j++) {
			bfree(data->folder_items.array[j].filename);
		}
		da_erase_range(data->folder_items, 0, 2);
	}
	build_shuffled_files(&items.da, &history.da);
	da_erase(history, 0);
	// we add new folder items after saving history because
	// added items are not part of the history
	for (size_t i = 5; i < 16; i += 5) {
		struct media_file_data *data = &items.array[i];
		ArrayCreateFolderItems(data, 2, 8);
	}

	shuffler_update_files(&shuffler, &items.da);

	assert(shuffler.shuffled_files.num == 80);
	assert(shuffler.next == 0);
	assert(shuffler.history == 7); // 6 new items

	for (int i = 0; i < 73; ++i) {
		assert(media_equal(history.array[i], shuffler.shuffled_files.array[7 + i]));
	}
	ArrayDestroy(&items2.da);

	/* remove 10 items in the history part */
	da_init(items2);
	da_move(items2, items);
	ArrayDeepCopy(&items2.da, &items.da);
	for (size_t i = 0; i < items.array[25].folder_items.num; i++) {
		bfree(items.array[25].folder_items.array[i].filename);
	}
	da_free(items.array[25].folder_items);
	da_erase_range(items, 25, 30);
	da_erase_range(history, 0, history.num);
	da_copy(history, shuffler.shuffled_files);

	shuffler_update_files(&shuffler, &items.da);

	assert(shuffler.shuffled_files.num == 70);
	assert(shuffler.history == 7); // unchanged

	/* the other items in the history must be kept in order */
	for (int i = 7; i < 50; ++i)
		assert(media_equal(history.array[i], shuffler.shuffled_files.array[i]));
	for (int i = 61; i < 80; ++i)
		assert(media_equal(history.array[i], shuffler.shuffled_files.array[i - 10]));
	ArrayDestroy(&items2.da);

	shuffler_destroy(&shuffler);
	ArrayDestroy(&items.da);
	da_free(items);
	da_free(items2);
	da_free(history);
#undef SIZE
}

int test_shuffler()
{
	// vlc tests
	test_all_items_selected_exactly_once();
	test_all_items_selected_exactly_once_per_cycle();
	test_all_items_selected_exactly_once_with_additions();
	test_all_items_selected_exactly_once_with_removals();
	test_cycle_after_manual_selection();
	test_cycle_with_additions_and_removals();
	test_force_select_new_item();
	test_force_select_item_already_selected();
	test_prev();
	test_prev_with_select();
	test_prev_across_reshuffle_loops();
	test_loop_respect_not_same_before();
	test_loop_respect_not_same_before_impossible();
	test_has_prev_next_empty();

	// my tests
	test_update_files_with_additions_and_removals();
	test_update_files_folders_with_additions_and_removals();
	return 0;
}

#endif
