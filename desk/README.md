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

## Users

| Application | Uses |
|---|---|
| deal | Palette and watch, since 0.12.0 |

net, dwl, bar, run and wall still have their own palette parsers. Moving them is Phase 3 step 1.
