# files roadmap

Implementation plan for files, the graphical file manager, from 0.1 to 1.0. It expands `files` in [Phase 5 of PLAN.md](../PLAN.md#phase-5-remaining-core-apps), pulled forward after view and mark.

## Starting point

files is one `files.c` and one `files.h`, built on `desk/` from its first commit, the same way view and shot were. It's a normal xdg-shell window (app ID `files`), so dwl tiles it like any other client.

The model is Miller columns, as in ranger or lf but drawn as a real window: the parent directory on the left, the current one in the middle, a preview of the selected entry on the right. `h` goes up, `l` goes in (or opens a file), `j` and `k` move. Everything works from the keyboard first; the mouse works too since 0.2, and right-click and drag and drop come in 0.5.

files has two jobs. It's the file manager, and it's also the file picker for every other application: the dialog a browser opens to upload a file or ask where a download goes, and any "Open…" or "Save as…" dialog. The picker is the same window and keys with a choose bar added (0.6). Applications reach it through the `org.freedesktop.impl.portal.FileChooser` portal backend, so there's one way to move around the filesystem instead of a GTK dialog with keys of its own.

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
- **The portal frontend.** `xdg-desktop-portal` isn't installed. Applications talk to it, and it forwards file dialogs to files (0.6). It's needed for 0.6 and later for screen sharing (`xdg-desktop-portal-wlr`, PLAN Phase 6). `XDG_CURRENT_DESKTOP` isn't set either, and the portal needs it to pick its config.
- **`~/.config/user-dirs.dirs`.** Doesn't exist, so there's no configured download directory. The picker falls back to `~/Downloads` (0.6). `xdg-user-dirs-update` would write the file.

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
- A toolbar at the top (replacing the status line 0.2 first had): back, forward and up buttons, and the path as pieces to click, with `read-only` or `root` at its end. Back and forward keep the last 100 directories (Alt+Left, Alt+Right, `-`, the mouse's side buttons).
- The mouse came forward from 0.5: click selects, double-click enters a directory, a click in the parent or preview column goes there, and the wheel scrolls the column under the pointer without moving the selection.

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

- Copy, move and delete run in a worker thread with a progress line under the toolbar (files, bytes, current name) and can be cancelled. Moves within a filesystem are `renameat2`; across filesystems, copy then remove. Copies use `copy_file_range` and keep modes and times. Symlinks are copied as links.
- Name conflicts ask once per operation: replace, skip, rename (`name (2).ext`), with "apply to all".
- The trash follows the freedesktop spec (`~/.local/share/Trash/files` and `info`, `$topdir/.Trash-$uid` on other filesystems), so other tools see the same trash. `T` opens the trash as a directory, where `p` restores.
- The clipboard is shared with other applications as `text/uri-list` and `x-special/gnome-copied-files` through `wl_data_device`, so a copy in files can be pasted in a browser's upload dialog or another file manager. Pasting `text/uri-list` from elsewhere copies those files in.
- Nothing is ever overwritten without asking, and a failed operation reports which file and why.

### Protected places

So a slip can't delete the system, files only changes things inside the user's own home directory. This is on top of the kernel's permissions: it also covers files the user happens to own elsewhere, such as in `/tmp` or on a mounted drive.

- Every operation asks `may_modify` (in files.c since 0.2) of each directory whose entries it changes: the destination of a copy, move, paste or new file, and the source directory of a move, rename or trash. The directory is judged by where it really is (`realpath`), so a symlink inside home that points at `/etc` doesn't count as home.
- Outside home the operation is refused before anything happens, with "/etc is protected: files only changes things inside your home directory". Copying *from* anywhere into home is fine.
- Running files as root (`sudo files`, effective uid 0) lifts the rule. The end of the path then shows `root` in the urgent color, so it's never forgotten.
- Recursive operations never follow symlinks and never cross onto another filesystem, like `rm --one-file-system`, so a bind mount or a link to `/` inside a directory being deleted can't take the system with it.
- More writable places, such as a USB drive under `/run/media/$USER`, are an explicit opt-in in the config (0.5, `[safety] writable`). Home itself can't be removed from the list.
- The end of the path already shows a lock and `read-only` outside home (0.2).

### Manual checks

- Copying a large directory from a USB stick into home shows progress and can be cancelled midway, leaving no half-copied file.
- Delete, Shift+Delete, paste and rename in `/etc` or `/tmp` are refused with a message naming the protected directory; run as root, they work and the path bar says `root`.
- Deleting a directory in home that contains a symlink to `/` removes the link, not what it points to.
- Renaming, moving and trashing can each be undone with `u`.
- A file trashed in files appears in another tool's trash view, and restoring it puts it back.

---

## 0.5: Mouse, thumbnails, config and desktop integration

- Mouse: since 0.2 the dividers drag, clicks select, double-clicks enter directories, the wheel scrolls, and the toolbar's buttons and path work. Left for 0.5: double-click opens files (through 0.3's opener), right-click opens the "Open with" list. Drag and drop out of files (`text/uri-list`) and into it.
- A grid view (`t`) of thumbnails for image directories. Thumbnails follow the freedesktop thumbnail spec (`~/.cache/thumbnails/normal` and `large`, PNG with `Thumb::URI` and `Thumb::MTime`), so other applications can share them. wall's browser keeps a private cache of its own (`~/.cache/wall/thumbnails`); moving it onto the standard one is optional and would let the two share.
- Config through desk at `~/.config/files/files.conf`, written from a sample on first run:

  ```ini
  [layout]
  # Column widths as parent : current : preview, until they're dragged.
  # Dragged widths are kept in ~/.local/state/files/state.
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

  [safety]
  # Places outside home that files may change, besides home itself.
  # Root may change anything regardless.
  writable =

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

## 0.6: File picker

files becomes the file dialog for other applications: choosing files to upload in a browser, choosing where a download goes, and any "Open…" or "Save as…" dialog. There are two parts. `files --pick` is a picker usable from the command line and scripts. `files --portal` is the portal backend that opens that picker for applications.

This comes after 0.5 because a picker needs the earlier milestones: filters and MIME types from 0.3, making a new folder from 0.4, and the mouse from 0.5, since a browser dialog is often reached with the mouse in hand.

### Command line

```text
files --pick [--multiple] [--directory] [--filter NAME:PATTERNS]... [--title TEXT] [--accept LABEL] [-0] [DIR|FILE]
files --save NAME [--filter NAME:PATTERNS]... [--title TEXT] [--accept LABEL] [DIR]
files --portal
```

- `--pick` chooses one existing file, or several with `--multiple`. With `--directory` it chooses a directory instead.
- `--save NAME` asks for a file name and a directory to save it in, with NAME as the suggested name. Without a DIR it starts in the download directory (`XDG_DOWNLOAD_DIR` from `~/.config/user-dirs.dirs`, else `~/Downloads`), or wherever the last save went.
- `--filter 'Images:*.png;*.jpg;image/webp'` is repeatable. Patterns are globs or MIME types (matched through `desk_mime_type`). The first filter is active, and "All files" is always added last.
- Chosen paths go to stdout, one per line, or NUL-separated with `-0`.
- Exit statuses: 0 chosen, 1 error, 2 usage, 3 cancelled (nothing printed). Cancelling is a separate status so a caller can tell it apart from a failure.

### The picker window

- It's the normal Miller columns, previews, filter (`/`), path entry (`:`), bookmarks (`'`), sorting and hidden files. The only change is a choose bar at the bottom with the title, the active filter, the accept button and, when saving, the name field.
- App ID `files-picker`, floating, with a dwl rule like view's, at about 60% of the output.
- Entries that don't match the active filter are hidden. Directories always show. `Ctrl+F` cycles the filters.
- Only `a` (new file or directory) is available from 0.4's operations, because saving into a new folder is a common need. Cut, paste, rename and trash are off, so a stray key in a dialog can't move anything.

| Key | Action in the picker |
|---|---|
| Enter on a file | Choose it (instead of opening it) |
| Space | With `--multiple`, select and move down; Enter then returns every selected file |
| Enter on a directory | Go into it, as usual |
| Ctrl+Enter | With `--directory`, choose the directory under the cursor, or the current one if the cursor is on a file |
| Ctrl+F | Next filter |
| Tab | When saving, move between the name field and the list |
| Escape | Clear the filter or selection first, then cancel |
| `q`, Ctrl+Q | Cancel |

### Saving

- The name field has focus at the start, with the name minus its extension selected, like `r` in 0.4. Typing replaces it.
- Choosing an existing file in the list copies its name into the field.
- A name with a `/` in it (`~/Documents/report.pdf`) is a path: the directory part goes there.
- Enter saves into the current directory. If the file already exists, files asks "Replace report.pdf?" before returning. It never writes the file itself. The application does that.
- The last directory saved to and the last one opened from are remembered per application in `~/.local/state/files/picker` (keyed by the portal's app ID, or `--app-id` for scripts), so a browser's downloads keep going to the same place.

### Portal backend

- `files --portal` is a small session-bus service named `org.freedesktop.impl.portal.desktop.files`. It implements `org.freedesktop.impl.portal.FileChooser` (`OpenFile`, `SaveFile`, `SaveFiles`) at `/org/freedesktop/portal/desktop`, through sd-bus (libsystemd, already used by bar and net).
- Each request runs `files --pick` or `files --save` as a child process and replies when that process exits. A crash in a dialog never takes the service down. The service keeps the pending call and doesn't block, so several dialogs can be open at once.
- Option mapping:
  - `accept_label` → `--accept`
  - `multiple`, `directory` → the same flags
  - `filters` (`a(sa(us))`, type 0 is a glob, 1 a MIME type) and `current_filter` → `--filter`, with the current one first
  - `current_name`, `current_folder` (bytes, NUL-terminated) and `current_file` (select it) → the name and starting directory
  - `SaveFiles` → `--directory` with the accept label "Save here", plus a check that names the given files that already exist there
  - `choices` (the extra combo boxes and checkboxes some applications add) are returned at their defaults in 0.6. Drawing them in the choose bar can come later if an application turns out to need them
- Results are `uris` (`file://`, percent-encoded from the raw bytes, so non-UTF-8 names survive) and `current_filter`. Response 0 means chosen, 1 cancelled, 2 failed.
- Each request exports an `org.freedesktop.impl.portal.Request` object at its handle. `Close` kills the child process, so a dialog the application gives up on goes away.
- The parent window arrives as `wayland:HANDLE` from xdg-foreign. dwl doesn't expose `zxdg_exporter_v2` yet, although wlroots 0.20 ships `wlr_xdg_foreign_v2`. Adding it to dwl lets the picker set the browser as its parent and float centered over it. Without it, the float rule still works and the handle is ignored.
- The service exits after a minute with no open dialogs. D-Bus activation starts it again on the next request.

### Installing

`make portal` installs:

- `/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.files.service` with `Exec=/usr/local/bin/files --portal`
- `/usr/share/xdg-desktop-portal/portals/files.portal`:

  ```ini
  [portal]
  DBusName=org.freedesktop.impl.portal.desktop.files
  Interfaces=org.freedesktop.impl.portal.FileChooser
  ```

- `~/.config/xdg-desktop-portal/dwl-portals.conf`, which sends file dialogs to files and leaves the rest to whatever is installed:

  ```ini
  [preferred]
  default=wlr
  org.freedesktop.impl.portal.FileChooser=files
  ```

On the system side, install `xdg-desktop-portal`, export `XDG_CURRENT_DESKTOP=dwl` before dwl starts, and pass `WAYLAND_DISPLAY` and `XDG_CURRENT_DESKTOP` to D-Bus activation from dwl's startup (`dbus-update-activation-environment` or `systemctl --user import-environment`).

How applications pick up the portal (to confirm against the installed versions):

- Flatpak apps use it on their own.
- Firefox needs `widget.use-xdg-desktop-portal.file-picker = 1` in `about:config`.
- Chromium-based browsers and Electron apps use it when the portal is running.
- Other GTK apps need `GTK_USE_PORTAL=1` (GTK 3) or `GDK_DEBUG=portals` (GTK 4).
- Qt apps need `QT_QPA_PLATFORMTHEME=xdgdesktopportal`.

### Manual checks

- `files --pick --multiple ~/Pictures` prints the chosen paths. Escape prints nothing and exits 3.
- `files --save report.pdf` starts in the download directory with "report" selected. Saving over an existing file asks first.
- `files --pick --filter 'Images:*.png;*.jpg'` hides other files, and Ctrl+F switches to "All files".
- A browser's upload button opens the files picker. Choosing two files uploads both.
- A download with "always ask where to save" opens save mode in `~/Downloads`. The file lands where it was saved, and the next download starts there.
- Cancelling the dialog from the browser side (closing the tab) closes the picker window.
- Killing a picker process makes the browser see a failed dialog, not a hang. The service keeps serving.

---

## 1.0: Daily driver

- Every 0.x checklist passes on dwl.
- A week as the only file manager, with annoyances logged in `docs/files-papercuts.md`. This includes a week as the only file picker: every upload and download goes through it.
- Fuzz the MIME sniffing, desktop entry and `mimeapps.list` parsers, the trash info parser, and the portal's option decoding (filters, folders as bytes) under ASan/UBSan.
- A stress test of the operations: copying trees with symlinks, special files, permission errors and name conflicts, compared with `cp -a`.
- README documents every key, option and setting.
- Tag `files-1.0.0`.

## After 1.0 (unscheduled)

- The portal's `choices` (extra combo boxes and checkboxes) drawn in the choose bar, if an application needs them.
- Removable drives in a sidebar, through `mount` (#15) once it exists.
- Archives: browse `.zip` and `.tar.*` as directories through libarchive, which is installed.
- Tabs or split panes, if Miller columns turn out not to be enough.
- PDF and video previews, once `read` and `video` exist to borrow from.
- Bulk rename in `editor`: the names as lines of a file, renamed to whatever they are when it's saved.

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.1 | One directory, keyboard navigation, live refresh; desk gains the font layer, keyboard and window, view moves onto them | Done (key checks and fractional scale pending) |
| 0.2 | Miller columns, previews of directories, images and text, toolbar with back, forward and the path, mouse | Done (live key and mouse checks pending) |
| 0.3 | Opening with the default application, Open with, filter, path entry, bookmarks, sorting | Not started |
| 0.4 | Selection, copy, cut, paste, rename, new, trash, undo, progress, shared clipboard | Not started |
| 0.5 | Right-click, drag and drop, thumbnail grid, config, palette, desktop entry | Not started |
| 0.6 | File picker: `--pick` and `--save`, the FileChooser portal backend for browsers and other apps | Not started |
| 1.0 | Checklists, a week of use, fuzzing, README, tag | Final |
