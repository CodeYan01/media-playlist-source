# Media Playlist Source

## Introduction

An OBS Plugin that serves as an alternative to VLC Video Source. It uses the
Media Source internally.

## Features

- Allows editing the playlist without restarting the video, even if files are
reordered.
- Saves the currently playing file so it would be played when OBS restarts.
- Allows selecting a file from the list.
- Shuffling (not yet implemented, planned to automatically reshuffle when the
list is exhausted)
- Shows the filename of the current file in the Properties window.

## Limitations

- The filename of the current file can not be updated in the Properties window
as it could cause OBS to crash when the video ends while interacting with the 
Properties window.
- While this plugin works with OBS 28 and up, it requires this change at 
https://github.com/obsproject/obs-studio/pull/8051 that allows this plugin to
save the index of the current file, so that restarting playback is not needed
when editing the list. This change isn't merged yet as of OBS 29.0.2.
- Adding folders and shuffling not yet implemented.

## Contact Me
Although there is a Discussion tab in these forums, I would see your message
faster if you ping me (@CodeYan) in the [OBS Discord server](https://discord.gg/obsproject),
in #plugins-and-tools. Please do report bugs or if there are features you'd like
to be added.

## Donations
You can	donate to me through 
[PayPal](https://www.paypal.com/donate/?hosted_button_id=S9WJDUDB8CK5S)
to support my development. Thank you!