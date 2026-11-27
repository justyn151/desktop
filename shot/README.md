# shot

`shot` takes screenshots on dwl. Version 0.2 captures a whole output or a dragged region to a PNG file. Clipboard, window capture and key bindings follow in the milestones in [docs/shot-roadmap.md](../docs/shot-roadmap.md).

It uses `ext-image-copy-capture-v1` with `ext-output-image-capture-source-v1`, dwl IPC to find the focused monitor, and a `wlr-layer-shell` overlay with `wp-viewporter` and `cursor-shape-v1` for region selection. It's built on [desk](../desk/README.md): atomic file writes, shm buffers, config paths and the INI parser.

## Build

On Arch Linux the build needs `gcc`, `make`, `pkgconf`, `wayland`, `wayland-protocols` (1.40 or later for the ext capture protocols) and `libpng`.

```sh
make
make install      # /usr/local/bin/shot; honors PREFIX, BINDIR and DESTDIR
```

## Usage

```text
shot [-r | -o OUTPUT] [-c] [FILE]
shot -l
```

| Option | Meaning |
|---|---|
| `-r`, `--region` | Drag a rectangle to capture (see below) |
| `-o`, `--output NAME` | Capture this output, by its name from `shot -l` (such as `eDP-1`) |
| `-c`, `--cursor` | Paint the cursor into the image |
| `-l`, `--list` | Print each output's name, size and description, marking the focused one, and exit |
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

Without `-o`, shot captures the output dwl reports as focused. Without dwl IPC it captures the first output and says so when there are several.

`FILE` defaults to the configured directory and name, `~/Pictures/Screenshots/YYYY-MM-DD_HH-MM-SS.png` unless changed. The directory is created when missing. A configured name never replaces an existing file: a second shot in the same second becomes `…-1.png`, then `…-2.png`. An explicit `FILE` is replaced. The saved path is printed on stdout, so `xdg-open "$(shot)"` works. `-` writes the PNG to stdout instead (`shot - | wl-copy`), and shot refuses when stdout is a terminal.

The file is written to a temporary file in the same directory and renamed into place, so a failed or interrupted write never leaves a partial PNG. Permissions are `0644` minus the umask.

Images are 8-bit RGB without alpha, upright, at the output's physical resolution. Output transforms are undone.

Exit status: 0 saved, 1 failed or cancelled, 2 usage error.

## Region capture

`shot -r` captures every output first, so menus and tooltips stay as they were, then covers each output with the frozen image, dimmed. Drag with the left button to select. Release saves. The selection stays on the output where the drag started and stops at its edges. Escape, a right-click, or a click without a drag cancels with status 1 and saves nothing.

The overlay buffer is the captured frame at full resolution, mapped onto the output with `wp-viewporter`, so what you select is exactly what's saved at any scale. Selection edges round outward to whole physical pixels.

## Configuration

shot reads `~/.config/shot/shot.conf` (or `$XDG_CONFIG_HOME/shot/shot.conf`). When the file is missing, shot writes the sample [`shot.conf`](shot.conf) there and says so on stderr. Delete the file to get the defaults back. Missing keys keep their built-in values, and a bad line is reported with its line number and ignored.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[save]` | `directory` | `~/Pictures/Screenshots` | Where captures go without a `FILE`. A leading `~/` means `HOME`. |
| | `name` | `%Y-%m-%d_%H-%M-%S.png` | `strftime` format. No `/`. |
| `[capture]` | `cursor` | `false` | Always paint the cursor. `-c` does it for one capture. |
| `[png]` | `compression` | `6` | zlib level 0–9. |
| `[region]` | `dim` | `0.5` | Darkening outside the selection, 0–1. |
| | `border` | `2` | Outline width in logical pixels, 0–64. 0 hides it. |
| | `color` | `accent` | `#RRGGBB`, or `accent` for the wallpaper palette's accent (white without a palette). |

## Manual verification on dwl

- [x] `shot -l` lists the output and marks the focused one
- [x] `shot` writes a 1920×1080 PNG with correct colors
- [x] Two `shot` runs in the same second give `…png` and `…-1.png`
- [x] `shot -c FILE` includes the cursor
- [x] `shot - | file -` reports a PNG
- [x] An explicit existing `FILE` is replaced
- [x] Unknown `-o` name, unwritable directory and missing display each fail with a message and status 1, and leave no temporary file
- [x] The first run writes `~/.config/shot/shot.conf`, identical to the sample; later runs read it
- [x] Bad values, malformed lines and unknown keys are reported with line numbers, and defaults stay in effect
- [x] Captures without `FILE` go to the configured directory, which is created when missing
- [x] `shot -r -o X` is a usage error
- [x] `shot -r` maps a dimmed frozen frame over the whole output, bar included
- [ ] `shot -r`: dragging shows the selection undimmed with an accent outline, and release saves exactly that area
- [ ] `shot -r`: Escape, right-click and a plain click each cancel with status 1 and save nothing
- [ ] `shot -r`: dragging past the output's edge stops at the edge
- [ ] `shot -r` with a menu open: the menu is in the result
- [ ] `shot -r` at output scale 1.5: the saved size matches the drag in physical pixels, with sharp edges
- [ ] `shot` without `-c` leaves the cursor out (compare with a `-c` shot)
- [ ] `shot -` on a terminal refuses with status 2
- [ ] Two monitors: `shot -o NAME` picks the right one, and the default follows focus
- [ ] Rotated output (`wlr-randr --output X --transform 90`, then `flipped-90`) gives an upright image. `wlr-randr` isn't installed yet.
