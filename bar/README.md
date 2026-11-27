# bar

`bar` is a native Wayland bar for dwl. It reserves space on one output and
draws workspace labels on the left and a network indicator beside the clock
on the right. By default it spans the top edge at 24 logical pixels with a
translucent background.

## Build and launch

On Arch Linux the build uses `gcc`, `make`, `pkgconf`, `wayland`,
`wayland-protocols`, `wlr-protocols`, `freetype2`, `fontconfig`, and
`systemd-libs` (providing `libsystemd`).
The default font comes from `ttf-dejavu`. Fontconfig resolves configured
font families and FreeType renders the text.

```sh
make
make install
dwl -s 'exec bar'
```

Use the path to your IPC-enabled dwl binary if it is outside your `PATH`.
With dwl IPC, you can also run `./bar` inside an existing session. The bar
uses IPC state for the output it occupies. Vanilla dwl provides read-only
workspace state through stdin when launched with `-s`. Without either
source, the clock still works and workspace labels have the inactive color.

Left-click a workspace to switch to it. Right-click toggles it in the
current view while keeping at least one workspace active. Scroll up over a
workspace label to move forward and down to move backward, wrapping at
either end of the configured labels. With several workspaces selected,
scrolling forward starts from the highest selected index and scrolling
backward starts from the lowest.

`make install` installs to `/usr/local/bin/bar` by default.
`make uninstall` removes the binary and `make clean` removes build products.
The install targets honor `PREFIX`, `BINDIR`, and `DESTDIR`.

## Configuration

Install the sample `bar.conf` from the project directory:

```sh
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/bar"
mkdir -p "$config_dir"
cp bar.conf "$config_dir/bar.conf"
```

The bar reads that path by default. Use `bar -c /path/to/bar.conf` to
choose another file. When launching through dwl, for example, use
`dwl -s 'exec bar -c /path/to/bar.conf'`. A missing file
uses the project’s `bar.conf` bundled at build time. Built-in defaults
fill in settings omitted from the file that was loaded.

| Section | Options |
| --- | --- |
| `[bar]` | `height` (1–4096 logical pixels), `position` (`top` or `bottom`), `background`, `opacity` (0–1), `scale` (positive multiplier, default 1.0), `foreground` (network and clock color), `padding` (0–4096 pixels per side) |
| `[font]` | `family` (an installed Fontconfig family or a generic family such as `monospace`), `size` (1–256 pixels) |
| `[workspaces]` | `labels` (1–32 comma-separated labels in tag order, up to 63 bytes each), `hide_empty` and `underline_active` (`true` or `false`), `active_color`, `occupied_color`, `inactive_color`, `urgent_color`, `spacing` (0–4096 pixels) |
| `[clock]` | `format` (a `strftime` format, including glibc extensions such as `%l` and `%P`) |
| `[network]` | `enabled`, `show_ssid`, `show_strength`, `click_enabled` (`true` or `false`), `custom_text` (Wi-Fi label), `separator` (up to 63 bytes each, quote to preserve spaces), `command` (up to 1023 bytes) |

Colors use `#RRGGBB`. Full-line comments begin with `#` or `;`. Spaces
inside values are preserved. The clock uses the process locale and local
time. The network and clock share a right-aligned text group, and the
workspace list stops before it.

`scale` multiplies the height, font size, padding, and workspace spacing.
Each result rounds to the nearest pixel and must fit its listed range.

The network indicator follows NetworkManager on the system bus. It shows
`WiFi 82%`, `Ethernet`, `Connected` for other active connection types,
or `Offline` when disconnected or when NetworkManager is unavailable.
`show_ssid` adds the Wi-Fi network name, `show_strength` controls the
signal percentage, and `enabled = false` removes the indicator and its
separator. Set `custom_text = ""` to omit the Wi-Fi label while retaining any
enabled SSID or strength. The default separator is `" | "`. [NetworkManager D-Bus API](https://www.networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html)

Left-click the network label to open `net` in deal. The default is
`command = deal --app-id net --columns 84 --rows 26 -e net`. Set `click_enabled = false` to disable
launching. Commands are split on whitespace, with single or double quotes
for grouped arguments and backslashes to escape characters outside single
quotes. Shell expansion and pipelines are unavailable. Use a foreground
command so bar can suppress repeat launches until it exits. deal and `net`
must be on the session's `PATH` for the default command. See the
[net installation and dwl rule notes](../net/README.md#bar-and-dwl-integration).

After editing the file, reload the running bar:

```sh
pkill -HUP -x bar
```

The bar keeps its Wayland and IPC connections and updates its height,
anchor, exclusive zone, and workspace hit regions on the next available
frame. Configuration and font errors are reported to stderr. A failed
reload leaves the current appearance in place.

## Wallpaper-based colors

Set the following in `bar.conf` to use the palette published by `wall`.

```ini
[theme]
source = wallpaper
```

The bar uses the palette's `background` and `foreground`. Workspace colors
use `highlight` for active, `accent` for occupied, `muted` for inactive, and
`urgent` for urgent. The bar watches the palette's containing directory and
applies color changes live. Missing or invalid updates leave its last
working colors in place.

The bar starts with its configured manual colors if the palette is
unavailable. Check that `wall` has `[colors] enabled = true`, that both
apps see the same `$XDG_CACHE_HOME/wall/colors.conf` or
`$HOME/.cache/wall/colors.conf`, and that `wall --colors` prints the
palette. Set `source = manual` and reload the bar configuration with
`SIGHUP` to restore the configured colors. Manual is the default.

## dwl IPC and rollback


The [community IPC protocol](https://codeberg.org/dwl/dwl-patches/src/branch/main/patches/ipc)
was ported from its dwl 0.9 patch to the local dwl 0.10-dev revision
`f590014`. The port keeps the existing stdout stream and keybindings.

- In `dwl`, `dwl.c` publishes per-output state and handles IPC requests.
  `Makefile` generates server bindings from `protocols/dwl-ipc-unstable-v2.xml`.
- In `bar`, `bar.c` associates the surface with an IPC output and dispatches
  pointer actions using the rendered hit regions. `bar.h` declares the module
  callback slots. `Makefile` generates client bindings from the matching XML
  under `protocols/`.

To revert the compositor port, first save any subsequent work in `dwl.c` and
`Makefile` that you want to keep. From the dwl directory, restore those two
files to the pre-IPC revision, remove the IPC XML and generated bindings, then
rebuild with your existing `config.h`.

```sh
git restore --source=f590014 -- dwl.c Makefile
rm -f protocols/dwl-ipc-unstable-v2.xml dwl-ipc-unstable-v2-protocol.h dwl-ipc-unstable-v2-protocol.c
make
```

The bar keeps its own protocol XML and works with vanilla dwl via the
read-only stdin fallback.
