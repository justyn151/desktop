# deal 0.10.5

Experimental native Wayland milestone for Linux and dwl. deal uses xdg-shell for the window, EGL and desktop OpenGL for the existing fixed-function renderer, FreeType and Fontconfig for fonts, and a Linux PTY for the shell or command. Frames are drawn on changes and paced by the compositor. XWayland and an X server are unnecessary.

Version 0.10.5 adds bold, italic and bold italic font faces, underline styles and colors, strikethrough, overline and concealed text. Version 0.10.3 added text selection, native clipboard and optional primary selection, bracketed paste, application mouse reporting, and OSC 8 link activation.

## Build on Arch Linux

| Purpose | Packages |
|---|---|
| Compiler, Make and pkg-config | `gcc`, `make`, `pkgconf` |
| Wayland client, EGL window glue, scanner and protocol XML | `wayland`, `wayland-protocols` |
| Desktop OpenGL and EGL | `libglvnd` and a compatible graphics driver such as `mesa` |
| Fonts and keyboard input | `freetype2`, `fontconfig`, `libxkbcommon`, and a monospace font such as `ttf-dejavu` |
| Optional custom terminfo compiler | `ncurses` for `tic` |
| Optional HTTP/HTTPS opener | `xdg-utils` for `xdg-open` |

Linux's libc and libutil provide PTY support. Make generates bindings for xdg-shell and `primary-selection-unstable-v1` in `.build/`. Use `make clean` to remove the executable and generated bindings.

From the repository root:

```sh
cd deal
make
```

From a terminal in your dwl session:

```sh
./deal
./deal --diagnostics
./deal --title "Color check" --working-directory "$PWD" -e sh -c 'printf "\033[32mdeal\033[0m\n"; sleep 5'
```

Optional installation uses `make install`. The default prefix is `$HOME/.local`. Installation adds the executable, desktop launcher, Xfce preferred-terminal helper, the configuration template under `share/deal/deal.conf`, and custom terminfo. The Makefile generates the desktop and terminfo files during installation. The launcher's desktop file matches the default application ID `deal`. The launcher uses the installed executable path.

For a system-wide install, build first and use `sudo make PREFIX=/usr/local install`. Set `DESTDIR` for package staging. Use `make uninstall` with the same prefix and staging root. The installer tracks its files and preserves modified files on uninstall.

## Command line

| Argument | Effect |
|---|---|
| `--help`, `--version` | Print usage or version and exit. |
| `--config PATH` | Use an alternate configuration file at startup and on reload. |
| `--working-directory PATH` | Set the child's starting directory. |
| `--title TITLE` | Override the configured title. Enabled OSC titles can still change it. |
| `--app-id ID`, `--class ID` | Override the Wayland application ID. `--class` is an alias for existing launch commands. |
| `--columns N`, `--rows N` | Request an initial grid size. A compositor configure can supersede the requested size. |
| `--diagnostics` | Probe Wayland, xdg-shell, EGL and the OpenGL renderer. Report clipboard protocol availability, configuration path, font and TERM without launching a child. |
| `-e COMMAND [ARGS...]` | Run a program with the remaining arguments and return its exit status. Place this option last. |

## Configuration and migration

The default path is `$XDG_CONFIG_HOME/deal/deal.conf`. When `XDG_CONFIG_HOME` is unset, empty, or relative, deal uses `~/.config/deal/deal.conf`. An absent default file uses built-in defaults at startup. If the default file cannot be parsed, deal reports the error and uses defaults. If its font spacing produces invalid cell dimensions, deal reports the error and uses default spacing. An unreadable or invalid file selected by `--config` fails startup.

Copy [deal.conf](deal.conf) to that path and transfer your settings from `init.lua`. Each former Lua table maps to an INI section with the same name. Move `window.padding.horizontal` and `window.padding.vertical` to `padding_horizontal` and `padding_vertical` under `[window]`. Put numeric `colors.palette` entries under `[palette]` using the same indices from 1 through 16. Omit `terminal.shell` to use `$SHELL` and then `/bin/sh`. Resolve any dynamic Lua expressions into literal values. Existing user Lua files are left in place.

Section names and keys are case-sensitive. Use `key = value` with optional surrounding whitespace. Strings can be bare or enclosed in single or double quotes. Quoted content is literal. Booleans use `true` and `false`. Opacity accepts a decimal from `0.0` to `1.0`. Colors use `#RRGGBB`. Blank lines and full-line comments beginning with `#` or `;` are allowed. Put comments on their own lines. Later assignments override earlier ones, so put individual padding overrides after `padding`.

Reload with **Ctrl+Shift+R** or send **SIGHUP** to the deal process with `kill -HUP <deal-pid>`. The same file is reread and validated before applying changes. Syntax and value errors include the file and line in standard error. Failed reloads keep the active settings. Fonts and spacing, colors, opacity, padding, cursor, title, application ID, visual bell, shortcuts and scrollback can update while the shell runs. The initial rows and columns, shell, TERM, working directory and command apply when a new terminal starts.

| Default shortcut | Action |
|---|---|
| Ctrl+Shift+R | Reload configuration |
| Ctrl+Shift+Plus or Equal / Ctrl+Shift+Minus | Zoom in / out. Hold to repeat. |
| Ctrl+Shift+0 | Reset to the configured font size |
| Ctrl+wheel up / down | Zoom in / out |
| Wheel up / down | Scroll history or send wheel reports to an application |
| Shift+Page Up / Page Down, Shift+Home / End | Browse scrollback |
| Left drag / double-click / triple-click | Select a range / word / logical line |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy to / paste from the clipboard |
| Middle click | Paste the primary selection when available |
| Ctrl+left click | Open an HTTP or HTTPS OSC 8 link |
| F11 | Request fullscreen or return to the previous layout |

Under `[font]`, set `zoom_min` and `zoom_max` between 6 and 96 pixels and keep `size` within those limits. `zoom_step` is a positive number of pixels from 1 to 90. The `[shortcuts]` keys `zoom_in`, `zoom_out`, and `zoom_reset` are configurable. Zoom keeps the window dimensions and updates the terminal grid and PTY. With the default configuration path, deal remembers the runtime size separately in `font-zoom`. Zoom leaves `deal.conf` untouched.

Key repeat follows the compositor's rate and delay for XKB-repeatable keys. Reload, fullscreen, copy and paste are single-press actions. Vertical wheel zoom consumes the gesture locally. High-resolution wheels accumulate fractional detents, and touchpads accumulate logical scroll distance. Set `[scrollback] scroll_lines` from 1 to 100 to change the number of history lines per detent. It defaults to 3.

With `mouse.reporting` enabled, applications can request button, motion and wheel reports in legacy or SGR format. Hold `mouse.selection_override` (Shift by default) for local selection, primary paste or scrollback. Set selection colors under `[selection]`. Set `click_interval` from 100 to 1000 milliseconds to change the double- and triple-click interval. Set `primary = false` to disable primary selection.

## Text attributes

deal asks Fontconfig for bold, italic and bold italic faces of the configured family. When the family has no matching face, FreeType synthesizes the style from the regular face. `--diagnostics` lists the face chosen for each style. Fallback fonts for missing glyphs are also synthesized. Box-drawing, block and Powerline glyphs ignore italic so lines still meet.

| SGR | Effect |
|---|---|
| 1 / 22 | Bold face. With `[colors] bold_is_bright = true` (the default), bold also brightens the default and first eight colors. 22 also clears dim. |
| 3 / 23 | Italic face. Italic glyphs may overhang half a cell. |
| 4 / 24, 21 | Single underline / none, double underline |
| 4:0 – 4:5 | No, single, double, curly, dotted and dashed underline |
| 58 / 59 | Underline color as `58;5;N`, `58;2;R;G;B` or `58:2::R:G:B` / follow the foreground |
| 9 / 29 | Strikethrough |
| 53 / 55 | Overline |
| 8 / 28 | Concealed text. It stays selectable and copyable. |
| 5, 6 / 25 | Blink. It's accepted but not animated. |

Set `[terminal] term = deal` so Neovim and other terminfo programs use italics, strikethrough (`smxx`), styled underlines (`Smulx`) and underline colors (`Setulc`). With `xterm-color` they fall back to plain underlines.

## Clipboard and primary selection

The regular clipboard uses the core Wayland `wl_data_device_manager` protocol. Primary selection uses `zwp_primary_selection_device_manager_v1` when advertised by the compositor. Completing a selection publishes it to primary selection. Ctrl+Shift+C publishes it to the separate regular clipboard. Local selection and the regular clipboard remain available when the primary protocol is absent.

Both channels transfer UTF-8 plain text with nonblocking pipes. Paste uses the PTY queue and follows the application's bracketed paste mode. It preserves UTF-8 and line breaks using the terminal's carriage-return convention. Pressing Ctrl+Shift+V or middle-clicking while a transfer is pending leaves that transfer in progress.

## Terminal compatibility and limitations

The PTY, UTF-8 and wide cells, ANSI and DEC parsing, 256-color and RGB rendering, alternate screens, focus reports, cursor modes, OSC titles, link storage and scrollback reflow remain available. Scrollback is limited by its configured line count and a 64 MiB cell-data budget. The primary grid reflows in normal scrolling mode. Alternate screens and primary layouts with a custom scroll region resize by position.

The default TERM is `xterm-color`. Set `[terminal] term = deal` to use the custom terminfo defined in the [Makefile](Makefile). `make install` compiles the entry when `tic` is available. deal falls back to `xterm-color` if the custom entry is unavailable. Remote hosts also need the custom entry. Use `TERM=xterm-color ssh host` for a one-off connection.

- Keyboard support covers XKB text, terminal keys and key repeat. Compose/dead-key composition, IME integration and multiple seats are deferred.
- Clipboard transfers support plain text, with a 32 MiB limit and a ten-second inactivity timeout. Clipboard persistence after the source exits depends on the compositor or a clipboard manager. File drag and drop and rich clipboard formats are outside this milestone.
- Local scrollback is vertical. Applications requesting mouse reports can receive wheel reports on both axes. Selection highlighting can clear during reflow. An already copied selection retains its text.
- Visual bell is available. Audible and urgency bells are deferred. Decoration policy is left to the compositor. The corresponding legacy settings remain accepted.
- Bold and italic come from the primary family or synthesis. Fallback fonts are not matched by style.
- Rendering uses scale-1 buffers. HiDPI and fractional output scaling may appear soft. Font coverage depends on the installed fonts. Complex text shaping and terminal graphics protocols are outside this milestone.
- A short-lived command closes its window on exit. Add a pause to inspect its output in manual checks.

## Manual verification on dwl

### 0.10.5 text attributes

- [x] Build and check `./deal --version` reports 0.10.5. Run diagnostics and check the bold, italic and bold italic faces it reports.
- [x] Print each SGR from the table above. Check that bold and italic use real faces and that `bold_is_bright = false` keeps bold text in its original color after reload.
- [x] Check every underline style at several zoom levels. Curly, dotted and dashed patterns continue across cells without gaps. Check underline colors in 256-color and RGB forms and that 59 resets them.
- [x] Check italic letters with descenders and overhangs (`gjyf`) are not clipped badly. Check box drawing under italic stays connected.
- [x] Combine attributes with selection, the block cursor, inverse, dim and opacity below 1. Erase a line with an attribute active (`printf '\e[9;4m'; tput el`) and check nothing is drawn on the erased cells.
- [x] With `term = deal` installed, check Neovim diagnostics show undercurls in their own color, and `:hi Comment gui=italic` renders italic.
- [x] Repeat the 0.10.3 rendering and selection checks for regressions.

### 0.10.3 baseline


- [x] Build and check `./deal --version` reports 0.10.3. Inspect `readelf -d ./deal` for direct dependencies and launch with `env -u DISPLAY ./deal`.
- [x] Run diagnostics. Check xdg-shell configure, EGL vendor/version, desktop OpenGL, the expected renderer, and clipboard and primary-selection availability.
- [x] Verify an interactive prompt, UTF-8 and wide glyphs, ANSI and RGB colors, alternate-screen entry and exit, and a known `-e` exit status.
- [x] Hold a letter, Backspace, and an arrow. Check the compositor's delay and repeat rate, then release the key, change focus, and change Shift while holding a letter. Check modified arrows, Home, End, Page Up and Page Down, and F1–F12 with the fullscreen shortcut disabled.
- [x] Hold both zoom shortcuts and reset with Ctrl+Shift+0. Verify the configured step and limits and release Ctrl before the zoom key to check for stray shell input. Try customized zoom bindings.
- [x] Try Ctrl+wheel with a detented wheel, a high-resolution wheel, and a touchpad. Verify smaller movements accumulate and ordinary scrolling still works. In an application using mouse reporting, verify ordinary wheel reports and the Shift override. Check that Ctrl+wheel sends no wheel report to the application.
- [x] Single-drag across rows containing wide and combining characters. Double-click a word across a soft wrap and triple-click a wrapped logical line. Check the copied text for missing or extra line breaks. Drag above and below the grid while in scrollback.
- [x] Copy and paste in both directions between two deal windows and between deal and another Wayland application. Paste large multiline UTF-8 text, check that the UI stays responsive, and close the source or destination during a transfer.
- [x] Copy one string to the regular clipboard, select another, then use Ctrl+Shift+V and middle click to check that the two channels remain independent. Repeat with `primary = false` and on a compositor without the primary-selection protocol if available.
- [x] Check paste with bracketed mode enabled and disabled. In Neovim with mouse enabled, exercise press, drag, release and both wheel axes. Hold Shift for local selection and test release after pressing Shift partway through an application drag.
- [x] Change `scroll_lines`, selection colors and `click_interval` and reload. Verify one change per detent on a wheel emitting both distance and detent metadata.
- [x] Use Ctrl+left click on an HTTP or HTTPS OSC 8 link. Check that normal clicks start selection when mouse reporting is inactive and unsupported URL schemes do not open.
- [x] While zooming, verify the window stays the same size, `stty size` follows the grid, the shell stays running, and existing scrollback reflows. Reload with changed zoom limits and verify a reset uses the configured size.
- [x] Tile, float, resize and toggle fullscreen. Confirm the shell sees new dimensions with `stty size`. Generate scrollback and verify reflow and the scrollback shortcuts.
- [x] Set opacity below 1 and compare the background, padding, explicit cell backgrounds, and text edges. Leave a shell idle with cursor blink disabled and confirm frame submissions stop.
- [x] Change the font, opacity, palette and padding. Reload once through the shortcut and once with SIGHUP. Check that the shell PID and scrollback survive and that invalid input reports its line while preserving the active settings.
- [x] Verify compositor close, shell exit and compositor disconnection in a disposable session. Exercise the desktop entry, custom terminfo and uninstall if testing installation.

## Recommended next work

Add Compose and IME support and verify fractional scaling on dwl.
