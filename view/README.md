# view

`view` is an image viewer for dwl. Version 0.1 opens one PNG or JPEG, fits it to the window and closes on `q` or Escape. What's left is in [docs/view-roadmap.md](../docs/view-roadmap.md).

It's a normal xdg-shell window with app ID `view`, so dwl tiles or floats it like any other client. It renders at the output's physical resolution through `wp-viewporter` and `wp-fractional-scale-v1`, so images stay sharp at fractional scales. It's built on [desk](../desk/README.md): image decoding and shm buffers.

## Build

On Arch Linux the build needs `gcc`, `make`, `pkgconf`, `wayland`, `wayland-protocols`, `libxkbcommon`, `libpng` and `libjpeg-turbo`.

```sh
make
make install      # /usr/local/bin/view; honors PREFIX, BINDIR and DESTDIR
```

## Usage

```text
view FILE
```

| Option | Meaning |
|---|---|
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

`FILE` is a PNG or JPEG, detected by content, not by its name. `-` reads the image from standard input (`shot - | view -`), and view refuses when standard input is a terminal.

The image is fitted to the window and centred on a dark background. It's shrunk with area averaging, so fine detail doesn't alias, and never enlarged: an image smaller than the window shows one image pixel per screen pixel. Transparent images are composited over the background. When the compositor leaves the window size to view, as for a floating window, the window starts at the image's size, limited to 80% of the output.

| Key | Action |
|---|---|
| `q`, Escape | Close |

Images up to 256 megapixels open. A larger JPEG is decoded at 1/2, 1/4 or 1/8 size; a larger PNG fails. Colors are treated as sRGB, with no color management.

## Exit status

| Status | Meaning |
|---|---|
| 0 | The window was closed |
| 1 | The image couldn't be opened or decoded, or the Wayland connection failed |
| 2 | Usage error |

## Manual checks (0.1)

- [x] A JPEG photo larger than the window fits, centred, without aliasing.
- [x] A PNG smaller than the window is shown pixel-exact, not enlarged (a 64×48 one-pixel checker).
- [x] A transparent PNG composites over the background.
- [x] `view -` reads standard input; empty input, a non-image, a truncated JPEG, a missing file and a directory exit 1 with a message.
- [ ] `q` and Escape close the window.
- [ ] Resizing the window (tile ↔ float, Super+Space) refits immediately without stretching.
- [ ] A floating window opens at the image's size, limited to 80% of the output.
- [ ] Sharp at 1.25× and 1.5× output scale (`wlr-randr`, not installed yet).
