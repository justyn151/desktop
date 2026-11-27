# view roadmap

Implementation plan for view, the image viewer, from 0.1 to 1.0. It expands `view` in [Phase 5 of PLAN.md](../PLAN.md#phase-5-remaining-core-apps), pulled forward ahead of the other Phase 5 apps.

## Starting point

view is one `view.c` and one `view.h`, built on `desk/` from its first commit, the same way shot was. It's a normal xdg-shell window (app ID `view`), not a layer surface, so dwl tiles or floats it like any other client.

### What already exists

| Piece | Where | view uses it for |
|---|---|---|
| PNG and JPEG decoding to RGBA | `wall/wall.c` (`decode_png`, `decode_jpeg`, `decode_image`, detection by content) | Loading images (0.1) |
| Bilinear sampling | `wall/wall.c` (`interpolate_rgba`) | Reference for upscaling (0.2) |
| shm buffer pool, two buffers with busy tracking | `desk_buffer_get` (`-DDESK_WAYLAND`) | Every frame |
| Fractional scale through `wp_viewporter` | shot 0.2 (buffer at physical size, viewport at logical size) | Sharp images at 1.25× and 1.5× |
| xkbcommon keymap handling | `wall/wall.c` browser, `run`, `deal` | Layout-aware keys (0.1) |
| INI config, paths, `desk_write_default` | desk | Config (0.5) |
| Palette | `desk_palette_load` | Background color (0.5) |
| File watch | `desk_watch_*` | Reload when the image is replaced (0.3) |

dwl already supports `xdg-shell`, `wp_viewporter`, `wp_fractional_scale_v1` and `cursor-shape-v1`. view needs no compositor change.

### What desk gains

| desk piece | Layer | Needed by | Phase 3 step it pre-empts |
|---|---|---|---|
| `desk_image_load` (PNG and JPEG to RGBA, moved from wall) | New opt-in layer, `-DDESK_IMAGE` (libpng, libjpeg) | 0.1 | None. wall and view would otherwise be two copies |
| xkb keymap and modifier state from `wl_keyboard` | 1 | 0.1 | Step 6 (repeat and compose stay out until 0.2) |
| Key repeat with a timerfd | 1 | 0.2 | Step 6 |
| Fractional scale tracking (`wp_fractional_scale_v1` + viewport) for xdg toplevels | 1 | 0.1 | Step 4, and the pattern deal 0.10.4 needs |

`-DDESK_IMAGE` keeps libpng and libjpeg out of apps that don't decode images. shot only encodes, so it stays on its own libpng writer for now.

view draws no text before 1.0. Status (name, position, size, zoom) goes in the window title, which the bar already shows. So view doesn't need desk's font layer either.

### Pixel pipeline

1. Decode to straight-alpha RGBA8, as wall does.
2. Premultiply once at load and convert to ARGB8888 (little-endian `B G R A` in memory). Keep this as the source image.
3. For each view (zoom, pan, rotation), render the visible part into a physical-size shm buffer: composite over the background (or a checkerboard for transparent images), then attach.
4. Scaling filter: area average (box filter) when shrinking, so fitted photos don't alias; bilinear when enlarging below 400%; nearest neighbour at 400% and above, so pixel art and screenshots stay crisp.
5. Cache the last scaled image. Panning without a zoom change only copies from the cache.

---

## 0.1: Open one image, fit to the window

### Command line

```text
view [FILE]
  FILE    PNG or JPEG, detected by content. "-" reads stdin.
```

No argument prints usage and exits 2. A file that fails to decode prints the decoder's message and exits 1.

### Work

1. Move `decode_png`, `decode_jpeg` and content detection from wall into `desk_image_load(FILE *, DeskImage *, char *error, size_t)`. Keep desk's rules: no printing, return an error string. Move wall onto it in a separate commit (`wall: load images through desk`) and run wall's README checklist, so behavior stays identical.
2. Window: `xdg_wm_base`, `xdg_surface`, `xdg_toplevel` with app ID `view` and title `<basename> — W×H`. Handle `configure` (use the compositor's size; if it's 0×0, use the image size limited to 80% of the output), `close` and `ping`.
3. Scale: `wp_fractional_scale_v1` when present, otherwise `preferred_buffer_scale`, otherwise 1. Render at physical size and set the logical size with `wp_viewport_set_destination`.
4. Draw the image fitted (never enlarged beyond 100% in fit mode) and centred on a flat background (`#1e1e1e` until 0.5 brings the palette).
5. Keyboard through xkbcommon: `q` and Escape close the window.
6. Redraw only after configure or a scale change, paced by `wl_surface.frame`.

### Edge cases

- Huge images: refuse anything above 256 megapixels (1 GiB of ARGB) with a clear message rather than running out of memory. Use libjpeg's `scale_num`/`scale_denom` to decode very large JPEGs at 1/2, 1/4 or 1/8 when even the 100% view would exceed the limit.
- 16-bit and palette PNGs, grayscale with alpha, interlaced PNGs and progressive JPEGs (the wall decoder handles these; add a test file for each).
- CMYK and YCCK JPEGs: libjpeg-turbo won't convert them to RGB, so check `jpeg_color_space`, decode as CMYK and convert in view (inverting Adobe's inverted CMYK), or fail with a message.

### Manual checks

- A screenshot from `shot` opens at its real size on a 1× output and is pixel-exact (zoom in with another tool to compare, or check with 0.2's 1:1).
- A large photo fits the window and is sharp at 1.25× and 1.5× output scale.
- Resizing the window (tile ↔ float, `Super+Space`) refits immediately without stretching.
- `view broken.png` and `view notes.txt` exit 1 with a message. `cat a.png | view -` works.

### As built

- The xkb keymap handling and fractional scale tracking stayed in view for now. Each is about forty lines, and neither is shared yet: they move to desk with key repeat in 0.2 (keyboard) and in Phase 3 step 4 (scale), when deal 0.10.4 shows what both need. The `-DDESK_WAYLAND` layer would otherwise pull xkbcommon into shot.
- `desk_image_load` takes a byte limit and a `DESK_IMAGE_SHRINK` flag. wall passes its own 256 MiB limit without the flag, so its behavior is unchanged; view passes 1 GiB with it.
- A window the compositor sizes (tiled) uses that size. When it leaves the size to view (0×0 configure, as for floating), view asks for the image size limited to 80% of `configure_bounds`, or of 1600×1000 when the compositor doesn't send bounds.

---

## 0.2: Zoom, pan and rotate

### Controls

| Input | Action |
|---|---|
| `+` / `=`, `-` | Zoom in or out by √2, around the window centre |
| Ctrl+scroll, or scroll with no modifier | Zoom around the pointer |
| `0` | Fit (the default mode) |
| `1` | 100% (one image pixel per physical pixel) |
| `h` `j` `k` `l` and the arrows | Pan by 10% of the window |
| Shift + `h` `j` `k` `l` | Pan to the edge |
| Left-button drag | Pan, with a `grabbing` cursor from `cursor-shape-v1` |
| `r`, Shift+`r` | Rotate 90° clockwise, counter-clockwise |
| `f`, Shift+`f` | Flip horizontally, vertically |

Rotation and flips are view-only and never written to the file.

### Design

- State: `fit` (bool), `zoom` (image pixels per physical pixel), `center` in image coordinates, and an orientation (one of the eight `wl_output_transform` values, reusing shot's transform code).
- While `fit` is set, the zoom follows the window size. Any zoom or pan clears it, and `0` sets it again.
- Clamp panning so the image never leaves the window when it's larger than the window, and stays centred on an axis where it's smaller.
- Zoom range: from the smaller of 10% and the fit size, up to 3200%.
- Zooming around the pointer keeps the image point under the pointer fixed.
- Key repeat for pan and zoom keys, using the compositor's `repeat_info` and a timerfd (this is where desk's repeat helper comes from).
- Title shows the zoom: `<basename> — W×H — 50%`.
- Performance: render only the visible rectangle. A 50 MP photo at 100% must pan at the frame rate on this laptop. If area averaging is too slow when zoom changes, build a mip chain (halved copies) once at load and sample the nearest larger level.

### As built

- Rotation and flips are applied to the pixels (a tiled copy for rotation, in place for flips), not kept as a `wl_output_transform` for the sampler. Everything after works on an upright image, and the mip levels are rebuilt. On 50 MP that takes about 0.25 s, which is fine for a key press.
- Mip levels are built once at load: each halves the last by 2×2 averaging. Shrinking picks the level just above the zoom and area-averages from it, at most 3×3 source pixels per screen pixel.
- The last frame is kept. When only the position changed, it's shifted and only the strip that scrolled in is rendered, so panning costs about 1 ms on 50 MP at any zoom. A zoom change redraws the window in about 30 ms. The offline test checks that a shifted frame matches a full redraw exactly.
- Panning doesn't clear fit: while fitted, the image is never larger than the window, so there's nothing to pan.
- Zoom steps stop at 100% when they cross it, so `+` and `-` from fit can always reach a pixel-exact view.
- Key repeat went into desk as `DeskRepeat`, a timerfd with no xkbcommon dependency, so it's Layer 0. The keymap itself still lives in view.
- Scrolling: wheel notches use `axis_value120` (`wl_seat` version 8), one √2 step per notch. Touchpad scrolling zooms smoothly, ×2 per 200 logical pixels.

### Manual checks

- Zoom around the pointer keeps the point under it still.
- A 1-pixel checker pattern at 100% shows no blur and no moiré, and at 800% shows sharp squares.
- A large photo at fit shows no aliasing on fine detail (brick, fabric).
- Panning a 50 MP image at 100% is smooth.
- `r` four times returns to the original, and the fit is recomputed for portrait orientation.

---

## 0.3: Browse a directory

### Command line

```text
view [FILE|DIR]...
```

- One file: browse the images in its directory, starting at that file.
- One directory: browse its images, starting at the first.
- Several arguments: browse exactly those (directories are expanded in place).

### Design

- List the directory once with `readdir`, keep names with known extensions (`.png`, `.jpg`, `.jpeg`, case-insensitive), and sort them in natural order (`img2` before `img10`). Content detection still decides the decoder, so a misnamed file opens anyway when it's given explicitly.
- `n`, Space and Right arrow at fit go to the next image; `p`, Backspace and Left arrow at fit go back. `g` and Shift+`g` go to the first and last. The list wraps around.
- The title becomes `<basename> — 3/42 — W×H — 50%`.
- Each image starts in fit mode. Rotation resets per image.
- Decode the next image after the current one is drawn and the event queue is idle, so stepping forward is instant. Keep at most the current, previous and next decoded images.
- An image that fails to decode shows the background and the error in the title, and navigation still works.
- `desk_watch` on the current file: when it's replaced (shot, wall, an editor), reload it and keep the zoom and pan if the size is unchanged.

### As built

- Natural order is glibc's `strverscmp` on the file name, so it's case-sensitive (`B.png` before `a.png`). Fine for camera and screenshot names; revisit if mixed-case folders get annoying.
- Right and Left change images whenever the image fits across the window, not only in fit mode, and pan otherwise. `h` and `l` always pan.
- Preloading runs in the event loop, not a thread: once the current frame has been shown and no events are waiting, one neighbour is decoded. A large JPEG blocks input for the length of its decode (about 0.1 s), which is acceptable.
- Image generations are unique across pictures, so the kept frame from 0.2 is never reused for a different image.
- A rotated or flipped image isn't kept in the cache, so it comes back upright, as "rotation resets per image" asks.
- Reload compares device, inode, size and modification time before decoding, so unrelated events in the directory cost nothing. A new version that fails to decode keeps the old one shown.
- A single `FILE` argument that fails to decode still exits 1, as in 0.1, even though the directory around it may have other images.

### Manual checks

- `view ~/Pictures/Screenshots` steps through every screenshot in name order, and wraps.
- Stepping forward through photos shows no decode delay after the first.
- Re-running `shot` to the same file updates an open view.
- A corrupt file in the middle of a directory doesn't stop browsing.

---

## 0.4: Formats and orientation

- **EXIF orientation:** parse the JPEG APP1 segment directly (only the `Orientation` tag, IFD0, both byte orders). It's about 60 lines, so no libexif. Apply it as the starting orientation. Phone photos must open upright.
- **Transparency:** draw a checkerboard behind images with alpha (`[view] transparency = checker | background`).
- **Decision: more formats.** Each one is a new dependency.

  | Format | Library | Recommendation |
  |---|---|---|
  | GIF (first frame) | giflib | Yes, small and already on most systems |
  | Animated GIF | giflib | Later (after 1.0): needs a frame timer |
  | WebP | libwebp | Yes if web downloads show up often, otherwise after 1.0 |
  | AVIF, HEIC, JXL | libavif, libheif, libjxl | After 1.0, only if needed |
  | SVG | — | No. Not a pixel image viewer's job |

  Formats go through `desk_image_load`, each behind its own `-DDESK_IMAGE_<FORMAT>` flag, so wall only links what it uses.
- **Colour management:** none. Images are treated as sRGB and the screen as sRGB. Note it in the README.

### Manual checks

- A portrait phone photo (EXIF orientation 6) opens upright.
- A PNG with a transparent background shows the checkerboard.
- Every format in the test directory opens, and an unsupported one fails with a message that names the format.

---

## 0.5: Config, theme and desktop integration

- Config through desk's INI parser at `~/.config/view/view.conf`, written from a sample on first run like shot does. Precedence: command line, view.conf, `desktop.conf` once Phase 3 step 3 exists, built-in defaults.

  ```ini
  [view]
  background = palette            ; palette, or #RRGGBB
  transparency = checker          ; checker | background
  fit_enlarge = false             ; enlarge small images to fit the window
  filter = auto                   ; auto | smooth | nearest

  [browse]
  wrap = true
  sort = natural                  ; natural | name | modified
  ```

- `background = palette` uses the palette's `background` role and follows wallpaper changes through `desk_watch`, like deal.
- dwl rule: view floats by default (`{ "view", NULL, 0, 1, -1 }` in `config.h`), sized to the image as in 0.1. Revisit after a week of use; tiled may turn out better.
- Desktop entry `view.desktop` with `MimeType=image/png;image/jpeg;` (plus 0.4's formats) and `NoDisplay=false`, so `run` lists it. `make install` also runs `xdg-mime default view.desktop image/png image/jpeg` when `xdg-mime` exists.
- shot: add `[save] open = false` to shot.conf. When it's true, shot spawns `view FILE` after saving. Off by default.
- `make install` follows bar's `PREFIX`/`BINDIR`/`DESTDIR` conventions. Add `/view/view` to `.gitignore`. Add view to the root README's table.

### Manual checks

- Changing the wallpaper recolors an open view's background within a second.
- `xdg-open photo.jpg` opens view.
- A config typo reports the line number and falls back to defaults.

---

## 1.0: Daily driver

- Every 0.x checklist passes on dwl.
- A week as the default image viewer, with annoyances logged the same way as `docs/deal-papercuts.md`.
- Exit codes documented (0 closed normally, 1 failed to open, 2 usage).
- README documents every option, key and config setting.
- Fuzz `desk_image_load` (including the EXIF parser) with AFL++ or libFuzzer under ASan/UBSan for a few hours, since view opens files from the internet.
- Tag `view-1.0.0`.

## After 1.0 (unscheduled)

- Animated GIF and WebP
- Copy the image to the clipboard (`image/png`, reusing shot's `ext-data-control-v1` code, which would move to desk)
- Set as wallpaper (`wall --set FILE` or by writing wall.conf)
- Delete to trash (freedesktop trash spec) with a confirmation
- An info overlay (EXIF date, camera, file size), once desk's font layer exists
- Slideshow mode
- A thumbnail grid: probably never. That belongs in `files`.

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.1 | One image, fit to window, scale-correct; decoders move from wall to desk | Done (fractional scale check skipped: 1× only for now) |
| 0.2 | Zoom, pan, rotate, filters, key repeat | Done |
| 0.3 | Directory browsing, preloading, reload on change | Code done, checklist pending |
| 0.4 | EXIF orientation, transparency, format decision | Planned |
| 0.5 | Config, palette, dwl rule, desktop entry, install | Planned |
| 1.0 | Checklists, fuzzing, README | Final |
