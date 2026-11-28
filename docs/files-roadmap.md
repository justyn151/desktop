# files roadmap

Implementation plan for files, the graphical file manager, from 0.1 to 1.0. It expands `files` in [Phase 5 of PLAN.md](../PLAN.md#phase-5-remaining-core-apps), pulled forward after view and mark.

## Starting point

files is one `files.c` and one `files.h`, built on `desk/` from its first commit, the same way view and shot were. It's a normal xdg-shell window (app ID `files`), so dwl tiles it like any other client.

The model is Miller columns, as in ranger or lf but drawn as a real window: the parent directory on the left, the current one in the middle, a preview of the selected entry on the right. `h` goes up, `l` goes in (or opens a file), `j` and `k` move. Everything works from the keyboard first; the mouse comes along in 0.5.

### What already exists

| Piece | Where | files uses it for |
|---|---|---|
| xdg-shell window, shm buffers, fractional scale through `wp_viewporter` | view (`desk_buffer_get`, `-DDESK_WAYLAND`) | Every frame (0.1) |
| xkbcommon keymap and modifiers | view, run, deal (each its own copy) | Keys (0.1) |
| Key repeat | `DeskRepeat` | Holding `j` (0.1) |
| Fontconfig matching and FreeType rendering into ARGB | bar, run, wall (each its own copy) | Every line of text (0.1) |
| Image decoding | `desk_image_load` (`-DDESK_IMAGE`) | Image previews (0.2), thumbnails (0.5) |
| Desktop entry parsing (`Exec`, `TryExec`, `MimeType`, field codes) | run | Opening files with the right application (0.3) |
| Markdown layout | mark | A markdown preview, if it's worth sharing (0.2, optional) |
| INI config, palette, file watch | desk | Config, colors, refreshing a directory that changes (0.1 onward) |
| Atomic files | `desk_atomic_*` | Writing `mimeapps.list` and bookmarks (0.3) |

dwl already supports everything files needs: xdg-shell, viewporter, fractional scale and cursor-shape.

### What's missing on the system

- **An icon theme.** Only `hicolor` is installed. files draws its icons as glyphs from Symbols Nerd Font Mono, which is installed and already the fontconfig fallback: a folder, a file, and a few dozen type icons (image, video, audio, archive, code, PDF, markdown), colored from the palette. No icon theme package, no SVG renderer.
- **MIME data.** Neither `shared-mime-info` nor `xdg-utils` is installed. files needs a MIME type to pick an application. Plan: read `/usr/share/mime/globs2` (from `shared-mime-info`, a small package worth installing) when it exists, with a built-in table of common extensions as the fallback, plus content sniffing for files without an extension (the image signatures desk already knows, `#!` scripts, UTF-8 text).
- **`xdg-open`.** files doesn't need it: it resolves `mimeapps.list` and desktop entries itself (0.3). Installing `xdg-utils` is still worth it for mark's and other apps' links.

### What desk gains

| desk piece | Layer | Needed by | Phase 3 step it pre-empts |
|---|---|---|---|
| Font loading (fontconfig match with fallback faces), glyph cache, measuring, drawing into ARGB buffers with clipping | 2, new (`-DDESK_FONT`: freetype2, fontconfig) | 0.1 | Step 5. files is the fourth copy otherwise |
| xkb keymap and modifier state from `wl_keyboard` | 1 | 0.1 | Step 6. view moves onto it in the same change |
| A plain xdg-shell window with scale tracking (`DeskWindow`: surface, toplevel, viewport, fractional scale, configure and close) | 1 | 0.1 | Step 4, for toplevels. view moves onto it |
| Desktop entries and `mimeapps.list` lookup (`desk_app_for_type`, `desk_app_exec` with field codes and a detached spawn) | 0 | 0.3 | None yet. run moves onto the parser later |
| MIME type of a file (`desk_mime_type`: globs2, built-in table, sniffing) | 0 | 0.3 | None |

Moving view onto the keyboard and window pieces happens in the same milestone that adds them, so there are never two copies. bar, run and wall move to the font layer in Phase 3 step 5, not here.

### Drawing

- One list row per entry: icon glyph, name, and right-aligned details (size for files, entry count for directories, modified date), in the UI font (`sans-serif`, which is Noto Sans), sizes in `monospace` digits so they line up.
- The selection is a full-width bar in the palette's selection colors. Directories in the accent color, hidden entries muted, symlinks in italic with their target in the preview, broken links in the urgent color.
- Every frame is drawn on the CPU into the shm buffer, like view. Scrolling a long directory redraws only the list columns; text is cached per glyph, so a full redraw at 4K stays well under a frame.
- Names that don't fit are cut with `…` in the middle for files (`a-very-long-na…me.tar.gz` keeps the extension) and at the end for directories.

---

## 0.1: One directory, keyboard only

A single column: the current directory, navigable from the keyboard. The point of 0.1 is the window, text and keys, done properly and shared through desk.

### Command line

```text
files [DIR]
```

- With no argument, the home directory. A file argument opens its directory with the file selected.
- Exit statuses: 0 closed normally, 1 couldn't open the directory or the display, 2 usage.

### Controls

| Key | Action |
|---|---|
| `j` / Down, `k` / Up | Next, previous entry (with key repeat) |
| `l` / Right / Enter | Into the directory. Files do nothing until 0.3 |
| `h` / Left / Backspace | Up to the parent, with the directory you came from selected |
| `g` / Home, `G` / End | First, last entry |
| Ctrl+D, Ctrl+U | Half a page down, up |
| `.` | Show or hide hidden files |
| `~` | Home |
| `q`, Ctrl+Q | Quit |

### Work

- desk's font layer, keyboard and window pieces (above), with view moved onto the last two.
- Read a directory with `openat`/`fstatat` relative to the directory fd, so a long path or a rename mid-listing can't confuse it. Sort directories first, then names in natural order (`file2` before `file10`), case-insensitively by Unicode code point for now.
- Remember the selection per directory for the session, so `h` then `l` returns to the same entry.
- The window title is the path, with `~` for home, so bar shows it.
- Watch the current directory with inotify (one `desk_watch`-like fd on the directory itself) and refresh it, keeping the selection on the same name.

### Edge cases

- Directories without read permission show a message in the list instead of entries; `h` still works.
- Names that aren't valid UTF-8 are shown with `�` but kept as bytes, so they can still be opened and renamed.
- A directory with 100,000 entries opens without a visible pause: read and sort once, draw only the visible rows.
- The current directory being deleted or unmounted moves to the nearest parent that still exists.

### Manual checks

- `files ~/Projects/desktop` opens with directories first and the selection visible; holding `j` scrolls smoothly.
- `h` and `l` move up and down the tree and the selection comes back to where it was.
- Text is sharp at 1× and at a fractional scale; Nerd Font icons line up with the text.
- `touch ~/Projects/desktop/new` in a terminal shows up in an open window within a second.

---

## 0.2: Miller columns and previews

### Layout

- Three columns: parent, current, preview, at roughly 1 : 2 : 3 of the width. With a narrow window (under 60 cells), the parent column drops first, then the preview.
- The preview shows:
  - A directory: its listing, as the column it would become.
  - An image: the image fitted to the column, through `desk_image_load`, decoded on a worker thread so `j` never waits. Large images are decoded at reduced size where the format allows (JPEG scaling), and the last few previews are cached.
  - Text (UTF-8, by sniffing the first 4 KiB): the first screenful in `monospace`, tabs expanded, no highlighting.
  - Markdown: as text in 0.2. Rendering it through mark's layout is a later, optional step, only if mark's code can be shared without copying it.
  - Anything else: its type, size, permissions, owner and modified time.
- A status line at the bottom: the selected entry's permissions, owner, size and modified time on the left, position (`12/340`) and free space on the right.

### Controls

| Key | Action |
|---|---|
| `J`, `K` | Scroll the preview |
| `z` | Toggle the preview column |

### Manual checks

- Moving through a directory of photos keeps up with key repeat; previews appear as they're decoded and never block movement.
- A text file's preview shows its start; a binary file shows its details.
- Narrowing the window drops the parent column, then the preview.

---

## 0.3: Opening, finding and going places

### Opening files

- `l` or Enter on a file opens it with the default application for its MIME type: `mimeapps.list` (`~/.config/mimeapps.list`, then the system lists) and desktop entries from `XDG_DATA_DIRS` and `~/.local/share/applications`. Images go to view, markdown to mark in deal (through `Terminal=true` and the `terminal` command from run's config), text to `editor` in deal.
- `o` opens an "Open with" list of every application that declares the type, plus all others below a separator; choosing one with Shift+Enter also makes it the default (written atomically to `~/.config/mimeapps.list`).
- Executable files are not run on Enter. `x` runs one, in deal when it's a script.
- Applications start detached in their own session, like mark's opener.

### Finding

| Key | Action |
|---|---|
| `/` | Filter the current directory as you type (substring, smart case); Enter keeps the filter, Escape clears it |
| `n`, `N` | With a kept filter, next and previous match instead of hiding the rest |
| `:` or Ctrl+L | Type a path, with Tab completion; `~` expands |
| `'` then a letter | Jump to a bookmark; `m` then a letter sets one (`~/.config/files/bookmarks`) |
| `-` | Back to the previous directory |
| `s` | Sort: name, modified, size, type, cycling; `S` reverses |

### Manual checks

- Enter on a PNG opens view, on a `.md` opens mark in a new deal window, on a `.c` file opens `editor` in deal.
- `o` lists the applications for a type; making one the default sticks after a restart.
- `/` narrows a large directory as you type; `:` with Tab completes paths.

---

## 0.4: File operations

### Selection and clipboard

| Key | Action |
|---|---|
| Space | Select or unselect and move down |
| `v` | Select all, `V` invert, Escape clears |
| `y` | Copy the selection (or the entry under the cursor) |
| `d` | Cut |
| `p` | Paste into the current directory |
| `r` | Rename inline, with the name minus its extension selected |
| `a` | New file or, ending in `/`, a directory, typed inline |
| Delete | Move to the trash |
| Shift+Delete | Delete permanently, after a confirmation that names what goes |
| `u` | Undo the last rename, move or trash |

### Design

- Copy, move and delete run in a worker thread with a progress line in the status bar (files, bytes, current name) and can be cancelled. Moves within a filesystem are `renameat2`; across filesystems, copy then remove. Copies use `copy_file_range` and keep modes and times. Symlinks are copied as links.
- Name conflicts ask once per operation: replace, skip, rename (`name (2).ext`), with "apply to all".
- The trash follows the freedesktop spec (`~/.local/share/Trash/files` and `info`, `$topdir/.Trash-$uid` on other filesystems), so other tools see the same trash. `T` opens the trash as a directory, where `p` restores.
- The clipboard is shared with other applications as `text/uri-list` and `x-special/gnome-copied-files` through `wl_data_device`, so a copy in files can be pasted in a browser's upload dialog or another file manager. Pasting `text/uri-list` from elsewhere copies those files in.
- Nothing is ever overwritten without asking, and a failed operation reports which file and why.

### Manual checks

- Copying a large directory to a USB stick shows progress and can be cancelled midway, leaving no half-copied file.
- Renaming, moving and trashing can each be undone with `u`.
- A file trashed in files appears in another tool's trash view, and restoring it puts it back.

---

## 0.5: Mouse, thumbnails, config and desktop integration

- Mouse: click selects, double-click opens, the wheel scrolls the column under the pointer, click in another column moves there, right-click opens the "Open with" list. Drag and drop out of files (`text/uri-list`) and into it.
- A grid view (`t`) of thumbnails for image directories. Thumbnails follow the freedesktop thumbnail spec (`~/.cache/thumbnails/normal` and `large`, PNG with `Thumb::URI` and `Thumb::MTime`), so other applications can share them. wall's browser keeps a private cache of its own (`~/.cache/wall/thumbnails`); moving it onto the standard one is optional and would let the two share.
- Config through desk at `~/.config/files/files.conf`, written from a sample on first run:

  ```ini
  [layout]
  # Column widths as parent : current : preview.
  columns = 1:2:3
  font_size = 11

  [view]
  hidden = false
  sort = name
  directories_first = true

  [preview]
  enable = true
  # Largest file decoded for an image preview, in MiB.
  image_limit = 64

  [open]
  # How terminal applications are started.
  terminal = deal -e
  ```

- Colors from the palette, followed live through `desk_watch`: background, foreground, accent for directories, muted for hidden entries and details, the selection colors for the cursor, urgent for broken links and errors.
- `files.desktop` with `MimeType=inode/directory;`, and `make default` registering it, so other apps' "show in folder" opens files. A dwl key binding (Super+E) in the README.

### Manual checks

- Double-clicking opens; dragging an image from files into a browser upload works.
- The grid fills in thumbnails once and reuses them on the next visit, from `~/.cache/thumbnails`.
- Changing the wallpaper recolors an open window.

---

## 1.0: Daily driver

- Every 0.x checklist passes on dwl.
- A week as the only file manager, with annoyances logged in `docs/files-papercuts.md`.
- Fuzz the MIME sniffing, desktop entry and `mimeapps.list` parsers, and the trash info parser, under ASan/UBSan.
- A stress test of the operations: copying trees with symlinks, special files, permission errors and name conflicts, compared with `cp -a`.
- README documents every key, option and setting.
- Tag `files-1.0.0`.

## After 1.0 (unscheduled)

- The `org.freedesktop.impl.portal.FileChooser` backend, so browsers and other apps use files as their file dialog (in PLAN's Phase 6, portals).
- Removable drives in a sidebar, through `mount` (#15) once it exists.
- Archives: browse `.zip` and `.tar.*` as directories through libarchive, which is installed.
- Tabs or split panes, if Miller columns turn out not to be enough.
- PDF and video previews, once `read` and `video` exist to borrow from.
- Bulk rename in `editor`: the names as lines of a file, renamed to whatever they are when it's saved.

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.1 | One directory, keyboard navigation, live refresh; desk gains the font layer, keyboard and window, view moves onto them | Not started |
| 0.2 | Miller columns, previews of directories, images and text, status line | Not started |
| 0.3 | Opening with the default application, Open with, filter, path entry, bookmarks, sorting | Not started |
| 0.4 | Selection, copy, cut, paste, rename, new, trash, undo, progress, shared clipboard | Not started |
| 0.5 | Mouse and drag and drop, thumbnail grid, config, palette, desktop entry | Not started |
| 1.0 | Checklists, a week of use, fuzzing, README, tag | Final |
