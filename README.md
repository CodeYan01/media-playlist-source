# Media Playlist Source

## Introduction

An OBS Plugin that serves as an alternative to VLC Video Source. It uses the
Media Source internally.

## Features

- Allows editing the playlist without restarting the video, even if files are
reordered.
- Allows editing any setting without restarting the video.
- Saves the currently playing file so it would be played when OBS restarts.
- Allows selecting a file or folder item from the list to play.
- Shuffling (Based on VLC 4's implementation)
- - Allows adding/removing items from the playlist without the need to
reshuffle. The shuffling order of already played items will be saved, so
clicking Previous Item will still play the previous item. Selecting a file or
folder item also does not break the history.
- - Reshuffles when the last file in the playlist is played out, without
affecting history.
- Shows the filename of the current file in the Properties window.
- Has an option to play the first file or the current file when the source is
restarted.

## Limitations

- The Properties window will show the filename of the current file, but it can
not be automatically updated when the video ends as it could cause OBS to crash
when the video ends while interacting with the Properties window. Reopening the
Properties window will refresh the shown current file.
- While this plugin works with OBS 28 and up, it requires this change at 
https://github.com/obsproject/obs-studio/pull/8051 that allows this plugin to
save the index of the current file, so that restarting playback is not needed
when editing the list. This change isn't merged yet as of OBS 29.1.3.
A custom build with this PR is available
[here](https://github.com/CodeYan01/obs-studio/releases).
- Does not support audio track or subtitle selection yet.
- Does not automatically refresh folder contents yet, but can be manually done
by saving the settings again.

## For Developers
To find out the keys used in the source [settings](https://docs.obsproject.com/reference-sources#c.obs_source_get_settings),
use [obs_data_get_json](https://docs.obsproject.com/reference-settings#c.obs_data_get_json)
to view it, or check the scene collection json. You could also check
[src/media-playlist-source.c](src/media-playlist-source.c)

To select a file programmatically:
```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
struct calldata cd = {0};
calldata_set_int(&cd, "media_index", 3); // 4th file
calldata_set_int(&cd, "folder_item_index", 3) // 4th folder item of the 4th file
proc_handler_call(ph, "select_index", &cd);
calldata_free(&cd);
```
`media_index` - the 0-based index of the file in the playlist

`folder_item_index` - the 0-based index of the folder item in the folder at `media_index`

If the file at `media_index` is not a folder, the second parameter is ignored.
If `media_index` is higher than the playlist item count, it will be set to 0.
If `folder_item_index` is higher than the folder item count or `media_index`,
it will be set to 0.

## Contact Me
Although there is a Discussion tab in these forums, I would see your message
faster if you ping me (@codeyan) in the [OBS Discord server](https://discord.gg/obsproject),
in #plugins-and-tools. Please do report bugs or if there are features you'd like
to be added.

## Donations
You can	donate to me through 
[PayPal](https://www.paypal.com/donate/?hosted_button_id=S9WJDUDB8CK5S)
to support my development. Thank you!
