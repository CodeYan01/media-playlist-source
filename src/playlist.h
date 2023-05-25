#include <util/darray.h>

struct media_file_data {
	char *path;
	char *filename; // filename with extension, for folder item checking
	size_t id;
	bool is_url;
	bool is_folder;
	DARRAY(struct media_file_data) folder_items;
	size_t parent_id; // for folder items
};
