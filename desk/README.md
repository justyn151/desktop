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

## shm buffers (`-DDESK_WAYLAND`)

`DeskBufferPool` holds two `wl_shm` buffers. `desk_buffer_get(&pool, shm, width, height, format)` returns one the compositor has released, reallocating it when the size changed, and marks it busy. Attach and commit it. It returns NULL when both are busy. `desk_buffer_pool_finish` frees both. Compiling with `-DDESK_WAYLAND` needs `wayland-client`.

## Images (`-DDESK_IMAGE`)

`desk_image_load(file, &image, max_bytes, flags, error, size)` decodes a PNG or JPEG, detected by content, from a seekable `FILE` into premultiplied RGBA8 (`DeskImage`, stride `width * 4`, freed with `free(image.pixels)`). 16-bit, palette, grayscale and interlaced PNGs, progressive JPEGs and CMYK JPEGs are converted. Warnings and recoverable damage, such as a truncated JPEG, count as failures. An image whose pixels would exceed `max_bytes` fails with `File too large`, unless `DESK_IMAGE_SHRINK` is set and it's a JPEG, which is then decoded at 1/2, 1/4 or 1/8 size. On failure it returns false and writes a message such as `PNG: CRC error` to `error`. Compiling with `-DDESK_IMAGE` needs `libpng` and `libjpeg`.

## Users

| Application | Uses |
|---|---|
| deal | Palette and watch, since 0.12.0 |
| shot | Atomic files and shared memory since 0.1; config, INI and shm buffers since 0.2 |
| wall | Image decoding (its own decoders moved here for view) |
| view | Image decoding and shm buffers since 0.1; key repeat since 0.2 |

net, dwl, bar, run and wall still have their own palette parsers. Moving them is Phase 3 step 1.
