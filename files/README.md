# files

`files` is the graphical file manager. Version 0.2 is laid out like a file explorer: tabs along the top, a toolbar with back, forward and up buttons, the path, a search field and the view buttons, a sidebar of places on the left, and the current directory filling the rest as a list or a grid of icons. You move through the tree from the keyboard or the mouse, and it refreshes when a directory changes. Opening files, file operations and the file picker come in later versions; the plan is in [docs/files-roadmap.md](../docs/files-roadmap.md).

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
| Up, Down | Previous, next entry (a row of icons in the icon view), selecting just it |
| Left, Right | Previous, next icon in the icon view |
| Shift with the moving keys | Select from where you started to there |
| Ctrl with the moving keys | Move without changing the selection (an outline shows where) |
| Ctrl+Space | Add the entry under the outline to the selection, or take it out |
| Ctrl+A | Select everything shown |
| Enter | Open: into a directory, or a file with its application (see Opening files) |
| Backspace, Alt+Up | Up to the parent, with the directory you came from selected |
| Alt+Left, Alt+Right | Back, forward through the directories visited |
| Alt+Home | Home |
| Home, End | First, last entry |
| Page Up, Page Down | A page up, down |
| Letters and digits | Type to select: the next name starting with what you type |
| Ctrl+H | Show or hide hidden files |
| F5, Ctrl+R | Refresh |
| F9 | Show or hide the sidebar |
| Menu, Shift+F10 | The right-click menu for the selected entry |
| Ctrl+T | New tab, in the same directory |
| Ctrl+W | Close the tab; the last one closes the window |
| Ctrl+Tab, Ctrl+Shift+Tab (or Ctrl+Page Down, Page Up) | Next, previous tab |
| Alt+1 … Alt+8, Alt+9 | That tab, the last tab |
| Ctrl+F | Search |
| Ctrl+L | Type a path (see below) |
| Escape | Clear the search; with no search, clear the selection |
| Ctrl+1, Ctrl+2 | List, icons |
| Ctrl+plus, Ctrl+minus, Ctrl+0 | Zoom in, out (switching view at either end), back to normal |
| Ctrl+Q | Quit |

While the search field has the focus, typing goes into it: Left, Right, Home and End move (a word at a time with Ctrl, selecting with Shift), Backspace and Delete delete (a word with Ctrl), Ctrl+A selects it all, Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste, Escape clears it, and Enter, Tab or Down give the focus back to the list with the search kept. Other Ctrl shortcuts still work.

### Typing a path

Ctrl+L, or a click on the empty part of the path, turns the path into a field holding the full path, all selected so typing replaces it. It edits like the search field. As you type, the folders in the directory you're typing in that start with what follows the last `/` (ignoring case; hidden ones only once you type a `.`) drop down below it, up to ten.

- **Tab** completes: the chosen suggestion, or the only one, or as much as they all share; on a whole folder name it adds the `/`.
- **Up and Down** choose a suggestion; a click on one goes there.
- **Enter** goes to what's typed. `~` is home, and a path not starting with `/` is taken from the current directory; `.` and `..` work. A file opens its folder with the file selected. A path that doesn't exist says so and leaves the field open.
- **Escape**, or a click anywhere else, gives the path pieces back.

Type to select works like other explorers: typing letters within a second of each other jumps to the first name starting with them, ignoring case; typing the same letter again moves on to the next name starting with it. After a second without typing it starts over.

0.1's letter keys (`h`, `j`, `k`, `l`, `g`, `q` and the rest) are gone, so letters can select. Movement keys repeat while held, at the compositor's rate. Enter doesn't repeat, so holding it can't dive down the tree.

## Opening files

Double-click or Enter opens a file the way other Linux desktops do:

1. Its type comes from shared-mime-info's database (`/usr/share/mime/globs2`) by name. Without shared-mime-info, files knows the common extensions itself. Failing both, it looks at the content: text, an ELF program, or unknown.
2. The application is the default for that type in `mimeapps.list` (`~/.config/mimeapps.list` first, then the system's), then other associations listed there, then any application whose desktop entry (in `~/.local/share/applications` and `/usr/share/applications`) declares the type. Text types also get text/plain's applications.
3. It starts in the file's directory, in its own session, so closing files doesn't close it. An application with `Terminal=true` (like mark) runs inside the terminal set in run's config (`[applications] terminal`), `deal -e` by default.
4. Text that nothing claims opens in `editor` in that terminal. Anything else shows a line at the foot of the list saying nothing is set up to open it, until the next click or key.

Programs aren't run by double-clicking: an executable opens like any other file of its type. The right-click menu's Run runs it.

## The right-click menu

| Right-click on | Menu |
|---|---|
| A file | Open, Open with ▸ (the applications for its type, the default first), Run (executables), Copy path |
| A folder | Open, Open in new tab, Copy path, Add to sidebar |
| A place in the sidebar | Open, Open in new tab, Copy path, Remove from sidebar (bookmarks) |
| The empty part of the list | Show hidden files, Sort by ▸ (Name, Type, Size, Modified, Descending), Refresh, Copy path (of the folder) |

It's a popup the compositor places, so it flips or slides to stay on screen; a click outside closes it. Hovering an item with ▸ opens its submenu. From the keyboard: Up and Down choose, Right opens a submenu, Left or Escape closes it, Enter or Space picks.

- **Run** starts a script (text with `#!`) in the terminal so its output shows, and a program on its own.
- **Copy path** puts the full path on the clipboard as text, until something else is copied.
- **Add to sidebar** adds the folder to GTK's bookmarks (`~/.config/gtk-3.0/bookmarks`), so GTK file dialogs and other file managers show it too. Bookmarks appear under a Bookmarks heading in the sidebar.

## Mouse

| Where | Action |
|---|---|
| A tab | Switches to it; its × or a middle-click closes it |
| `+` after the tabs | New tab |
| The sidebar button (left of back) | Shows or hides the sidebar |
| Back, forward, up buttons | As Alt+Left, Alt+Right, Alt+Up; a button is dimmed when there's nowhere to go |
| A piece of the path | Goes to that directory, with the one you came through selected |
| A place in the sidebar | Goes there |
| The empty part of the path | Type a path, as Ctrl+L |
| The search field | Focuses it, with the cursor where you clicked; drag to select, double-click to select it all; its × clears it |
| The list and icon buttons (right end) | Switch the view |
| An entry | Selects it alone; double-click opens it (goes into a directory, or opens a file). Entries light up under the pointer |
| Ctrl+click an entry | Adds it to the selection, or takes it out |
| Shift+click an entry | Selects from the last entry clicked to this one (with Ctrl, adds that range) |
| The empty part of the list | Clears the selection |
| Drag from empty space, or from beside a name in the list view | A rubber band: selects what it touches (with Ctrl or Shift, adds to the selection); scrolls when dragged past the top or bottom |
| Right-click | The menu for an entry, a place, or the empty part of the list (see below) |
| Middle-click on a directory, place or path piece | Opens it in a new tab behind this one |
| The wheel over the entries | Scrolls them, three rows a notch (a row of icons in the icon view), without moving the selection. Touchpads scroll by distance |
| Ctrl+wheel | Zooms; past the biggest list it turns into icons, and past the smallest icons back into the list |
| Mouse side buttons | Back, forward |
| The sidebar's edge | Drag to resize; double-click resets |

After scrolling with the wheel the selection may be off screen; the next key that moves it brings it back into view.

## What it shows

At the very top, the tabs: each shows its directory's name (Home for home) and a ×, the current one underlined, then `+`. Each tab keeps its own directory, selection, back and forward history and search; switching to a tab whose directory has gone since lands on the nearest parent that exists.

Under them, the toolbar: the sidebar button, back, forward and up, then the path, cut into pieces you can click (home shows as a house). When the path is too long the pieces on the left are folded into `…`, which goes to the last one folded. At the right end of the path, `read-only` or `root` when that applies (see below). Back and forward remember the last 100 directories; going back skips any that have been deleted since. Then the search field and the list and icon view buttons.

### Search

The search field filters the current directory as you type: names containing the text, ignoring ASCII case, with hidden files still hidden unless shown. "No matches" says when nothing does. Going to another directory ends the search.

### Views and zoom

- **List:** one row per entry, with size and date where there's room. Zoom changes its text size, from 0.75× to 1.5×.
- **Icons:** a grid of large icons with the name under each in up to two lines, broken at a space, dot, dash or underscore where it can be, and cut in the middle when it's still too long. Zoom changes the icon size, from 48 to 192 pixels; the grid fits as many columns as it can.

The two make one scale: zooming in past the biggest list switches to the smallest icons, and zooming out past the smallest icons switches back to the biggest list. Ctrl+0 resets the current view only. Each view keeps its own zoom. The view and both zooms are kept in the state file.

### The sidebar

| Place | Where |
|---|---|
| Home | `$HOME` |
| Desktop, Documents, Downloads, Music, Pictures, Videos | From `~/.config/user-dirs.dirs` when it names them, else `~/Desktop` and so on; only the ones that exist |
| Bookmarks | GTK's bookmarks, ones that still exist, by their label or folder name |
| File system | `/` |
| Drives | Whatever is mounted under `/run/media`, `/media` or `/mnt`, by its mount point's name |

It has three groups: **Places** (home, its folders and the file system), **Bookmarks** and **Drives**, each under a heading, the last two only when they have something. The place you're in is highlighted. The list is made again whenever a directory loads, so a new `~/Music` shows up, and drives follow `/proc/self/mountinfo`, so one appears as soon as it's mounted and goes when it's unmounted.

The sidebar is 180 pixels wide to begin with. Drag its edge (the pointer turns into a resize arrow) or use `<` and `>`; double-clicking the edge or `=` resets it. It's never narrower than 100 or wider than half the window, and it hides itself when the window is narrower than 60 character cells. Its width and whether it's shown are kept in `~/.local/state/files/state` (or under `$XDG_STATE_HOME`), with the view and zoom.

### The list

A header over the list names its columns: Name, Type, Size and Modified. Each row has an icon and the name; where the list is wide enough the type, size and the date and time (`2026-11-28 14:03`) follow. The header stays in the window's font when the list is zoomed, lined up with the columns.

- **Sorting:** click a heading to sort by it, again to reverse; an arrow marks the sorted one. Size and Modified start biggest and newest first. Folders always come before files. Ties go by name.
- **Column widths:** drag the edge between two headings (the pointer turns into a resize arrow) to resize the column on its right; double-click the edge to reset it. The name takes what's left. In a narrow window Type, Size and Modified shrink in proportion rather than disappear.
- The sort and the widths are kept in the state file.

- **Type** comes from the extension: `Folder`, `PNG image`, `MP4 video`, `ZIP archive`, `C source`, `Markdown file`, `PDF document`, `Program` for executables, or `TXT file` and plain `File` for the rest. Symlinks to folders say `Folder link` and broken links `Broken link`.
- **Size** of a folder is everything inside it, summed on a separate thread: the apparent size of regular files, without following symlinks or crossing into another file system (so `/` doesn't count `/proc` or a USB stick). It shows `…` while it's being summed and `?` when the folder can't be read; a folder you can only partly read counts what it can. Links to folders have no size. Sorting by size reorders as the sums come in.

- Directories come first, then files, in natural order (`file2` before `file10`) and case-insensitively. Letters fold by ASCII case only; other characters compare by code point.
- Directories are in the palette's accent color and hidden entries are muted. Symlinks are in italic and broken links in the urgent color.
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
- [x] The tab strip, search field and view buttons draw; the icon view lays out a grid.
- [x] The list has a Name, Type, Size, Modified header lined up with its columns.
- [x] Folders show their summed size, and sorting by Size orders them by it.

## Manual checks (0.3)

- [x] Opening a file starts the application mimeapps.list or a desktop entry gives it, expanding `%f`, `%F` and `%c` (checked with test entries).
- [x] Text nothing claims opens in `editor`; a file nothing opens says so.
- [x] Typing letters selects by name; the same letter again cycles.
- [x] The menus for a file, a folder, a place and the empty list have the right items, and their actions work (checked headlessly: Open with lists the type's applications, Sort by sorts, Add to sidebar writes GTK's bookmarks).
- [ ] Right-click opens the menu at the pointer, a submenu opens on hover, a click outside closes it, and the Menu key works.
- [ ] Copy path pastes into another application.
- [ ] Double-clicking a PNG opens view and a `.md` opens mark in deal, once both are installed (`make install` and their desktop entries).
- [ ] Clicking headings sorts and reverses; dragging an edge resizes, double-clicking resets, and both come back after a restart.
- [ ] Narrowing the window narrows the columns instead of hiding them.
- [ ] Tabs open, close and switch by clicking, middle-click and keys, each keeping its place.
- [ ] Typing in the search field filters as you type; its × clears it.
- [ ] Shift+arrows and dragging select in the search field; Ctrl+C and Ctrl+V copy and paste, including from another app.
- [ ] Rows and icons light up under the pointer. Ctrl+click, Shift+click, Shift+arrows, Ctrl+A and Escape select as described; a rubber band from empty space or beside a name selects what it touches, and scrolls past the edges.
- [ ] Ctrl+L or a click on the empty part of the path gives a field; suggestions follow typing; Tab completes; Enter goes there; Escape or a click elsewhere puts the pieces back.
- [ ] Ctrl+wheel zooms the list and the icons, crossing from one to the other at the ends, and the zoom comes back after a restart.
