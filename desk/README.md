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

## Users

| Application | Uses |
|---|---|
| deal | Palette and watch, since 0.12.0 |
| shot | Atomic files and shared memory, since 0.1 |

net, dwl, bar, run and wall still have their own palette parsers. Moving them is Phase 3 step 1.
