# Boosty Download Queue UI Design

## Scope

This change aligns Boosty Downloader behavior with the donor where it fits, while keeping Boosty-specific download semantics:

- Settings sidebar collapses like YouTube Downloader.
- URL input remains a single-link input.
- Paste handles one link differently from link lists.
- TXT drag-and-drop accepts plain line lists and Python-style string lists.
- Queue tasks show post title and post thumbnail on the left.
- A Boosty post with multiple videos downloads all videos sequentially inside one task.

## Settings Sidebar

The settings dialog uses donor-style sidebar geometry:

- Expanded width at normal dialog widths.
- Collapsed width below the donor breakpoint or when toggled manually.
- Collapsed navigation shows compact labels/icons.
- Content rect is computed from the current sidebar width, so panels do not overlap.

Keep this as a local layout change in `Application.cpp`; do not introduce a new settings framework.

## Input, Paste, and Hotkeys

The URL input is for one Boosty post link.

- `Enter` starts the current single-link download.
- `Ctrl+V` replaces the input text when clipboard contains one URL.
- `Ctrl+V` immediately enqueues downloads when clipboard contains multiple URLs or a Python-style list of strings.
- The `Вставить` button uses the same behavior as `Ctrl+V`.
- Existing drag-and-drop TXT import uses the same parser.

The shared parser returns only URL strings. It supports:

- plain newline-separated URLs;
- Python-style lists such as `["url1", "url2"]`;
- optional commas and quotes around list items.

## Post Titles, Filenames, and Multiple Videos

Filenames use the Boosty post title, not the nested `ok_video.title`.

For one video:

- `<sanitized post title> [post-id].<ext>`

For multiple videos in one post:

- `<sanitized post title> 01 [post-id].<ext>`
- `<sanitized post title> 02 [post-id].<ext>`

One Boosty post URL creates one queue task. If the post has multiple videos, the task downloads them sequentially. Progress resets for each video, then conversion runs for that video if needed, then the next video begins.

## Queue Preview

No preview is added near the URL input.

Queue rows show a thumbnail on the left when Boosty API provides one for the post. If no thumbnail is available, the row falls back to the current text-only layout or a simple placeholder area. The task title uses the post title once metadata is known.

## Testing

Add focused self-test coverage for:

- Python-style list parsing.
- Single URL parsing still works.
- Filename base uses post title.
- Multiple videos from one post produce ordered target filenames.

Manual verification covers:

- `Ctrl+V` one URL replaces input.
- `Ctrl+V` multiple URLs enqueues immediately.
- `Enter` starts single-link download.
- Settings sidebar collapse at narrow width and manual toggle.
- Queue row thumbnail/title rendering.
