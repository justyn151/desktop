# files

`files` is the graphical file manager. Version 0.2 shows Miller columns: the parent directory, the current one and a preview of the selected entry, with a status line underneath. You move through the tree from the keyboard, and it refreshes when a directory changes. Opening files, file operations and the file picker come in later versions; the plan is in [docs/files-roadmap.md](../docs/files-roadmap.md).

It's a normal xdg-shell window with app ID `files`, so dwl tiles it like any other client. It draws on the CPU into shm buffers at the output's physical resolution through `wp-viewporter` and `wp-fractional-scale-v1`, so text stays sharp at fractional scales. It's built on [desk](../desk/README.md): the window, keyboard, key repeat, fonts, image decoding, palette and file watch.

## Build

On Arch Linux the build needs `gcc`, `make`, `pkgconf`, `wayland`, `wayland-protocols`, `libxkbcommon`, `freetype2`, `fontconfig`, `libpng`, `libjpeg-turbo` and `libwebp`. GIF decoding is built in.

```sh
make
make install      # /usr/local/bin/files; honors PREFIX, BINDIR and DESTDIR
```

Icons are glyphs from Symbols Nerd Font, found through fontconfig's fallback for `sans-serif`, so no icon theme is needed. Without that font they show as boxes.

## Usage

```text
files [DIR | FILE]
```

| Option | Meaning |
|---|---|
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

With no argument files opens the home directory. A `FILE` opens its directory with the file selected.

| Exit status | Meaning |
|---|---|
| 0 | Closed normally |
| 1 | Couldn't open the directory or the display |
| 2 | Usage error |

## Keys

| Key | Action |
|---|---|
| `j` / Down, `k` / Up | Next, previous entry |
| `l` / Right / Enter | Into the directory. On a file nothing happens yet (opening comes in 0.3) |
| `h` / Left / Backspace | Up to the parent, with the directory you came from selected |
| `g` / Home, `G` / End | First, last entry |
| Ctrl+D, Ctrl+U | Half a page down, up, moving the view with the cursor |
| Page Down, Page Up | A page down, up |
| `J`, `K` | Scroll the preview down, up |
| `z` | Hide or show the preview column |
| `.` | Show or hide hidden files |
| `~` | Home |
| `q`, Ctrl+Q | Quit |

Movement and preview scrolling keys repeat while held, at the compositor's rate. Enter doesn't repeat, so holding it can't dive down the tree.

## What it shows

The window has three columns, about 1 : 2 : 3 of its width: the parent directory with the current one highlighted, the current directory, and a preview of the selected entry. When the window is narrower than 60 character cells the parent column goes, and under 40 the preview goes too.

Each row has an icon and the name. Where the current column is wide enough, the size (or a directory's entry count) and the date and time (`2026-11-28 14:03`) are on the right. They're always in the status line.

- Directories come first, then files, in natural order (`file2` before `file10`) and case-insensitively. Letters fold by ASCII case only; other characters compare by code point.
- Directories are in the palette's accent color and hidden entries are muted. Symlinks are in italic and broken links in the urgent color, marked `broken`. A directory that can't be read counts as `?`.
- A name that doesn't fit is cut in the middle for files, keeping the extension (`a-very-long-na…me.tar.gz`), and at the end for directories.
- Names that aren't valid UTF-8 show `�` for the bad bytes, but are kept as they are on disk, so they can still be entered.
- Colors come from wall's palette (`~/.cache/wall/colors.conf`) and change with the wallpaper. Without a palette files uses its own dark colors.
- The window title is the current path, with `~` for home.

### Previews

| Selected | Preview |
|---|---|
| A directory | Its listing, with the entry you last selected there highlighted |
| A PNG, JPEG, GIF or WebP image | The image shrunk to fit the column, turned upright by its EXIF orientation |
| A text file | The first 32 KiB in monospace, tabs expanded to 4 columns, from the top |
| Anything else | Its type, size, permissions, owner and modified time |
| A symlink | `→ target` above the preview of what it points to; a broken link says so |

A file counts as text when its first 4 KiB have no NUL byte and almost no invalid UTF-8. Named pipes, sockets and devices are never opened: they show their details.

Previews are made on two worker threads, one for images and one for directories and text, so moving never waits for them, and a slow image doesn't hold up a directory. A preview appears when it's ready. Images are decoded into at most 64 MiB, so a larger JPEG is decoded at reduced size; a larger PNG shows an error. The last 16 previews are cached by path, modified time and size, so going back is instant and a changed file is previewed again.

### Status line

On the left: the selected entry's permissions, owner and group, size (or entry count), modified time, and for a symlink `→ target`. On the right: the position in the list (`3/15`) and the free space on the current filesystem.

## Behaviour

- Directories are read with `openat` and `fstatat` relative to the directory, so long paths and renames while reading can't confuse it. Symlinks to directories are followed by name, so the path shows the link and `h` returns to where the link was.
- The selection is remembered per directory for the session, so `h` then `l` comes back to the same entry.
- The current directory is watched with inotify. Changes are gathered for 100 ms and then the directory is read again, keeping the selection on the same name.
- If the current directory is deleted, moved or unmounted, files moves to the nearest parent that still exists.
- A directory without read permission shows the error in place of the entries; `h` still works.
- The parent directory is watched too, so its column stays current.
- Only the rows on screen are drawn. A directory of 100,000 entries takes about 0.1 s to read and sort, and redrawing a 4K frame takes about 3 ms once the glyphs are cached.

## Manual checks (0.1)

- [ ] `files ~/Projects/desktop` opens with directories first and the selection visible; holding `j` scrolls smoothly.
- [ ] `h` and `l` move up and down the tree and the selection comes back to where it was.
- [ ] Text is sharp at 1× and at a fractional scale; Nerd Font icons line up with the text. Checked at 1× only.
- [x] `touch ~/Projects/desktop/new` in a terminal shows up in an open window within a second.
- [x] A `FILE` argument opens its directory with it selected.
- [x] Deleting the open directory moves the window to the nearest existing parent.
- [x] Invalid UTF-8 names, broken links and long names draw as described above.

## Manual checks (0.2)

- [ ] Moving through a directory of photos keeps up with key repeat; previews appear as they're decoded and never block movement.
- [x] A text file's preview shows its start; a binary file shows its details.
- [ ] Narrowing the window drops the parent column, then the preview.
- [x] Directory, image, symlink, named pipe and empty-file previews draw as described above.
