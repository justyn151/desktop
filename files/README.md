# files

`files` is the graphical file manager. Version 0.1 shows one directory and moves through the tree from the keyboard, refreshing when the directory changes. Miller columns, previews, opening files, file operations and the file picker come in later versions; the plan is in [docs/files-roadmap.md](../docs/files-roadmap.md).

It's a normal xdg-shell window with app ID `files`, so dwl tiles it like any other client. It draws on the CPU into shm buffers at the output's physical resolution through `wp-viewporter` and `wp-fractional-scale-v1`, so text stays sharp at fractional scales. It's built on [desk](../desk/README.md): the window, keyboard, key repeat, fonts, palette and file watch.

## Build

On Arch Linux the build needs `gcc`, `make`, `pkgconf`, `wayland`, `wayland-protocols`, `libxkbcommon`, `freetype2` and `fontconfig`.

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
| `.` | Show or hide hidden files |
| `~` | Home |
| `q`, Ctrl+Q | Quit |

Movement keys repeat while held, at the compositor's rate. Enter doesn't repeat, so holding it can't dive down the tree.

## What it shows

- Directories first, then files, in natural order (`file2` before `file10`) and case-insensitively. Letters fold by ASCII case only; other characters compare by code point.
- Each row has an icon, the name, and on the right the size (or a directory's entry count) and the modified time. The time is shown as `14:03` for today, `28 Nov` for this year, and `2024-03-01` before that. The details are hidden when the window is too narrow for them.
- Directories are in the palette's accent color and hidden entries are muted. Symlinks are in italic and broken links in the urgent color, marked `broken`. A directory that can't be read counts as `?`.
- A name that doesn't fit is cut in the middle for files, keeping the extension (`a-very-long-na…me.tar.gz`), and at the end for directories.
- Names that aren't valid UTF-8 show `�` for the bad bytes, but are kept as they are on disk, so they can still be entered.
- Colors come from wall's palette (`~/.cache/wall/colors.conf`) and change with the wallpaper. Without a palette files uses its own dark colors.
- The window title is the current path, with `~` for home.

## Behaviour

- Directories are read with `openat` and `fstatat` relative to the directory, so long paths and renames while reading can't confuse it. Symlinks to directories are followed by name, so the path shows the link and `h` returns to where the link was.
- The selection is remembered per directory for the session, so `h` then `l` comes back to the same entry.
- The current directory is watched with inotify. Changes are gathered for 100 ms and then the directory is read again, keeping the selection on the same name.
- If the current directory is deleted, moved or unmounted, files moves to the nearest parent that still exists.
- A directory without read permission shows the error in place of the entries; `h` still works.
- Only the rows on screen are drawn. A directory of 100,000 entries takes about 0.1 s to read and sort, and redrawing a 4K frame takes about 3 ms once the glyphs are cached.

## Manual checks (0.1)

- [ ] `files ~/Projects/desktop` opens with directories first and the selection visible; holding `j` scrolls smoothly.
- [ ] `h` and `l` move up and down the tree and the selection comes back to where it was.
- [ ] Text is sharp at 1× and at a fractional scale; Nerd Font icons line up with the text. Checked at 1× only.
- [x] `touch ~/Projects/desktop/new` in a terminal shows up in an open window within a second.
- [x] A `FILE` argument opens its directory with it selected.
- [x] Deleting the open directory moves the window to the nearest existing parent.
- [x] Invalid UTF-8 names, broken links and long names draw as described above.
