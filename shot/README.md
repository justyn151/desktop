# shot

`shot` takes screenshots on dwl. Version 0.1 captures a whole output to a PNG file. Region, clipboard, window capture and key bindings follow in the milestones in [docs/shot-roadmap.md](../docs/shot-roadmap.md).

It uses `ext-image-copy-capture-v1` with `ext-output-image-capture-source-v1`, and dwl IPC to find the focused monitor. It's built on [desk](../desk/README.md) (atomic file writes and shared memory).

## Build

On Arch Linux the build needs `gcc`, `make`, `pkgconf`, `wayland`, `wayland-protocols` (1.40 or later for the ext capture protocols) and `libpng`.

```sh
make
make install      # /usr/local/bin/shot; honors PREFIX, BINDIR and DESTDIR
```

## Usage

```text
shot [-o OUTPUT] [-c] [FILE]
shot -l
```

| Option | Meaning |
|---|---|
| `-o`, `--output NAME` | Capture this output, by its name from `shot -l` (such as `eDP-1`) |
| `-c`, `--cursor` | Paint the cursor into the image |
| `-l`, `--list` | Print each output's name, size and description, marking the focused one, and exit |
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

Without `-o`, shot captures the output dwl reports as focused. Without dwl IPC it captures the first output and says so when there are several.

`FILE` defaults to `YYYY-MM-DD_HH-MM-SS.png` in the current directory. A default name never replaces an existing file: a second shot in the same second becomes `…-1.png`, then `…-2.png`. An explicit `FILE` is replaced. The saved path is printed on stdout, so `xdg-open "$(shot)"` works. `-` writes the PNG to stdout instead (`shot - | wl-copy`), and shot refuses when stdout is a terminal.

The file is written to a temporary file in the same directory and renamed into place, so a failed or interrupted write never leaves a partial PNG. Permissions are `0644` minus the umask.

Images are 8-bit RGB without alpha, upright, at the output's physical resolution. Output transforms are undone.

Exit status: 0 saved, 1 failed, 2 usage error.

## Manual verification on dwl

- [x] `shot -l` lists the output and marks the focused one
- [x] `shot` writes a 1920×1080 PNG with correct colors
- [x] Two `shot` runs in the same second give `…png` and `…-1.png`
- [x] `shot -c FILE` includes the cursor
- [x] `shot - | file -` reports a PNG
- [x] An explicit existing `FILE` is replaced
- [x] Unknown `-o` name, unwritable directory and missing display each fail with a message and status 1, and leave no temporary file
- [ ] `shot` without `-c` leaves the cursor out (compare with a `-c` shot)
- [ ] `shot -` on a terminal refuses with status 2
- [ ] Two monitors: `shot -o NAME` picks the right one, and the default follows focus
- [ ] Rotated output (`wlr-randr --output X --transform 90`, then `flipped-90`) gives an upright image. `wlr-randr` isn't installed yet.
