# files roadmap

Implementation plan for files, the graphical file manager, from 0.1 to 1.0. It expands `files` in [Phase 5 of PLAN.md](../PLAN.md#phase-5-remaining-core-apps), pulled forward after view and mark.

## Starting point

files is one `files.c` and one `files.h`, built on `desk/` from its first commit, the same way view and shot were. It's a normal xdg-shell window (app ID `files`), so dwl tiles it like any other client.

files is a normal graphical file explorer, used with the mouse first: tabs, a toolbar with back, forward, up, the path and a search field, a sidebar of places on the left, and the current directory filling the rest as a list or icons (0.2). Things are done the way other explorers do them: double-click opens, right-click brings up a menu, files are dragged where they should go, and dialogs ask before anything is replaced or deleted. The keyboard has the standard shortcuts (Ctrl+C, Ctrl+V, F2, Delete, Ctrl+L, Alt+Left), not a vocabulary of its own to learn.

0.1 started out keyboard-only, with ranger's `h`, `j`, `k` and `l`. Those letter keys go in 0.3, when typing a letter starts selecting by name as in other explorers.

files has two jobs. It's the file manager, and it's also the file picker for every other application: the dialog a browser opens to upload a file or ask where a download goes, and any "Open…" or "Save as…" dialog. The picker is the same window with Open and Cancel buttons added (0.6). Applications reach it through the `org.freedesktop.impl.portal.FileChooser` portal backend, so there's one way to move around the filesystem instead of a GTK dialog with keys of its own.

### What already exists

| Piece | Where | files uses it for |
|---|---|---|
| xdg-shell window, shm buffers, fractional scale through `wp_viewporter` | view (`desk_buffer_get`, `-DDESK_WAYLAND`) | Every frame (0.1) |
| xkbcommon keymap and modifiers | view, run, deal (each its own copy) | Keys (0.1) |
| Key repeat | `DeskRepeat` | Holding an arrow key (0.1) |
| Fontconfig matching and FreeType rendering into ARGB | bar, run, wall (each its own copy) | Every line of text (0.1) |
| Image decoding | `desk_image_load` (`-DDESK_IMAGE`) | Thumbnails (brought forward into 0.4) |
| Desktop entry parsing (`Exec`, `TryExec`, `MimeType`, field codes) | run | Opening files with the right application (0.3) |
| INI config, palette, file watch | desk | Config, colors, refreshing a directory that changes (0.1 onward) |
| Atomic files | `desk_atomic_*` | Writing `mimeapps.list` and bookmarks (0.3) |

dwl already supports everything files needs: xdg-shell, viewporter, fractional scale and cursor-shape.

### What's missing on the system

- **An icon theme.** Only `hicolor` is installed. files draws its icons as glyphs from Symbols Nerd Font Mono, which is installed and already the fontconfig fallback: a folder, a file, and a few dozen type icons (image, video, audio, archive, code, PDF, markdown), colored from the palette. No icon theme package, no SVG renderer.
- **MIME data.** Neither `shared-mime-info` nor `xdg-utils` is installed. files needs a MIME type to pick an application. Plan: read `/usr/share/mime/globs2` (from `shared-mime-info`, a small package worth installing) when it exists, with a built-in table of common extensions as the fallback, plus content sniffing for files without an extension (the image signatures desk already knows, `#!` scripts, UTF-8 text).
- **`xdg-open`.** files doesn't need it: it resolves `mimeapps.list` and desktop entries itself (0.3). Installing `xdg-utils` is still worth it for mark's and other apps' links.
- **The portal frontend.** `xdg-desktop-portal` isn't installed. Applications talk to it, and it forwards file dialogs to files (0.6). It's needed for 0.6 and later for screen sharing (`xdg-desktop-portal-wlr`, PLAN Phase 6). `XDG_CURRENT_DESKTOP` isn't set either, and the portal needs it to pick its config.
- **`~/.config/user-dirs.dirs`.** Doesn't exist, so there's no configured download directory. The sidebar uses the usual names (`~/Downloads` and so on, 0.2) and the picker falls back to `~/Downloads` (0.6). `xdg-user-dirs-update` would write the file.

### What desk gains

| desk piece | Layer | Needed by | Phase 3 step it pre-empts |
|---|---|---|---|
| Font loading (fontconfig match with fallback faces), glyph cache, measuring, drawing into ARGB buffers with clipping | 2, new (`-DDESK_FONT`: freetype2, fontconfig) | 0.1 | Step 5. files is the fourth copy otherwise |
| xkb keymap and modifier state from `wl_keyboard` | 1 | 0.1 | Step 6. view moves onto it in the same change |
| A plain xdg-shell window with scale tracking (`DeskWindow`: surface, toplevel, viewport, fractional scale, configure and close) | 1 | 0.1 | Step 4, for toplevels. view moves onto it |
| Desktop entries, `mimeapps.list` and launching (`-DDESK_APPS`: `desk_apps_load`, `desk_apps_for`, `desk_app_launch` with field codes and a detached spawn, `desk_mimeapps_set_default`) | 0 | 0.3 (done) | None yet. run moves onto the parser later |
| MIME type of a file (`desk_mime_type`: globs2, built-in table, sniffing) | 0 | 0.3 (done) | None |
| Popup menus (`xdg_popup` with a grab, positioned against the window, closing on a click outside) | 1 | 0.3 (done: `desk_popup_open`) | Step 4, for popups. bar's menus could move onto it |
| A one-line text field (cursor, selection, word movement, clipboard paste), for the path, search, rename and the picker's name | 2 | 0.3 (done: `DeskText` and `desk_text_key`; drawing and the clipboard stay in files) | None. run's prompt could move onto it |

Moving view onto the keyboard and window pieces happens in the same milestone that adds them, so there are never two copies. bar, run and wall move to the font layer in Phase 3 step 5, not here.

### Drawing

- One list row per entry: icon glyph, name, and right-aligned details (size for files, modified date; directories counted their entries until 0.2.5), in the UI font (`sans-serif`, which is Noto Sans), sizes in `monospace` digits so they line up.
- The selection is a full-width bar in the palette's selection colors. Directories in the accent color, hidden entries muted, symlinks in italic, broken links in the urgent color.
- Every frame is drawn on the CPU into the shm buffer, like view. Scrolling a long directory redraws only the list; text is cached per glyph, so a full redraw at 4K stays well under a frame.
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

## 0.2: Sidebar, toolbar and mouse

### Layout

Like a file explorer: a toolbar along the top, a sidebar of places on the left, and the current directory filling the rest.

- **Toolbar:** a sidebar button, back, forward and up, then the path as pieces to click, with `read-only` or `root` at its end. Back and forward keep the last 100 directories and skip any deleted since. A path too long for the box folds its start into `…`.
- **Sidebar:** Home, then the user folders that exist (Desktop, Documents, Downloads, Music, Pictures, Videos, from `user-dirs.dirs` or the usual names), File system (`/`), and under a Drives heading whatever is mounted in `/run/media`, `/media` or `/mnt`. It follows `/proc/self/mountinfo`, so a drive shows up when it's mounted and goes when it's unmounted. The place you're in is highlighted. Bookmarks join it in 0.3.
- The sidebar is 180 logical pixels wide to begin with. Its edge can be dragged (no narrower than 100, no wider than half the window), and the width and whether it's shown are kept in `~/.local/state/files/state`. Under 60 cells of width it hides itself.
- **The list:** the current directory under a column header, Name, Type, Size and Modified (0.2.5). Type comes from the extension (`PNG image`, `C source`, `Folder`).
- **Sorting and columns** (0.2.6, brought forward from 0.3): clicking a heading sorts by it and clicking again reverses, with an arrow on the sorted one; Size and Modified start biggest and newest first. Directories stay first. Dragging the edge between headings resizes a column, and double-clicking it resets it. When the window is too narrow the Type, Size and Modified columns narrow in proportion instead of disappearing. The sort and widths are kept in the state file.
- **Folder sizes** (0.2.6): a thread sums each folder's contents (regular files' apparent sizes, not following links or crossing into other file systems), showing `…` until it's done, so a big tree never holds up the window. Moving to another directory stops the walk. A refresh keeps the old sizes on screen until the new ones arrive.
- **Mouse** (brought forward from 0.5): click selects, double-click enters a directory, a place goes there, and the wheel scrolls the list without moving the selection. The mouse's side buttons go back and forward.
- **Tabs** along the top, each with its own directory, selection, history and search: Ctrl+T, Ctrl+W (the last tab closes the window), Ctrl+Tab, Alt+1–9, a × on each and a `+`, and middle-click on a directory, place or path piece to open it in a background tab.
- **Search** in a field beside the path (Ctrl+F): it filters the current directory as you type, ignoring ASCII case. Going elsewhere ends it. Brought forward from 0.3.
- **Views:** list, or a grid of large icons with names under them in up to two lines, from buttons at the toolbar's end or Ctrl+1 and Ctrl+2. Brought forward from 0.5; thumbnails in the icon view stay in 0.5.
- **Zoom:** Ctrl+wheel, Ctrl+plus, minus and 0. The list's text goes from 0.75× to 1.5×, the icons from 48 to 192 pixels, as one scale: zooming in past the biggest list switches to icons, and out past the smallest icons back to the list. Each view keeps its own zoom, saved with the view in the state file.
- No preview. 0.2 first had Miller columns (parent, current, preview) with image and text previews made on worker threads; they were replaced by this layout in 0.2.2. That code is in git history (`14b4e7f`) if a preview pane comes back, and the image decoding and worker thread will return with 0.5's thumbnail grid.
- The status line 0.2 first had is gone too. The details it showed are the list's size and date columns; permissions and owner come back in a properties view if they're missed.

### Controls

| Key | Action |
|---|---|
| Alt+Left or `-`, Alt+Right | Back, forward |
| Alt+Up | Up, like `h` |
| `b`, F9 | Show or hide the sidebar |
| `<`, `>`, `=` | Sidebar narrower, wider, back to 180 |
| Ctrl+T, Ctrl+W, Ctrl+Tab | New tab, close tab, next tab |
| Ctrl+F, Escape | Search, clear it |
| Ctrl+1, Ctrl+2 | List, icons |
| Ctrl+plus, Ctrl+minus, Ctrl+0 | Zoom |

### Manual checks

- The sidebar button, back, forward, up and the path pieces work by clicking; the side buttons go back and forward.
- Clicking selects, double-clicking enters, and the wheel scrolls the list.
- Plugging in a USB stick (once mounted) adds it under Drives; unmounting removes it.
- Dragging the sidebar's edge resizes it, and the width and visibility come back after a restart.
- Tabs open, close and switch by click, middle-click and keys, and each keeps its own place.
- The search field narrows a large directory as you type.
- Ctrl+wheel zooms the list and the icons, and both come back after a restart.

---

## 0.3: Opening, menus and finding

Progress: opening files, the standard keys and type to select are in (0.3.0), with desk's new `DESK_APPS` piece (desktop entries, `mimeapps.list`, MIME types, launching). The right-click menu with Open with, Run, Copy path and bookmarks followed (0.3.1), on desk's new popups, then typing a path with suggestions and Tab completion (0.3.2), on desk's new text field, which the search field moved onto too (selection, copy and paste). Left: the "Other application…" dialog. Sorting moved into 0.2.

### Opening files

- Double-click or Enter on a file opens it with the default application for its MIME type: `mimeapps.list` (`~/.config/mimeapps.list`, then the system lists) and desktop entries from `XDG_DATA_DIRS` and `~/.local/share/applications`. Images go to view, markdown to mark in deal (through `Terminal=true` and the `terminal` command from run's config), text to `editor` in deal.
- Several selected files (0.4) open together, each with its own application.
- Executables don't run on double-click; they open like any other file of their type. The menu has Run for them, in deal when it's a script.
- Applications start detached in their own session, like mark's opener.

### The right-click menu

A popup menu (desk's new `xdg_popup` piece) at the pointer. Keyboard users reach it with the Menu key or Shift+F10.

| Right-click on | Menu |
|---|---|
| A file | Open, Open with ▸, Run (executables), Copy path, Add to sidebar (directories). 0.4 adds Cut, Copy, Rename, Move to trash, Properties |
| Empty space in the list | Show hidden files (checked when on), Sort by ▸, Refresh. 0.4 adds Paste, New folder, New file |
| A place in the sidebar | Open, Remove from sidebar (bookmarks only) |

- **Open with ▸** lists the applications that declare the type, the default first, then "Other application…", a dialog listing every application with a search field and an "Always use for this kind of file" checkbox, written atomically to `~/.config/mimeapps.list`.

### Finding and going places

- **Typing a path:** clicking the empty part of the path bar, or Ctrl+L, turns it into a text field holding the full path, selected. Suggestions for the directory being typed drop down below it; Tab or a click completes one, `~` expands, Enter goes there, Escape gives the crumbs back.
- **Search** in the current directory is in since 0.2. Searching below it, recursively, comes after 1.0 if it's wanted.
- **Type to select:** typing letters in the list jumps to the first name that starts with them, as in other explorers. The typed text resets after a second without a key.
- **Bookmarks:** "Add to sidebar" in the menu, or dragging a folder onto the sidebar (0.4), adds it under a Bookmarks heading. They're kept in GTK's bookmarks file (`~/.config/gtk-3.0/bookmarks`), so GTK file dialogs and other file managers show the same ones.

### Keys

The standard ones, replacing 0.1's letter keys:

| Key | Action |
|---|---|
| Up, Down, Page Up, Page Down, Home, End | Move the selection |
| Enter | Open (go into a directory) |
| Backspace, Alt+Up | Up to the parent |
| Alt+Left, Alt+Right | Back, forward (and the mouse's side buttons) |
| Alt+Home | Home |
| Ctrl+L | Type a path |
| Ctrl+H | Show or hide hidden files |
| F5 | Refresh |
| F9 | Show or hide the sidebar |
| Menu, Shift+F10 | The right-click menu for the selection |
| Ctrl+W, Ctrl+Q | Close |
| Letters and digits | Type to select |

### Manual checks

- Double-clicking a PNG opens view, a `.md` opens mark in a new deal window, a `.c` file opens `editor` in deal.
- Right-click → Open with → Other application… with "Always use" checked changes the default, and it sticks after a restart.
- Typing a path with Tab completion goes there.
- A bookmark added in files shows in a GTK app's file dialog.
- Clicking the Size heading sorts by size, and again reverses it.

---

## 0.4: File operations

Progress: selecting is in (0.4.0): Ctrl+click, Shift+click, Shift and Ctrl with the moving keys, Ctrl+A, the rubber band, and entries lighting up under the pointer. The selection survives refreshes and sorting; a search drops what it hides. The "Other application…" dialog from 0.3 is put off. Thumbnails were brought forward from 0.5 into 0.4.1, in the freedesktop cache. Rename (in place), New folder and New file, the trash (freedesktop, with a Trash place, Restore and Empty trash), Delete permanently behind a dialog, and one step of undo followed in 0.4.2; the slow second click to rename is left out for now, and only the home drive's trash is used. Copy, cut and paste (shared with other applications), copying, moving and deleting on a worker thread with a progress bar and Cancel, the Skip / Keep both / Replace (Merge) question, and undo for copies and moves followed in 0.4.3; the bar is along the window's foot rather than under the toolbar. Drag and drop followed in 0.4.4, and in dwl what files opens floats over it. Properties (read-only for now) and a badge under the pointer while dragging came in 0.4.5, which finishes 0.4 apart from the slow second click to rename, the trash on other drives and the "Other application…" dialog.

### Selecting

- Click selects one entry, Ctrl+click adds or removes one, Shift+click selects the range from the last click. Dragging on empty space draws a rubber band that selects what it touches.
- Ctrl+A selects everything; Escape or a click on empty space clears the selection. Shift with the arrow keys extends it.

### Doing things

Every action is in the right-click menu, with the usual shortcut:

| Action | Shortcut | Notes |
|---|---|---|
| Cut, Copy, Paste | Ctrl+X, Ctrl+C, Ctrl+V | Paste goes into the current directory, or into a folder right-clicked |
| Rename | F2, or a slow second click on a selected name | Inline, with the name minus its extension selected |
| New folder, New file | Ctrl+Shift+N | Created as "New folder" with its name being edited |
| Move to trash | Delete | |
| Delete permanently | Shift+Delete | After a dialog that names what goes |
| Undo | Ctrl+Z | The last rename, move, copy or trash |
| Properties | Alt+Enter | Type, size (a folder's counted in the background), location, permissions, owner, modified time, link target |

### Drag and drop

- Dragging the selection onto a folder, a crumb in the path or a place in the sidebar moves it there within one filesystem and copies it onto another, as in other explorers. Holding Ctrl copies, Shift moves. The target highlights while it's under the pointer.
- Dropping a folder on the sidebar's empty space bookmarks it.
- Dragging to and from other applications comes in 0.5.

### Design

- Copy, move and delete run in a worker thread with a progress bar under the toolbar (files, bytes, current name) and a Cancel button. Moves within a filesystem are `renameat2`; across filesystems, copy then remove. Copies use `copy_file_range` and keep modes and times. Symlinks are copied as links.
- Dialogs are drawn over the window, modal: "Replace report.pdf?" with Replace, Skip, Keep both (`report (2).pdf`) and "Do this for all conflicts"; confirmations for permanent deletes; errors that name the file and why.
- The trash follows the freedesktop spec (`~/.local/share/Trash/files` and `info`, `$topdir/.Trash-$uid` on other filesystems), so other tools see the same trash. Trash is a place in the sidebar; right-click in it offers Restore and Empty trash.
- The clipboard is shared with other applications as `text/uri-list` and `x-special/gnome-copied-files` through `wl_data_device`, so a copy in files can be pasted in a browser's upload dialog or another file manager. Pasting `text/uri-list` from elsewhere copies those files in.
- Nothing is ever overwritten without asking.

### Protected places

So a slip can't delete the system, files only changes things inside the user's own home directory. This is on top of the kernel's permissions: it also covers files the user happens to own elsewhere, such as in `/tmp` or on a mounted drive.

- Every operation asks `may_modify` (in files.c since 0.2) of each directory whose entries it changes: the destination of a copy, move, paste or new file, and the source directory of a move, rename or trash. The directory is judged by where it really is (`realpath`), so a symlink inside home that points at `/etc` doesn't count as home.
- Outside home, actions that would change something are greyed out in the menu, a drop is refused (the pointer shows "not allowed"), and a shortcut shows "/etc is protected: files only changes things inside your home directory". Copying *from* anywhere into home is fine.
- Running files as root (`sudo files`, effective uid 0) lifts the rule. The end of the path then shows `root` in the urgent color, so it's never forgotten.
- Recursive operations never follow symlinks and never cross onto another filesystem, like `rm --one-file-system`, so a bind mount or a link to `/` inside a directory being deleted can't take the system with it.
- More writable places, such as a USB drive under `/run/media/$USER`, are an explicit opt-in in the config (0.5, `[safety] writable`). Home itself can't be removed from the list.
- The end of the path already shows a lock and `read-only` outside home (0.2).

### Manual checks

- Copying a large directory from a USB stick into home shows progress and can be cancelled midway, leaving no half-copied file.
- In `/etc` or `/tmp`, Cut, Rename, Delete and Paste are greyed out and a drop is refused; run as root, they work and the path bar says `root`.
- Deleting a directory in home that contains a symlink to `/` removes the link, not what it points to.
- Dragging files onto a folder moves them; with Ctrl it copies. Renaming, moving and trashing can each be undone with Ctrl+Z.
- Ctrl+click, Shift+click and the rubber band select as in other explorers.
- A file trashed in files appears in another tool's trash view, and restoring it puts it back.

---

## 0.5: Views, thumbnails, config and desktop integration

- **Views:** the list and icon views and their zoom are in since 0.2. 0.5 remembers the view per directory.
- **Thumbnails** (done early, in 0.4.1; the `[thumbnails]` config is still to come) for images in the icon view, decoded on a worker thread (desk's image decoding, as 0.2's first version did for previews). They follow the freedesktop thumbnail spec (`~/.cache/thumbnails/normal` and `large`, PNG with `Thumb::URI` and `Thumb::MTime`), so other applications can share them. wall's browser keeps a private cache of its own (`~/.cache/wall/thumbnails`); moving it onto the standard one is optional and would let the two share.
- **Drag and drop with other applications** through `wl_data_device`: dragging files out of files into a browser's upload area, an editor or another file manager (`text/uri-list`), and dropping files from them into files, which copies them in.
- **Hover:** rows highlight under the pointer, and resting on a name that was cut short shows it in full in a tooltip.
- Config through desk at `~/.config/files/files.conf`, written from a sample on first run:

  ```ini
  [layout]
  # The sidebar's starting width in logical pixels, until it's dragged.
  # Dragged widths are kept in ~/.local/state/files/state.
  sidebar = 180
  font_size = 11

  [view]
  # details or icons, for directories not yet visited.
  view = details
  hidden = false
  sort = name
  directories_first = true
  # Double-click to open, or single.
  click = double

  [thumbnails]
  enable = true
  # Largest image decoded for a thumbnail, in MiB.
  image_limit = 64

  [safety]
  # Places outside home that files may change, besides home itself.
  # Root may change anything regardless.
  writable =

  [open]
  # How terminal applications are started.
  terminal = deal -e
  ```

- Colors from the palette, followed live through `desk_watch`: background, foreground, accent for directories, muted for hidden entries and details, the selection colors for the selection, urgent for broken links and errors.
- `files.desktop` with `MimeType=inode/directory;`, and `make default` registering it, so other apps' "show in folder" opens files. A dwl key binding (Super+E) in the README.

### Manual checks

- Dragging an image from files into a browser upload works; dragging a file from a browser into files copies it in.
- The icon view fills in thumbnails once and reuses them on the next visit, from `~/.cache/thumbnails`.
- Changing the wallpaper recolors an open window.

---

## 0.6: File picker

files becomes the file dialog for other applications: choosing files to upload in a browser, choosing where a download goes, and any "Open…" or "Save as…" dialog. There are two parts. `files --pick` is a picker usable from the command line and scripts. `files --portal` is the portal backend that opens that picker for applications.

This comes after 0.5 because a picker needs the earlier milestones: MIME types and the search field from 0.3, multiple selection and New folder from 0.4, and thumbnails from 0.5, since choosing a photo to upload is easier when you can see it.

### Command line

```text
files --pick [--multiple] [--directory] [--filter NAME:PATTERNS]... [--title TEXT] [--accept LABEL] [-0] [DIR|FILE]
files --save NAME [--filter NAME:PATTERNS]... [--title TEXT] [--accept LABEL] [DIR]
files --portal
```

- `--pick` chooses one existing file, or several with `--multiple`. With `--directory` it chooses a directory instead.
- `--save NAME` asks for a file name and a directory to save it in, with NAME as the suggested name. Without a DIR it starts in the download directory (`XDG_DOWNLOAD_DIR` from `~/.config/user-dirs.dirs`, else `~/Downloads`), or wherever the last save went.
- `--filter 'Images:*.png;*.jpg;image/webp'` is repeatable. Patterns are globs or MIME types (matched through `desk_mime_type`). The first filter is active, and "All files" is always added last, in the filter dropdown.
- Chosen paths go to stdout, one per line, or NUL-separated with `-0`.
- Exit statuses: 0 chosen, 1 error, 2 usage, 3 cancelled (nothing printed). Cancelling is a separate status so a caller can tell it apart from a failure.

### The picker window

- It's the normal window: toolbar, sidebar, list or icons, search, typing a path, sorting and hidden files. Along the bottom is a bar with the file type filter (a dropdown: "Images", "All files"), and Cancel and Open (or the `--accept` label) buttons. When saving, a name field sits on the left of that bar.
- App ID `files-picker`, floating, with a dwl rule like view's, at about 60% of the output.
- Entries that don't match the chosen filter are hidden; directories always show.
- Double-clicking a file, or selecting it and clicking Open, chooses it. With `--multiple`, Ctrl+click and Shift+click select several and Open returns them all. With `--directory`, Open chooses the selected directory, or the current one when nothing is selected.
- The only operation from 0.4 is New folder, because saving into a new folder is a common need. Cut, paste, rename and trash are off, so a stray click in a dialog can't move anything.
- Enter is Open and Escape is Cancel, as in any dialog.

### Saving

- The name field has the focus at the start, with the name minus its extension selected. Typing replaces it.
- Clicking an existing file copies its name into the field.
- A name with a `/` in it (`~/Documents/report.pdf`) is a path: the directory part goes there.
- Save (Enter) saves into the current directory. If the file already exists, files asks "Replace report.pdf?" before returning. It never writes the file itself. The application does that.
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
  - `choices` (the extra combo boxes and checkboxes some applications add) are returned at their defaults in 0.6. Drawing them in the bottom bar can come later if an application turns out to need them
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

- `files --pick --multiple ~/Pictures` prints the chosen paths. Cancel prints nothing and exits 3.
- `files --save report.pdf` starts in the download directory with "report" selected. Saving over an existing file asks first.
- `files --pick --filter 'Images:*.png;*.jpg'` hides other files, and the filter dropdown switches to "All files".
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
- README documents every menu, shortcut, option and setting.
- Tag `files-1.0.0`.

## After 1.0 (unscheduled)

- The portal's `choices` (extra combo boxes and checkboxes) drawn in the picker's bottom bar, if an application needs them.
- Recursive search below the current directory, from the search field.
- Drives that aren't mounted yet in the sidebar, mounted with a click through `mount` (#15) once it exists. Mounted ones are listed since 0.2.
- Archives: browse `.zip` and `.tar.*` as directories through libarchive, which is installed.
- Split panes, if tabs turn out not to be enough.
- A preview pane, if it's missed: images and text from 0.2's first version (`14b4e7f`), PDF and video once `read` and `video` exist to borrow from.
- Bulk rename in `editor`: the names as lines of a file, renamed to whatever they are when it's saved.

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.1 | One directory, keyboard navigation, live refresh; desk gains the font layer, keyboard and window, view moves onto them | Done (key checks and fractional scale pending) |
| 0.2 | Sidebar of places, toolbar with back, forward, up and the path, mouse, tabs, search, list and icon views, zoom, sortable resizable columns, folder sizes | Done (live key and mouse checks pending) |
| 0.3 | Double-click to open, the right-click menu and Open with, typing a path, bookmarks, standard keys | Done, but for the "Other application…" dialog (put off) |
| 0.4 | Selecting with Ctrl/Shift/rubber band, cut, copy, paste, rename, new folder, trash, undo, drag and drop, dialogs, progress, properties | In progress: selecting, rename, new folder, trash, delete, undo, copy, move, paste, drag and drop, properties done |
| 0.5 | Thumbnails in the icon view, drag and drop with other apps, hover, config, palette, desktop entry | Not started |
| 0.6 | File picker: `--pick` and `--save` with Open and Cancel buttons, the FileChooser portal backend for browsers and other apps | Not started |
| 1.0 | Checklists, a week of use, fuzzing, README, tag | Final |
