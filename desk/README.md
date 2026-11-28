# desk

Shared code for the desktop applications: one `desk.c` and one `desk.h`. Each application compiles `desk.c` into its own binary, so there's no shared library to install. See [PLAN.md](../PLAN.md#phase-3-design-desk-and-migrate-to-it) for the full design and migration order.

Rules: no event loop, no global state, no widgets, and no printing. Functions return errors and the caller reports them. Nothing in here is specific to one application.

## Palette

wall publishes the wallpaper palette in `$XDG_CACHE_HOME/wall/colors.conf`, or `~/.cache/wall/colors.conf` when `XDG_CACHE_HOME` is unset or relative.

| Function | Purpose |
|---|---|
| `desk_palette_path()` | Return the palette path as a new string, or `NULL`. |
| `desk_palette_load(path, &palette)` | Read the ten roles from `[colors]` as `0xRRGGBB` values. Return 0, an `errno` value, or `EINVAL` for a missing role, duplicate or bad color. Unknown keys and sections are ignored so wall can add roles without breaking readers. |
| `desk_role_name(role)` | The key for a `DESK_*` role, such as `selection_background`. |

## Watching files

`desk_watch_open` watches a file that is replaced atomically, which is how wall writes the palette. It watches the file's directory, or the nearest existing ancestor until the directory appears. Poll `watch.fd` for `POLLIN`, then call `desk_watch_changed(&watch, revents)`, which drains the events and returns whether the file may have changed. Close it with `desk_watch_close`.

## Atomic files

`desk_atomic_open(&atomic, path, mode)` creates a temporary file next to `path` with permissions `mode & ~umask`. Write to `atomic.file`, then call `desk_atomic_commit(&atomic, replace)`, which flushes, syncs and renames it onto `path`. With `replace` false an existing target is left alone and `EEXIST` is returned with the temporary file kept, so the caller can pick another name with `desk_atomic_retarget` and commit again. `desk_atomic_abort` removes the temporary file and is safe to call after a commit. Every function returns 0 or an `errno` value.

## Shared memory

`desk_shm_open(size)` returns a sealed `memfd` of `size` bytes for a `wl_shm` pool, or -1 with `errno` set.

## Config and paths

| Function | Purpose |
|---|---|
| `desk_config_path(app)` | `$XDG_CONFIG_HOME/<app>/<app>.conf`, or `~/.config/<app>/<app>.conf` |
| `desk_expand_home(path)` | Expand a leading `~/` with `HOME` |
| `desk_mkdirs(path, mode)` | `mkdir -p` |
| `desk_write_default(path, data, length)` | Write a sample file, creating parent directories, unless it already exists (`EEXIST`) |
| `desk_ini_parse(text, length, handler, data)` | Call `handler(data, section, key, value, line)` per `key = value`. Values are trimmed and one pair of surrounding quotes is removed. Comments are whole lines starting with `#` or `;`. Malformed lines reach the handler with `key` and `value` NULL, so the app reports them with the line number. |
| `desk_ini_load(path, handler, data)` | Read a file of up to 1 MiB and parse it |
| `desk_parse_bool`, `desk_parse_rgb` | `true`/`false` (also yes/no, on/off, 1/0) and `#RRGGBB` |

This is Phase 3 step 2's parser. The other apps still have their own.

## Key repeat

`DeskRepeat` is a timerfd that fires while a key is held. `desk_repeat_open` creates it (25 per second after 600 ms until told otherwise), `desk_repeat_info` takes `wl_keyboard.repeat_info`, `desk_repeat_start(key)` arms it after the delay, and `desk_repeat_release(key)` or `desk_repeat_cancel` stops it. Poll `repeat.fd`, then `desk_repeat_fired` returns how many repeats are due. It knows nothing about keymaps: the caller decides which keys repeat and what they do. This is part of Phase 3 step 6; compose stays in the apps for now.

## UTF-8 and the canvas

`desk_utf8_next(&text, end)` decodes one code point and advances past it. Every byte of an invalid, truncated, overlong or surrogate sequence decodes as U+FFFD, so any byte string, such as a file name, can be drawn.

`DeskCanvas` is a premultiplied ARGB buffer with a clip rectangle. `desk_canvas(pixels, width, height)` covers a whole buffer, `desk_canvas_clip` sets the clip (intersected with the buffer, not with the previous clip), `desk_canvas_fill(&canvas, x, y, width, height, 0xAARRGGBB)` blends a rectangle in, replacing when alpha is `0xff`, and `desk_canvas_blit(&canvas, x, y, pixels, width, height)` composites premultiplied ARGB pixels, such as a decoded image. Text drawing (below) draws into the same canvas.

## Keyboard (`-DDESK_KEYBOARD`)

`DeskKeyboard` holds the xkb keymap and modifier state from `wl_keyboard`. `desk_keyboard_open` creates the context, `desk_keyboard_keymap(&keyboard, format, fd, size)` takes the keymap event (always closing `fd`, keeping the old keymap on failure) and `desk_keyboard_modifiers` the modifiers event. `desk_keyboard_sym(&keyboard, key)` returns the keysym for an evdev key code, already shifted (Shift+g is `G`), `desk_keyboard_mods` the active `DESK_MOD_SHIFT`, `_CTRL`, `_ALT` and `_LOGO`, and `desk_keyboard_repeats` whether the keymap lets a key repeat. It needs `xkbcommon` but not `wayland-client`. This is part of Phase 3 step 6; compose is still to come.

## Fonts (`-DDESK_FONT`)

`desk_font_open(pattern, pixel_size, error, size)` matches a fontconfig pattern such as `sans-serif` or `monospace:bold` and keeps fontconfig's whole sorted fallback list. A character the first face lacks comes from the first face in the list that has it, so Nerd Font icons and other scripts work through the system's fallback configuration. Faces are loaded the first time they're needed. Glyphs are rendered with FreeType's light hinting into a cache of grayscale coverage, one entry per code point.

| Function | Purpose |
|---|---|
| `desk_font_resize(font, pixel_size)` | Change the size, such as when the window's scale changes, and clear the cache |
| `desk_font_size`, `desk_font_metrics` | The size, and the ascent, descent and line height in pixels |
| `desk_text_width(font, text, length)`, `desk_char_width(font, code)` | Advance widths in pixels |
| `desk_text_fit(font, text, length, max_width)` | How many bytes fit, ending on a character boundary |
| `desk_text_draw(font, &canvas, x, baseline, text, length, 0xAARRGGBB)` | Draw inside the canvas's clip and return the x after the text |

Control characters draw as U+FFFD. There's no shaping or kerning: text is laid out glyph by glyph, which suits names and labels. It needs `freetype2` and `fontconfig`. This is Phase 3 step 5's font layer; bar, run and wall still have their own copies.

## shm buffers (`-DDESK_WAYLAND`)

`DeskBufferPool` holds two `wl_shm` buffers. `desk_buffer_get(&pool, shm, width, height, format)` returns one the compositor has released, reallocating it when the size changed, and marks it busy. Attach and commit it. It returns NULL when both are busy. `desk_buffer_pool_finish` frees both. Compiling with `-DDESK_WAYLAND` needs `wayland-client`.

## Windows (`-DDESK_WINDOW`)

A plain xdg-shell toplevel with scale tracking, on top of `-DDESK_WAYLAND`. The application generates the xdg-shell, viewporter and fractional-scale-v1 client code and puts the headers on the include path, since desk includes them.

- `DeskGlobals` holds `wl_compositor`, `wl_shm`, `xdg_wm_base` (whose pings it answers), and optionally `wp_viewporter` and `wp_fractional_scale_manager_v1`. Call `desk_globals_bind` first in the registry's `global` handler: it returns true when it took the global. After the roundtrip, `desk_globals_missing` names a required global that's missing, or returns NULL. `desk_globals_finish` destroys them.
- `desk_window_open(&window, &globals, app_id, &listener, data)` creates the surface, viewport, fractional scale and toplevel. The listener's `draw` is called whenever a frame is needed: after each configure, when the scale changes, and after `desk_window_redraw`, but at most once per frame callback. Inside it, `desk_window_buffer(&window, format)` returns a free buffer at the window's size in buffer pixels (or NULL, which leaves the window dirty for later), and `desk_window_commit(&window, buffer)` attaches, damages and commits it. `close` reports the compositor's close request, and the optional `initial_size` picks a size when the compositor leaves it to the window.
- `desk_window_scale` is the buffer pixels per logical pixel: the fractional scale through the viewport, or the integer buffer scale. `desk_window_buffer_size` gives the buffer size.
- Call `desk_window_update` after dispatching events each time round the loop, so a frame skipped for lack of a free buffer is drawn once one is released. `desk_window_idle` says whether the window is drawn with no frame pending, which is a good time for background work.
- The struct's fields (`width`, `height`, `bounds_width`, `bounds_height`, `fraction`, `buffer_scale`, `configured`, `activated`) are readable.

This covers Phase 3 step 4 for toplevels; layer-shell setup is still in each app.

## Images (`-DDESK_IMAGE`)

`desk_image_load(file, &image, max_bytes, flags, error, size)` decodes a PNG or JPEG, detected by content (plus GIF with `-DDESK_IMAGE_GIF`, built in, and WebP with `-DDESK_IMAGE_WEBP`, which needs `libwebp`), from a seekable `FILE` into premultiplied RGBA8 (`DeskImage`, stride `width * 4`, freed with `free(image.pixels)`). 16-bit, palette, grayscale and interlaced PNGs, progressive JPEGs and CMYK JPEGs are converted. Warnings and recoverable damage, such as a truncated JPEG, count as failures. An image whose pixels would exceed `max_bytes` fails with `File too large`, unless `DESK_IMAGE_SHRINK` is set and it's a JPEG, which is then decoded at 1/2, 1/4 or 1/8 size. `image.orientation` is the JPEG's EXIF orientation (1–8, 1 when absent), reported but not applied. On failure it returns false and writes a message such as `PNG: CRC error` to `error`; a format it recognises but doesn't decode is named, as in `AVIF images aren't supported`. Compiling with `-DDESK_IMAGE` needs `libpng` and `libjpeg`.

## Users

| Application | Uses |
|---|---|
| deal | Palette and watch, since 0.12.0 |
| shot | Atomic files and shared memory since 0.1; config, INI and shm buffers since 0.2 |
| wall | Image decoding (its own decoders moved here for view) |
| view | Image decoding and shm buffers since 0.1; key repeat since 0.2; file watch since 0.3; GIF, WebP and EXIF orientation since 0.4; palette, INI config and paths since 0.5; window and keyboard since files 0.1 |
| files | Window, keyboard, fonts, canvas, key repeat, palette and file watch since 0.1; image decoding since 0.2 |

net, dwl, bar, run and wall still have their own palette parsers. Moving them is Phase 3 step 1.
