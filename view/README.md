# view

`view` is an image viewer for dwl. Version 0.2 opens one PNG or JPEG fitted to the window, then zooms, pans, rotates and flips it. What's left is in [docs/view-roadmap.md](../docs/view-roadmap.md).

It's a normal xdg-shell window with app ID `view`, so dwl tiles or floats it like any other client. It renders at the output's physical resolution through `wp-viewporter` and `wp-fractional-scale-v1`, so images stay sharp at fractional scales, and uses `cursor-shape-v1` for the drag cursor. It's built on [desk](../desk/README.md): image decoding, shm buffers and key repeat.

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

The image opens fitted to the window and centred on a dark background, never enlarged: an image smaller than the window shows one image pixel per screen pixel. While fitted, it refits when the window changes size. Transparent images are composited over the background. When the compositor leaves the window size to view, as for a floating window, the window starts at the image's size, limited to 80% of the output.

| Key | Action |
|---|---|
| `+` or `=`, `-` | Zoom in or out by √2 around the window centre. Zooming stops at 100% on the way through it |
| Scroll | Zoom around the pointer: one step per wheel notch, smoothly on a touchpad |
| `0` | Fit to the window |
| `1` | 100%: one image pixel per screen pixel |
| `h` `j` `k` `l`, arrows | Pan by a tenth of the window |
| `H` `J` `K` `L` | Pan to that edge |
| Left-button drag | Pan |
| `r`, `R` | Rotate clockwise, counter-clockwise |
| `f`, `F` | Flip horizontally, vertically |
| `q`, Escape | Close |

Zoom and pan keys repeat while held, at the compositor's rate. Zoom ranges from 10% (or less, when fitting needs it) to 3200%, and the title shows the name, size and zoom. Rotation and flips change only what's shown; the file is never written.

Shrinking averages the area each screen pixel covers, using halved copies of the image prepared when it opens, so fine detail doesn't shimmer. Enlarging is smooth below 400% and draws sharp square pixels from 400% up.

Images up to 256 megapixels open. A larger JPEG is decoded at 1/2, 1/4 or 1/8 size; a larger PNG fails. Colors are treated as sRGB, with no color management.

## Exit status

| Status | Meaning |
|---|---|
| 0 | The window was closed |
| 1 | The image couldn't be opened or decoded, or the Wayland connection failed |
| 2 | Usage error |

## Manual checks (0.2)

Checked offline by calling the renderer directly: 100% and fit are pixel-exact, 800% draws exact squares, four rotations and two flips return the original, and panning (which only renders the strip that scrolls in) matches a full redraw at 37%, 50%, 100%, 170% and 600%. On a 50 MP image a pan step takes about 1 ms and a zoom change about 30 ms.

- [ ] Scrolling zooms around the pointer, and the point under it stays still.
- [ ] Keys: zoom, `0`, `1`, pan, Shift+pan to the edge, `r`/`R`/`f`/`F`, and repeat while held.
- [ ] Dragging pans, with a grabbing cursor.
- [ ] A one-pixel checker at 100% shows no blur, and at 800% shows sharp squares.
- [ ] A large photo at fit shows no shimmer on fine detail.
- [ ] `r` on a landscape photo refits it as portrait.

## Manual checks (0.1)

- [x] A JPEG photo larger than the window fits, centred, without aliasing.
- [x] A PNG smaller than the window is shown pixel-exact, not enlarged (a 64×48 one-pixel checker).
- [x] A transparent PNG composites over the background.
- [x] `view -` reads standard input; empty input, a non-image, a truncated JPEG, a missing file and a directory exit 1 with a message.
- [x] `q` and Escape close the window.
- [x] Resizing the window (tile ↔ float, Super+Space) refits immediately without stretching.
- [x] A floating window opens at the image's size, limited to 80% of the output.
- [ ] Sharp at 1.25× and 1.5× output scale. Skipped: the desktop runs at 1×.
