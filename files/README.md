# files

`files` is the graphical file manager. Version 0.2 is laid out like a file explorer: a toolbar with back, forward and up buttons and the path, a sidebar of places on the left, and the current directory filling the rest. You move through the tree from the keyboard or the mouse, and it refreshes when a directory changes. Opening files, file operations and the file picker come in later versions; the plan is in [docs/files-roadmap.md](../docs/files-roadmap.md).

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
| `h` / Left / Backspace, Alt+Up | Up to the parent, with the directory you came from selected |
| Alt+Left or `-`, Alt+Right | Back, forward through the directories visited |
| `g` / Home, `G` / End | First, last entry |
| Ctrl+D, Ctrl+U | Half a page down, up, moving the view with the cursor |
| Page Down, Page Up | A page down, up |
| `b`, F9 | Show or hide the sidebar |
| `<`, `>` | Make the sidebar narrower, wider |
| `=` | Reset the sidebar's width |
| `.` | Show or hide hidden files |
| `~` | Home |
| `q`, Ctrl+Q | Quit |

Movement keys repeat while held, at the compositor's rate. Enter doesn't repeat, so holding it can't dive down the tree.

## Mouse

| Where | Action |
|---|---|
| The sidebar button (left of back) | Shows or hides the sidebar |
| Back, forward, up buttons | As Alt+Left, Alt+Right, Alt+Up; a button is dimmed when there's nowhere to go |
| A piece of the path | Goes to that directory, with the one you came through selected |
| A place in the sidebar | Goes there |
| A row in the list | Selects it; double-click goes into a directory (opening files comes in 0.3) |
| The wheel over the list | Scrolls it, three rows a notch, without moving the selection. Touchpads scroll by distance |
| Mouse side buttons | Back, forward |
| The sidebar's edge | Drag to resize; double-click resets |

After scrolling with the wheel the selection may be off screen; the next key that moves it brings it back into view.

## What it shows

Along the top: the sidebar button, back, forward and up, then the path, cut into pieces you can click (home shows as a house). When the path is too long the pieces on the left are folded into `…`, which goes to the last one folded. At the right end of the path, `read-only` or `root` when that applies (see below). Back and forward remember the last 100 directories; going back skips any that have been deleted since.

### The sidebar

| Place | Where |
|---|---|
| Home | `$HOME` |
| Desktop, Documents, Downloads, Music, Pictures, Videos | From `~/.config/user-dirs.dirs` when it names them, else `~/Desktop` and so on; only the ones that exist |
| File system | `/` |
| Drives | Whatever is mounted under `/run/media`, `/media` or `/mnt`, by its mount point's name |

The place you're in is highlighted. The list is made again whenever a directory loads, so a new `~/Music` shows up, and drives follow `/proc/self/mountinfo`, so one appears as soon as it's mounted and goes when it's unmounted.

The sidebar is 180 pixels wide to begin with. Drag its edge (the pointer turns into a resize arrow) or use `<` and `>`; double-clicking the edge or `=` resets it. It's never narrower than 100 or wider than half the window, and it hides itself when the window is narrower than 60 character cells. Its width and whether it's shown are kept in `~/.local/state/files/state` (or under `$XDG_STATE_HOME`).

### The list

Each row has an icon and the name. Where the list is wide enough, the size (or a directory's entry count) and the date and time (`2026-11-28 14:03`) are on the right.

- Directories come first, then files, in natural order (`file2` before `file10`) and case-insensitively. Letters fold by ASCII case only; other characters compare by code point.
- Directories are in the palette's accent color and hidden entries are muted. Symlinks are in italic and broken links in the urgent color, marked `broken`. A directory that can't be read counts as `?`.
- A name that doesn't fit is cut in the middle for files, keeping the extension (`a-very-long-na…me.tar.gz`), and at the end for directories.
- Names that aren't valid UTF-8 show `�` for the bad bytes, but are kept as they are on disk, so they can still be entered.
- Colors come from wall's palette (`~/.cache/wall/colors.conf`) and change with the wallpaper. Without a palette files uses its own dark colors.
- The window title is the current path, with `~` for home.

### Protected places

files will only change things inside your home directory: that's the rule every file operation follows once they arrive in 0.4, so a slip can't delete the system. A directory counts as home by where it really is, so a symlink in home pointing at `/etc` is still protected. Running files as root lifts the rule. Outside home the end of the path shows a lock and `read-only`; as root it shows `root` in red.

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

## Manual checks (0.2)

- [x] `/etc` shows `read-only` at the end of the path; home doesn't.
- [x] A long path folds its start into `…`.
- [ ] The sidebar button, back, forward, up and the path pieces work by clicking; the side buttons go back and forward.
- [ ] Clicking a place goes there; clicking a row selects it, double-clicking enters, and the wheel scrolls the list.
- [ ] Dragging the sidebar's edge resizes it with a resize arrow, and the width and visibility come back after a restart.
- [ ] A mounted USB stick shows under Drives and goes when it's unmounted.
- [ ] Narrowing the window hides the sidebar.
