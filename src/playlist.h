#pragma once

#include <util/darray.h>

struct media_file_data {
	char *path;
	char *filename; // filename with ext, ONLY for folder item checking
	size_t id;
	bool is_url;
	bool is_folder;
	DARRAY(struct media_file_data) folder_items;
	struct media_file_data *parent;
	size_t parent_id; // for folder items
	size_t index;     // makes it easier to switch back to non-shuffle mode
};
