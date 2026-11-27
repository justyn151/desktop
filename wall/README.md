# wall

`wall` is a native Wayland wallpaper utility for dwl with wlroots 0.20.

## Build on Arch Linux

Install `base-devel`, `wayland`, `wayland-protocols`, `wlr-protocols`, `libpng`, `libjpeg-turbo`, `freetype2`, `fontconfig`, and `libxkbcommon`. Install `ttf-dejavu` for the default browser font, or choose an installed font in `wall.conf`. The build uses `wayland-scanner` from `wayland` and XML files from the protocol packages. [Arch Linux package database](https://archlinux.org/packages/)

```sh
make
make install
```

The default install path is `/usr/local/bin/wall`. Use that full path to distinguish it from the system broadcast utility at `/usr/bin/wall`. The Makefile supports `PREFIX`, `BINDIR`, and `DESTDIR`. Use `make clean` to remove build products and `make uninstall` with the same install options to remove the installed binary.

## Configure

Edit `$XDG_CONFIG_HOME/wall/wall.conf` or `$HOME/.config/wall/wall.conf` when `XDG_CONFIG_HOME` is unset, empty, or relative. If the user file is absent, `wall` reads the project’s bundled `wall.conf`. A build embeds the project file as it exists at build time. Settings omitted from an existing file use the built-in values, such as the following.

```ini
[wallpaper]
path =
mode = fill
background = #353535
```

An empty path shows the background color. Set `path = ~/Pictures/My Wallpaper.png` to use an image. The parser accepts spaces within paths and expands leading `~/` using `HOME`. Relative configuration paths resolve from the renderer’s working directory. Lines starting with `#` or `;` are comments, and `background` takes a six-digit RGB value prefixed with `#`. PNG and JPEG are detected by file content. Transparent pixels are composited over the configured background.

| Mode | Behavior |
| --- | --- |
| `fill` | Preserve the aspect ratio and crop to cover the output. |
| `fit` | Preserve the aspect ratio and show the complete image over the background. |
| `stretch` | Resize to the output dimensions. |
| `center` | Keep the image at its original pixel size, centered and clipped at the output edges as needed. |
| `solid` | Show only the background color and ignore the image path. |

The decoder accepts up to 256 MiB of decoded RGBA pixels and 512 MiB of compressed image data per file.

## Wallpaper transitions

CLI changes, browser selections, and configuration reloads use the settings in `[transition]`. The first wallpaper is shown immediately. An existing configuration without this section uses these built-in values.

```ini
[transition]
enabled = true
type = random
duration = 350
fps = 60
easing = ease_in_out
effects = fade, slide, circle, wipe, diagonal, grow
```

| Effect | Behavior |
| --- | --- |
| `none` | Show the new wallpaper immediately. |
| `fade` | Blend from the old wallpaper to the new one. |
| `slide` | Slide both wallpapers together horizontally or vertically. |
| `circle` | Reveal the new wallpaper with an expanding circle and a smooth edge. |
| `wipe` | Reveal the new wallpaper from a monitor edge. |
| `diagonal` | Reveal the new wallpaper from a corner along a smooth diagonal edge. |
| `grow` | Reveal the new wallpaper with an expanding centered rectangle. |
| `random` | Pick one effect from the configured `effects` list. |

`duration` is in milliseconds and accepts 0 through 10,000. Set `enabled = false`, `type = none`, or `duration = 0` for immediate changes. `fps` accepts 1 through 240. `easing` accepts `linear`, `ease_in`, `ease_out`, and `ease_in_out`. The `effects` list must contain one or more distinct animated effects and is used when `type = random`. Reloading transition settings alone updates the next change.

Every participating monitor uses the same effect, direction, and monotonic timeline. Each output uses frames prepared at its own size and scale. Slow outputs can skip intermediate frames and still receive the exact target at completion. An output connected or resized during a transition is drawn at its new geometry using the target wallpaper.

Each animated output uses at most four shared-memory buffers, including its old and target frames. If an intermediate buffer cannot be allocated, the output shows its prepared target. The renderer frees animation buffers after Wayland releases them and stops scheduling frames when the transition ends.

## Wallpaper colors

Set `enabled = true` under `[colors]` to export a shared palette when the renderer starts and after accepted wallpaper changes. When a change animates, the palette is published after the renderer queues the exact final frames. For immediate changes the palette is published before the control command replies. A reload of the color settings can regenerate the palette for the current wallpaper. Failed loads, generation errors, and failed cache writes leave the previous palette in place. Disabling generation leaves the last published palette available.

```ini
[colors]
enabled = false
theme = dark
method = kmeans
samples = 4096
contrast = 4.5
```

| Option | Behavior |
| --- | --- |
| `enabled` | Enable renderer export with `true` or `false`. |
| `theme` | Choose a `dark` or `light` palette. |
| `method` | Use `kmeans`. |
| `samples` | Set a limit from 1 through 32,768 pixels. |
| `contrast` | Set a minimum contrast ratio from 1.0 through 21.0. Text roles use at least 3.0. |

The extractor samples a deterministic grid spread over the original decoded image. It skips fully transparent samples and composites partially transparent samples over `[wallpaper] background`. K-means clusters the visible samples in Oklab using farthest-first initialization, up to eight clusters, and at most 16 iterations. Cluster population, lightness, and saturation determine the base and accent colors. Monochrome images yield tonal variations. A solid wallpaper or a sample with no visible pixels uses the configured background as its source color.

The generator measures contrast using sRGB relative luminance. It adjusts foreground, accent, highlight, urgent, and selection text to meet the effective target against their respective backgrounds. The selection background is also separated from the main background. Muted colors and borders use lower contrast targets to preserve hierarchy. Higher contrast targets can move some colors toward black or white.

The palette is written atomically to `$XDG_CACHE_HOME/wall/colors.conf`, or `$HOME/.cache/wall/colors.conf` when `XDG_CACHE_HOME` is unset, empty, or relative. Sessions using the same cache directory share the file. Its INI section is `[colors]`, and each value is a six-digit `#RRGGBB` color. The keys are `background`, `foreground`, `accent`, `highlight`, `muted`, `selection_background`, `selection_foreground`, `border_active`, `border_inactive`, and `urgent`.

```sh
./wall --colors
./wall --generate-colors "$HOME/Pictures/My Wallpaper.png"
./wall --generate-colors "$HOME/Pictures/My Wallpaper.png" -c /path/to/wall.conf
```

`--colors` prints the last published palette and reports an error if it is missing or invalid. `--generate-colors` prints a palette for the given PNG or JPEG using the configured theme, sample count, contrast, and wallpaper background, regardless of `enabled`. Both commands work without a running renderer. Standalone generation leaves the wallpaper, saved selection, and shared palette unchanged.

## Run and control

Run the renderer from within the dwl session using `./wall` or the installed executable. The launch options are `-c PATH` for an alternative configuration file, `-i PATH` for an image override, `-m MODE` for a mode override, and `-h` for usage. Use the same Wayland session environment for subsequent commands.

The integrated dwl build starts the renderer automatically, restores the
saved wallpaper, and stops its session-owned child at exit. **Super+W**
opens the browser. Repeating the shortcut or launching another
`wall --browse` in the same session leaves the existing browser open.
See the [dwl session setup](../dwl/README.md#wallpaper-session) for build
and launch instructions and the compositor’s live border theme.

```sh
./wall --set "$HOME/Pictures/My Wallpaper.png" --mode fit
./wall --get
./wall --reload
./wall --stop
```

`--set` keeps the current mode unless a mode is supplied. Relative `--set` paths resolve from the control command’s working directory. `--get` prints the current path and mode. Control commands exit after a reply and report an error if the session has no renderer. `--stop` acknowledges shutdown after the renderer removes its socket and releases the session lock.

`--reload` rereads `wall.conf` and any image it selects. `SIGHUP` uses the same reload logic in renderers and requests a directory rescan in browsers. The following command signals every process named `wall` belonging to your user. Use `--reload` for a single renderer session.

```sh
pkill -HUP -u "$(id -u)" -x wall
```

## Saved selection and precedence

Successful `--set` requests save the absolute image path and effective mode to `$XDG_STATE_HOME/wall/selection`. If `XDG_STATE_HOME` is unset, empty, or relative, the location is `$HOME/.local/state/wall/selection`. The renderer writes a private temporary file and atomically renames it into place. The configuration file and its comments are left untouched. Sessions running under the same account share this file, and the last successful `--set` supplies the selection for the next launch.

At startup, each field uses the first available value from this list.

1. Startup `-i` or `-m` overrides.
2. Saved path or mode.
3. `wall.conf`.
4. Built-in defaults.

The background color always comes from `wall.conf` or the built-in default. An unreadable or invalid saved selection falls back to the configured values and startup overrides. An invalid initial configuration or image falls back to the default background.

A successful `--set` becomes the saved selection when the transition starts or when an immediate change is submitted. Explicit reloads apply the current configuration and startup overrides for that running session, leaving the saved selection available for the next launch. A failed decode, buffer preparation, or state write leaves the previously applied selection intact. A `--set` request validates its image even in `solid` mode.

## Outputs and buffer lifetime

Each output has its own background layer-shell surface, with integer or fractional scaling as available. Hotplugged outputs receive the current wallpaper at their own resolution. Disconnected outputs release their surfaces and buffers. The surfaces cover the whole monitor, including under panels, and leave input and window placement to the compositor.

A change decodes one image for all currently configured outputs and prepares their replacement buffers before submitting any of them. An output that has yet to report its geometry receives the active wallpaper when it configures. Static wallpapers remain idle. Wayland release events allow buffer reuse, and decoded pixels are freed after rendering. The renderer keeps a compressed copy so a display change can redraw the last successful image if the source file has since changed.

## IPC protocol

The renderer, CLI, and browser use a Linux `AF_UNIX` `SOCK_SEQPACKET` socket. Its pathname is `$XDG_RUNTIME_DIR/wall-<id>.sock`, where `<id>` is the lowercase, zero-padded, 16-digit hexadecimal FNV-1a 64-bit hash of the canonical Wayland socket pathname. Use `WAYLAND_DISPLAY` or `wayland-0` by default, resolve relative names under `XDG_RUNTIME_DIR`, then resolve symlinks. An inherited `WAYLAND_SOCKET` must have an identifiable absolute peer pathname. The runtime directory must be private to the current user, the socket is mode `0600`, and both peers check the UID. The renderer holds an advisory lock on the adjacent `.sock.lock` file. That file remains available for subsequent launches. An owned stale socket is removed only after the lock is acquired and a connection probe is refused.

Send one packet per connection and read one response. Each field, including the final field, ends with a NUL byte. The notation `\0` below represents one actual zero byte. The maximum packet is 4,223 bytes. Paths are filesystem bytes and must be absolute and at most 4,095 bytes.

| Request | Success reply |
| --- | --- |
| `WALL1\0SET\0<mode>\0<path>\0` | `OK\0` |
| `WALL1\0GET\0` | `OK\0<mode>\0<path>\0` |
| `WALL1\0RELOAD\0` | `OK\0` |
| `WALL1\0STOP\0` | `OK\0` |

Use an empty mode field in `SET` to keep the current mode. An empty path in a `GET` reply means no path is selected. Failures return `ERR\0<message>\0`. The server bounds idle connections and retains only the latest pending wallpaper change. If a request replaces a queued IPC change, the older caller receives an error and its selection is left unapplied. A successful change reply confirms that the target was prepared and, for `SET`, saved. `GET` reports that accepted target while the compositor is still displaying the animation.

`wall.h` declares `wall_ipc_request()`, the CLI’s reusable request function. The saved selection uses the `WALL1` `SET` packet format with the effective mode filled in.

## Wallpaper browser

Start a renderer in the session, then open the browser from another terminal.

```sh
./wall --browse
./wall --browse -c /path/to/wall.conf
```

The browser opens its own resizable xdg-shell window. It reports renderer errors in the window and can still browse files while the renderer is unavailable. Enter or double-click sends the selected path using the existing control socket and retains the renderer’s current scaling mode. The current wallpaper marker changes after success. Closing the browser leaves the renderer running.

| Key or action | Behavior |
| --- | --- |
| Arrows or `h`/`j`/`k`/`l` | Move selection and scroll it into view. |
| Click, double-click, wheel | Select a tile, apply it, or scroll the grid. |
| Enter | Apply the selected wallpaper. |
| `/` or click the search field | Edit the search. |
| Backspace | Erase the last search character while editing. |
| `r` | Rescan directories while outside search editing. |
| Escape | Leave search editing, or close the browser if already outside it. |
| `q` | Close the browser while outside search editing. |

Search matches the filename and its path relative to a configured directory and ignores case. The list is ordered by filename and then by absolute path.

The sample `wall.conf` shows the default `[browser]` and `[appearance]` settings. Older configurations use these defaults automatically. List multiple roots as comma-separated paths. Spaces inside paths and leading `~/` are supported. Relative roots resolve from the browser’s working directory. Set `recursive = false` to scan only the roots. Directory symlinks encountered below a root are skipped, while symlinks to regular image files and hard links are deduplicated by filesystem identity. PNG and JPEG discovery uses file signatures. Unreadable directories are skipped and corrupt thumbnails show as unavailable.

`columns = auto` uses as many columns as fit based on `thumbnail_width`, and a positive number sets an optional cap. The browser starts at 1352 × 624 logical pixels and fits four columns with the default 312 × 176 thumbnails. Wider windows can show more columns. The grid divides the available width evenly. The integrated dwl build opens the browser as a centered floating window and reduces its starting size on smaller outputs. `thumbnail_width` and `thumbnail_height` take values from 32 through 512 pixels. `font` is a Fontconfig name, `font_size` takes values from 6 through 48 pixels, and appearance colors use `#RRGGBB`. Set `close_on_apply = false` to keep browsing after a successful change.

Discovery, thumbnail decoding, and renderer requests run in separate workers. The browser loads visible thumbnails on demand, releases full-size pixels after thumbnail generation, and keeps at most 128 thumbnail entries or 24 MiB of thumbnail pixels in memory. Discovery is capped at 100,000 entries and 32 MiB of catalog data. The browser reports when those limits are reached.

With `cache = true`, up to 1,024 reusable thumbnail slots are stored under `$XDG_CACHE_HOME/wall/thumbnails/`, falling back to `~/.cache/wall/thumbnails/`. Cache keys track file identity, size, timestamps, and thumbnail dimensions. The cache stores transparent pixels independently of the browser background. Older thumbnail entries rebuild once on demand to gain transparency. Linux file notifications invalidate active thumbnail entries after source edits. Set `cache = false` to disable the disk cache.

### Wallpaper-based browser colors

Set the following in `wall.conf` to theme the browser with the published palette.

```ini
[theme]
source = wallpaper
```

The browser loads `background`, `foreground`, `highlight`, and `muted`
from the shared `colors.conf`. An open browser picks up palette
replacements after the renderer publishes them at the end of a wallpaper
change. Existing thumbnails are reused. Set `close_on_apply = false`
if you want the browser to remain open after applying a wallpaper.

If the palette is missing or invalid on startup, the browser uses its
`[appearance]` colors. A failed live update preserves the last valid
colors. Check `[colors] enabled = true`, the cache directory shared by
the browser and renderer, and the output of `wall --colors`. Set
`source = manual` and reopen the browser to restore configured colors.
Manual is the default and applies only to the browser. Palette generation
is controlled independently by `[colors]`.
