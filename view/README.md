# view

`view` is an image viewer for dwl. Version 0.3 browses PNG and JPEG images: a file and the images beside it, a directory, or a list. It zooms, pans, rotates and flips them, and reloads an image when its file is replaced. What's left is in [docs/view-roadmap.md](../docs/view-roadmap.md).

It's a normal xdg-shell window with app ID `view`, so dwl tiles or floats it like any other client. It renders at the output's physical resolution through `wp-viewporter` and `wp-fractional-scale-v1`, so images stay sharp at fractional scales, and uses `cursor-shape-v1` for the drag cursor. It's built on [desk](../desk/README.md): image decoding, shm buffers, key repeat and the file watch.

## Build

On Arch Linux the build needs `gcc`, `make`, `pkgconf`, `wayland`, `wayland-protocols`, `libxkbcommon`, `libpng` and `libjpeg-turbo`.

```sh
make
make install      # /usr/local/bin/view; honors PREFIX, BINDIR and DESTDIR
```

## Usage

```text
view [FILE | DIRECTORY]...
```

| Option | Meaning |
|---|---|
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

| Arguments | Browses |
|---|---|
| One `FILE` | The images in its directory, starting at it |
| One `DIRECTORY` | Its images, starting at the first |
| Several | Exactly those, in order, with directories expanded in place |

A directory's images are the `.png`, `.jpg` and `.jpeg` files (any case), without hidden ones, in natural order: `img2` before `img10`. A file named explicitly is included whatever its name, because the decoder is chosen by content. `-` reads one image from standard input (`shot - | view -`), and view refuses when standard input is a terminal.

The list wraps around. Each image opens fitted and upright. While one image is on screen, its neighbours are decoded in the background between events, so stepping is instant; the one in the direction of travel goes first. An image that can't be decoded shows the background and the error in the title, and browsing carries on. A single `FILE` that can't be decoded is still an error, exiting 1.

view watches the file on screen. When it's replaced or rewritten (by shot, wall or an editor), the new version is shown, keeping the zoom and position when the size is unchanged. If the new version doesn't decode, the old one stays.

The image opens fitted to the window and centred on a dark background, never enlarged: an image smaller than the window shows one image pixel per screen pixel. While fitted, it refits when the window changes size. Transparent images are composited over the background. When the compositor leaves the window size to view, as for a floating window, the window starts at the image's size, limited to 80% of the output.

| Key | Action |
|---|---|
| `n`, Space, Page Down | Next image |
| `p`, Backspace, Page Up | Previous image |
| Right, Left | Next or previous image when the image fits across the window; otherwise pan |
| `g` or Home, `G` or End | First, last image |
| `+` or `=`, `-` | Zoom in or out by √2 around the window centre. Zooming stops at 100% on the way through it |
| Scroll | Zoom around the pointer: one step per wheel notch, smoothly on a touchpad |
| `0` | Fit to the window |
| `1` | 100%: one image pixel per screen pixel |
| `h` `j` `k` `l`, arrows | Pan by a tenth of the window (see Right and Left above) |
| `H` `J` `K` `L` | Pan to that edge |
| Left-button drag | Pan |
| `r`, `R` | Rotate clockwise, counter-clockwise |
| `f`, `F` | Flip horizontally, vertically |
| `q`, Escape | Close |

Zoom and pan keys repeat while held, at the compositor's rate. Zoom ranges from 10% (or less, when fitting needs it) to 3200%, and the title shows the name, the position in the list, the size and the zoom: `img2.png — 3/42 — 1920×1080 — 50%`. Rotation and flips change only what's shown; the file is never written.

Shrinking averages the area each screen pixel covers, using halved copies of the image prepared when it opens, so fine detail doesn't shimmer. Enlarging is smooth below 400% and draws sharp square pixels from 400% up.

Images up to 256 megapixels open. A larger JPEG is decoded at 1/2, 1/4 or 1/8 size; a larger PNG fails. Colors are treated as sRGB, with no color management.

## Exit status

| Status | Meaning |
|---|---|
| 0 | The window was closed |
| 1 | A single `FILE` couldn't be decoded, a directory had no images or couldn't be read, or the Wayland connection failed |
| 2 | Usage error |

## Manual checks (0.3)

Checked offline under ASan and UBSan, by calling the browsing code directly: natural order without hidden files, a `FILE` starting at itself, an explicit non-image name included, several arguments in order with directories expanded in place; preloading forward then backward, a broken neighbour cached as failed, stepping onto a preloaded image without decoding it again, wrapping both ways, a rotated image not kept, reload of a replaced file (keeping the zoom at the same size), no decode for an unchanged file, and a broken replacement keeping the old image. Live on dwl, replacing the open file with `shot` showed the new capture.

- [ ] `view ~/Pictures/Screenshots` steps through every screenshot in name order, and wraps.
- [ ] Stepping forward through photos shows no decode delay after the first.
- [ ] A corrupt file in the middle of a directory shows its error in the title and doesn't stop browsing.
- [ ] Right and Left change images at fit and pan when zoomed in.

## Manual checks (0.2)

Checked offline by calling the renderer directly: 100% and fit are pixel-exact, 800% draws exact squares, four rotations and two flips return the original, and panning (which only renders the strip that scrolls in) matches a full redraw at 37%, 50%, 100%, 170% and 600%. On a 50 MP image a pan step takes about 1 ms and a zoom change about 30 ms.

- [x] Scrolling zooms around the pointer, and the point under it stays still.
- [x] Keys: zoom, `0`, `1`, pan, Shift+pan to the edge, `r`/`R`/`f`/`F`, and repeat while held.
- [x] Dragging pans, with a grabbing cursor.
- [x] A one-pixel checker at 100% shows no blur, and at 800% shows sharp squares.
- [x] A large photo at fit shows no shimmer on fine detail.
- [x] `r` on a landscape photo refits it as portrait.

## Manual checks (0.1)

- [x] A JPEG photo larger than the window fits, centred, without aliasing.
- [x] A PNG smaller than the window is shown pixel-exact, not enlarged (a 64×48 one-pixel checker).
- [x] A transparent PNG composites over the background.
- [x] `view -` reads standard input; empty input, a non-image, a truncated JPEG, a missing file and a directory exit 1 with a message.
- [x] `q` and Escape close the window.
- [x] Resizing the window (tile ↔ float, Super+Space) refits immediately without stretching.
- [x] A floating window opens at the image's size, limited to 80% of the output.
- [ ] Sharp at 1.25× and 1.5× output scale. Skipped: the desktop runs at 1×.
