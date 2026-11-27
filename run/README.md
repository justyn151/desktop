# run

`run` is a native Wayland command launcher for dwl. It opens a single row
across the selected monitor and holds keyboard focus until it closes.
The customized dwl temporarily hides the bar on that monitor and places
`run` at the bar's edge. The bar keeps its reserved space, so tiled windows
stay in place. If there is no bar on the selected monitor, `run` opens at
the top. The row shows a prompt, the search input and cursor, a page of
matches, and the matching/total command count.

## Build and launch

On Arch Linux, the build uses `gcc`, `make`, `pkgconf`, `wayland`,
`wayland-protocols`, `wlr-protocols`, `libxkbcommon`, and `freetype2`.
The runtime needs `wayland`, `libxkbcommon`, `freetype2`, and the configured
font. The default font comes from `ttf-dejavu`.
[Arch layer-shell protocol files](https://archlinux.org/packages/extra/any/wlr-protocols/files/)
· [Arch DejaVu package](https://archlinux.org/packages/extra/any/ttf-dejavu/)

Build in the project directory and launch from a terminal in the dwl session.

```sh
make
./run
./run -c ./run.conf
```

`make install` installs the executable to `/usr/local/bin/run` by default.
`make uninstall` removes it. Both accept `PREFIX`, `BINDIR`, and `DESTDIR`
overrides. `make clean` removes the executable, object files, and generated
protocol bindings. The build uses `wayland-scanner` to generate the
layer-shell bindings and the xdg-shell protocol data referenced by them.
Set `WLR_PROTOCOLS_DIR` or `WAYLAND_PROTOCOLS_DIR` to override the XML locations.

## Keyboard controls

| Key | Action |
| --- | --- |
| Left | Select the previous result |
| Right, Ctrl+J | Select the next result |
| Tab | Copy the selected result's name into the input |
| Backspace | Delete the UTF-8 character before the cursor |
| Ctrl+A | Move the cursor to the start of the input |
| Ctrl+E | Move the cursor to the end of the input |
| Ctrl+K | Delete from the cursor to the end of the input |
| Ctrl+U | Clear the input |
| Enter | Launch the selected result |
| Shift+Enter | Launch the typed command and its arguments |
| Escape | Close the launcher |

The catalog includes desktop applications and regular executable files on
`PATH`, including symlinks to such files. An empty `PATH` component denotes
the current directory. The first executable entry for a duplicate name wins.
Raw commands and desktop entries remain separately selectable, even if they
have the same display name.

Search matches the application name, the program named by `Exec`, the generic
name, and each keyword. It is case-insensitive by default and ranks exact,
prefix, substring, and fuzzy matches in that order. A fuzzy match needs every
query character to occur in order within one field. For example, `ffx`
matches Firefox and `obsstu` matches OBS Studio.

Fuzzy scoring rewards consecutive characters, matches at the beginning of
a field, and matches at word boundaries. It penalizes each gap and the extra
characters it adds to the matching span. Ordinary fields use the best-scoring
alignment. Large inputs use a bounded number of scans to keep search
responsive. At equal match quality, matches in application and executable
names come before matches in generic names or keywords. Remaining ties use
the desktop-entry preference and then alphabetical order. Empty queries
show all results in your locale's alphabetical order.

Set `fuzzy = false` for exact, prefix, and substring search only.
Set `case_sensitive = true` to require matching capitalization in every
search tier. Moving past the edge of the visible results changes the page.
Long input scrolls to keep the cursor visible.

For Shift+Enter, type a case-sensitive command name from `PATH` or an
explicit path. For example, `deal --title "notes window"` passes two arguments
to `deal`. Single and double quotes preserve spaces, adjacent quoted and
unquoted text forms one argument, and a backslash escapes the next
character outside single quotes. Empty quoted arguments are preserved.
`run` executes the program directly. Shell operators such as `|` and `>`
are literal arguments, and shell expansions do not run. The launcher closes
as soon as the program starts in its own session. Syntax and launch errors
appear in the launcher so you can correct the command.
Programs launched directly by `run` get end-of-file on standard input.
Launch interactive commands inside a terminal emulator.

## Desktop applications

`run` scans the `applications` directory under `$XDG_DATA_HOME`, defaulting
to `$HOME/.local/share`. It then scans the `applications` directories under
`$XDG_DATA_DIRS` in order, defaulting to `/usr/local/share` and `/usr/share`.
Nested files use desktop file IDs. For example, `tools/editor.desktop` has
the ID `tools-editor.desktop`. The first file for an ID wins, including a
user-local `Hidden=true` override that suppresses a system entry.
[XDG Base Directory Specification](https://specifications.freedesktop.org/basedir/latest/),
[Desktop file IDs](https://specifications.freedesktop.org/desktop-entry/latest/file-naming.html)

Localized names, generic names, and keywords follow `LC_MESSAGES`.
`OnlyShowIn` and `NotShowIn` use `XDG_CURRENT_DESKTOP`. Entries are skipped
if they are hidden, malformed, marked `NoDisplay`, or require an unavailable
executable, working directory, or terminal.
Discovery skips desktop files larger than 1 MiB and caps expanded arguments
at 128 KiB per entry.
[Desktop entry keys](https://specifications.freedesktop.org/desktop-entry/latest/recognized-keys.html)

Enter launches a desktop entry's `Exec` arguments directly and honors its
`Path` working directory. It expands `%c` to the localized name, `%k` to the
desktop file path, `%i` to the icon arguments when present, and `%%` to a
literal percent sign. File and URL placeholders are removed because `run`
starts applications without files or URLs. D-Bus-activatable entries use
their `Exec` fallback. Entries without a usable fallback are skipped.
[Desktop entry Exec rules](https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html)

`Terminal=true` entries open in deal by default (`terminal = deal -e`). Set
`terminal` to a different executable and any fixed options it needs before
the command and its arguments. For example, use `terminal = xterm -e`. A matching
PATH command or a command typed with Shift+Enter also uses the terminal,
with the arguments supplied by the user. Detached launches keep the
login console out of their standard streams.

### Flatpak and Steam

Flatpak export directories listed in `XDG_DATA_DIRS` keep their configured
order. As fallbacks, `run` checks `$XDG_DATA_HOME/flatpak/exports/share`,
`$HOME/.local/share/flatpak/exports/share`, and
`/var/lib/flatpak/exports/share`. It follows symlinks to exported desktop
files and applies the same desktop file ID precedence. Flatpak's generated
`Exec` quoting is decoded into arguments, and empty file-forwarding groups
are removed when launching without files or URLs.
[Flatpak export conventions](https://docs.flatpak.org/en/latest/conventions.html),
[Flatpak file forwarding](https://docs.flatpak.org/en/latest/flatpak-command-reference.html)

Steam game shortcuts in the standard application directories launch through
their desktop entry. `run` passes the Steam URI and its arguments through,
including percent-encoded bytes in the URI. Different desktop file IDs stay
separate even when their entries all use `steam` or `flatpak`. A shortcut
needs an available launcher named by its `Exec` key. For Flatpak Steam,
that can be `flatpak run com.valvesoftware.Steam` followed by the URI.
[Steam for Linux shortcut format](https://github.com/ValveSoftware/steam-for-linux/issues/11012)

## Configuration

Copy [run.conf](run.conf) to `$XDG_CONFIG_HOME/run/run.conf` or
`$HOME/.config/run/run.conf`. The launcher tries the XDG location first
and uses the home location if the XDG file is absent. If both are absent,
it uses the project’s `run.conf` bundled at build time. `run -c PATH`
reads the given file.
Each setting is optional. Blank lines and whole-line comments beginning
with `#` or `;` are allowed.

| Section | Option | Default | Meaning |
| --- | --- | --- | --- |
| `[appearance]` | `background` | `#353535` | Row background |
| | `foreground` | `#c5ad81` | Input and unselected result text |
| | `highlight` | `#ead4aa` | Prompt, cursor, and error text |
| | `muted` | `#887d69` | Result count |
| | `selected_background` | `#c5ad81` | Selected result background |
| | `selected_foreground` | `#353535` | Selected result text |
| | `opacity` | `0.78` | Background opacity from 0 to 1 |
| | `height` | `24` | Row height in logical pixels, from 1 to 512 |
| | `padding` | `8` | Horizontal padding in logical pixels, from 0 to 512 |
| | `scale` | Inherited | Positive multiplier for logical pixel dimensions |
| `[font]` | `file` | `/usr/share/fonts/TTF/DejaVuSansMono.ttf` | Font file loaded by FreeType |
| | `size` | `14` | Font size in logical pixels, from 1 to 256 |
| `[layout]` | `prompt` | `run:` | Prompt text. An empty value hides it |
| | `input_width` | `240` | Preferred input width in logical pixels, from 1 to 8192 |
| | `result_spacing` | `12` | Gap between results in logical pixels, from 0 to 512 |
| `[applications]` | `desktop_entries` | `true` | Discover and show desktop applications |
| | `path_commands` | `true` | Show raw PATH commands. Shift+Enter still resolves commands on PATH |
| | `prefer_desktop_entries` | `true` | Prefer desktop entries at equal match quality |
| | `terminal` | `deal -e` | Terminal executable and optional fixed arguments |
| `[search]` | `case_sensitive` | `false` | Require matching capitalization |
| | `fuzzy` | `true` | Include subsequence matches after all direct matches |

Set `[appearance]` `scale` to override the bar. Otherwise `run` reads
`[bar]` `scale` from `$XDG_CONFIG_HOME/bar/bar.conf`, or
`$HOME/.config/bar/bar.conf` when `XDG_CONFIG_HOME` is unset or empty.
Without a usable bar scale, it uses `1.0`. If the bar uses a custom
`-c` path, set the same scale explicitly in `run.conf`.
Scale multiplies the height, padding, font size, input width, and result
spacing before Wayland output scaling. Each result rounds to the nearest
logical pixel and must fit the range in the table.

## Wallpaper-based colors

Set the following in `run.conf` to use the palette published by `wall`.

```ini
[theme]
source = wallpaper
```

Each launcher invocation reads the background, foreground, highlight,
muted text, and both selected-result colors from
`$XDG_CACHE_HOME/wall/colors.conf`. If `XDG_CACHE_HOME` is unset, empty,
or relative, it uses `$HOME/.cache/wall/colors.conf`.

If the palette is missing or invalid, the launcher uses its configured
colors. Check that `wall` has `[colors] enabled = true`, that the
programs share the same cache directory, and that `wall --colors` prints
a complete palette. Set `source = manual` to restore your configured
colors on the next launch. Manual is the default.
