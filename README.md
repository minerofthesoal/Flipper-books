# Flipper Books

An EPUB reader for the Flipper Zero.

Load `.epub` files from your phone or computer onto the SD card, then read them
on the device with bookmarks, table of contents, search, night mode,
auto-scroll, and battery-friendly power modes.

## Features

### Format
- **fbook2 format** with cover thumbnail, per-chapter word counts, language
  metadata, longer chapter titles (48 chars), images up to 128x64, and
  separate per-image format byte for future grayscale support. The reader
  accepts both v1 and v2; the converter emits v2 by default (`--v1` for
  legacy).
- EPUB support via the Python converter.
- `.txt` support directly on device.

### Library
- **Cover thumbnails** in a custom library view with **inline progress bars**
  next to each title.
- **Sort modes**: Name / Recent / Progress / Favorites first.
- **Favorite toggle** per book, displayed as a star next to the cover.
- **Mark Finished** (manual or auto when reaching the end).

### Reader
- Word-wrap pagination, resume-from-last-page.
- **Text size** (Tiny / Small / Medium / Large).
- **Line spacing** (Tight / Normal / Loose / Double).
- **Font family** (Auto / Serif / Sans).
- **Margin** (None / Normal / Wide).
- **Page number / percentage overlay** (top-right).
- **Working slide & fade animations** that show both old and new page during
  the transition.
- **Night mode** (true black background with white text).
- **Toggleable images** (1bpp, LSB-first XBM order so they render correctly).
- **Chapter prev/next** on long-press Left / Right.
- **Sleep timer** (5/10/15/30/60 min, returns to library on expiry).
- **Auto-scroll** with adjustable speed.
- **Bookmarks** (up to 8 per book).
- **Table of Contents** with chapter titles and page numbers.
- **Full-text search** with "search next".
- **Go to %...** picker (0-100%).
- Justify / hyphenation toggles.
- Vibrate on page turn.

### Stats
- **Reading speed** (words/minute) computed from accumulated reading time.
- **Estimated time remaining** for the current book based on WPM and the
  unread word count.
- Total time read, pages read, books opened, books finished.

Per-book progress is saved automatically on every page turn and on exit.

## Controls (in reader)

| Key             | Action                                |
| --------------- | ------------------------------------- |
| `Left`          | Previous page                         |
| `Right`         | Next page                             |
| `Long Left`     | Previous chapter                      |
| `Long Right`    | Next chapter                          |
| `Up`            | Add bookmark at current position      |
| `Down`          | Open table of contents                |
| `OK`            | Open in-book menu (bookmarks/search)  |
| `Back`          | Save progress and return to library   |

## Loading books

1. Connect the Flipper to your computer or use qFlipper / the mobile app.
2. Copy your book into `/ext/apps_data/books/library/`.
3. Either:
   - Drop a `.epub` file in, **after** running the converter once (the device
     keeps the converted sidecar in `/ext/apps_data/books/cache/`), or
   - Run `python3 tools/epub_to_fbook.py my-book.epub` on your computer and
     copy the resulting `.fbook` onto the Flipper's `apps_data/books/library`.
   - Or drop a plain `.txt` file (no conversion needed).

## Converter

```bash
# Requires Python 3.8+ and Pillow for image support
pip install Pillow
python3 tools/epub_to_fbook.py my-book.epub -o my-book.fbook
```

Flags:

- `--no-images` - skip image extraction (smaller files, faster conversion).
- `-o PATH` - custom output path.

### Regenerating older .fbook files

If you have `.fbook` files made before this version of the converter
they may be empty (text body length = 0) or have images packed in the
wrong byte order. Re-run the converter on the original `.epub` and
delete the stale entries from `/ext/apps_data/books/cache/` so the
new copies are used.

## Building

Uses [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) with the
Momentum SDK:

```bash
pip install ufbt
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel=release
ufbt
```

The built `.fap` lands in `dist/`.

## CI

A single GitHub Actions workflow (`.github/workflows/build-and-release.yml`)
builds the FAP on every push and publishes a GitHub Release whenever a tag
starting with `v` is pushed (or manually via the `workflow_dispatch` input).
