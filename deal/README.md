# deal 0.13.0

Experimental native Wayland milestone for Linux and dwl. deal uses xdg-shell for the window, EGL and desktop OpenGL for the existing fixed-function renderer, FreeType and Fontconfig for fonts, and a Linux PTY for the shell or command. Frames are drawn on changes and paced by the compositor. XWayland and an X server are unnecessary.

Version 0.13.0 adds tabs. Version 0.12.1 gave explicit backgrounds that match the default background the window's opacity. Version 0.12.0 takes its colors from the wallpaper and answers OSC 4 color queries. Version 0.11.2 kept the window margins even and answers resizes without waiting for the next frame. Version 0.11.1 added OSC 52 copy, synchronized output, and new windows in the current directory. Version 0.11.0 added Compose and dead-key input and the urgency bell. Version 0.10.5 added bold, italic and bold italic font faces, underline styles and colors, strikethrough, overline and concealed text. Version 0.10.3 added text selection, native clipboard and optional primary selection, bracketed paste, application mouse reporting, and OSC 8 link activation.

## Build on Arch Linux

| Purpose | Packages |
|---|---|
| Compiler, Make and pkg-config | `gcc`, `make`, `pkgconf` |
| Wayland client, EGL window glue, scanner and protocol XML | `wayland`, `wayland-protocols` |
| Desktop OpenGL and EGL | `libglvnd` and a compatible graphics driver such as `mesa` |
| Fonts and keyboard input | `freetype2`, `fontconfig`, `libxkbcommon`, and a monospace font such as `ttf-dejavu` |
| Optional custom terminfo compiler | `ncurses` for `tic` |
| Optional HTTP/HTTPS opener | `xdg-utils` for `xdg-open` |

The palette reader comes from [desk](../desk/README.md), which Make compiles into the executable. Linux's libc and libutil provide PTY support. Make generates bindings for xdg-shell, `primary-selection-unstable-v1` and `xdg-activation-v1` in `.build/`. Use `make clean` to remove the executable and generated bindings.

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
| Ctrl+Shift+N | Open a new window in the shell's current directory |
| Ctrl+Shift+T / Ctrl+Shift+W | Open a tab in the current directory / close the current tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab, with two or more tabs |
| Alt+1 … Alt+9 | Go to tab 1 to 9, with two or more tabs |

Under `[font]`, set `zoom_min` and `zoom_max` between 6 and 96 pixels and keep `size` within those limits. `zoom_step` is a positive number of pixels from 1 to 90. The `[shortcuts]` keys `zoom_in`, `zoom_out`, and `zoom_reset` are configurable. Zoom keeps the window dimensions and updates the terminal grid and PTY. With the default configuration path, deal remembers the runtime size separately in `font-zoom`. Zoom leaves `deal.conf` untouched.

Key repeat follows the compositor's rate and delay for XKB-repeatable keys. Reload, fullscreen, copy and paste are single-press actions. Vertical wheel zoom consumes the gesture locally. High-resolution wheels accumulate fractional detents, and touchpads accumulate logical scroll distance. Set `[scrollback] scroll_lines` from 1 to 100 to change the number of history lines per detent. It defaults to 3.

With `mouse.reporting` enabled, applications can request button, motion and wheel reports in legacy or SGR format. Hold `mouse.selection_override` (Shift by default) for local selection, primary paste or scrollback. Set selection colors under `[selection]`. Set `click_interval` from 100 to 1000 milliseconds to change the double- and triple-click interval. Set `primary = false` to disable primary selection.

## Tabs

Each tab has its own shell, screens, scrollback, selection and title. Background tabs keep running and receive their output. Only the active tab is drawn and receives keyboard and mouse input, and the window title follows it. A new tab runs your shell, not the `-e` command, in the current tab's directory. A tab closes when its shell exits, and closing the last tab closes the window. A window holds up to 16 tabs. Resizes, zoom and configuration reloads apply to every tab. The switching shortcuts only act with two or more tabs, so with one tab Ctrl+Tab and Alt+digits still reach the application. Set any tab shortcut to `false` under `[keybinds]` to disable it.

There's no tab bar yet. It comes next, with click to switch and a marker for background tabs that rang the bell.

## Wallpaper theme

Set `[theme] source = wallpaper` to take the colors from the palette that [wall](../wall/README.md#wallpaper-colors) publishes in `$XDG_CACHE_HOME/wall/colors.conf` (or `~/.cache/wall/colors.conf`). Turn on palette generation with `[colors] enabled = true` in `wall.conf`. Open windows recolor when the wallpaper changes, without restarting the shell.

| Palette role | deal color |
|---|---|
| `background`, `foreground` | Default background and foreground, ANSI 15 on dark themes |
| `accent` | Cursor. It also replaces the hue of the nearest ANSI color when it's saturated enough. |
| `muted` | ANSI 8 (bright black) |
| `selection_background`, `selection_foreground` | Selection colors |

The palette has no ANSI colors, so deal derives them. Red, green, yellow, blue, magenta and cyan keep their usual hues so their meaning survives, such as errors in red. deal picks their lightness in the Oklab color space to reach a contrast of at least 4.5:1 against the background (5.5:1 for the bright colors). Black and white come from the background and foreground. Explicit `[palette]` entries still override the derived colors. When the palette is missing or invalid, deal keeps the colors it's using. `--diagnostics` shows the palette path and whether it's usable.

Programs can read the current colors with OSC 4 (`ESC ] 4 ; N ; ? BEL`) and OSC 10, 11 and 12. Setting colors through OSC is not supported.

## Remote copy, synchronized output and new windows

**OSC 52.** Programs can copy to the clipboard by printing `ESC ] 52 ; c ; <base64> BEL`. This works over ssh, so Neovim's `"+y` reaches the local clipboard (Neovim 0.10 and later use OSC 52 automatically in an ssh session without another clipboard tool). In tmux, set `set -g set-clipboard on`. Targets `c` (or empty) copy to the clipboard, and `p` or `s` to the primary selection. Payloads up to 16 MiB of base64 are accepted. Reading the clipboard with `?` is not supported, because it would let any program read what you copied. Set `[selection] osc52 = false` to ignore OSC 52 completely.

**Synchronized output.** Programs that set mode 2026 (`ESC [ ? 2026 h`) while they redraw, like Neovim and many TUI libraries, get one complete frame when they reset it. deal shows a frame anyway after 150 ms, so a program that forgets to reset the mode can't freeze the window. `ESC [ ? 2026 $ p` reports the mode.

**New windows.** Ctrl+Shift+N opens another deal in the shell's directory. deal uses the directory the shell reports with OSC 7 (`ESC ] 7 ; file://host/path`) when the host is this machine, and otherwise reads the shell process's current directory, so it works without shell setup. A directory reported by a remote shell over ssh is ignored. The new window uses the same `--config` file. Set `[keybinds] new_window = false` to disable it.

## Compose and the bell

deal reads the Compose table for the locale in `LC_ALL`, `LC_CTYPE` or `LANG`, so dead keys and the Compose key (for example `setxkbmap -option compose:ralt` or `xkb_options compose:ralt` in dwl) work as in other applications. `~/.XCompose` is honored. A cancelled sequence types nothing, and composed characters don't repeat. `--diagnostics` shows the Compose locale in use.

When a program rings the bell (BEL) while the window is unfocused, `[bell] urgency = true` asks the compositor for attention through `xdg-activation-v1`. dwl marks the window urgent and highlights its tag without moving focus. Focusing the window clears it. deal sends at most one request a second. `visual = true` flashes the window as before.

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

- Keyboard support covers XKB text, Compose and dead keys, terminal keys and key repeat. IME integration and multiple seats are deferred until after 1.0.
- Clipboard transfers support plain text, with a 32 MiB limit and a ten-second inactivity timeout. Clipboard persistence after the source exits depends on the compositor or a clipboard manager. File drag and drop and rich clipboard formats are outside this milestone.
- Local scrollback is vertical. Applications requesting mouse reports can receive wheel reports on both axes. Selection highlighting can clear during reflow. An already copied selection retains its text.
- Visual and urgency bells are available. The audible bell is deferred. Decoration policy is left to the compositor. The corresponding legacy settings remain accepted.
- Bold and italic come from the primary family or synthesis. Fallback fonts are not matched by style.
- Rendering uses scale-1 buffers. HiDPI and fractional output scaling may appear soft. Font coverage depends on the installed fonts. Complex text shaping and terminal graphics protocols are outside this milestone.
- A short-lived command closes its window on exit. Add a pause to inspect its output in manual checks.

## Manual verification on dwl

### 0.13.0 Tabs

- [ ] Build and check `./deal --version` reports 0.13.0.
- [ ] `cd /tmp`, press Ctrl+Shift+T and run `pwd` in the new tab. Switch with Ctrl+Tab, Ctrl+Shift+Tab and Alt+1/Alt+2, and check each tab keeps its own screen, scrollback and title.
- [ ] Run `for i in $(seq 30); do echo $i; sleep 0.2; done` in one tab and switch away. Come back and check every line arrived.
- [ ] Resize the window and zoom with two tabs open, then run `stty size` in both. Reload the configuration and check both tabs change.
- [ ] Open Neovim in one tab and a shell in another. Switch back and forth and check Neovim redraws correctly and receives focus events.
- [ ] Type `exit` in a background-free tab and check the tab closes and the next one is shown. Close tabs with Ctrl+Shift+W until the window closes.
- [ ] With one tab, check Ctrl+Tab reaches the application (for example in `cat -v`).
- [ ] Copy in one tab and paste in another. Run `deal -e sh -c 'exit 3'; echo $?` from another terminal and check it prints 3.

### 0.12.0 Wallpaper theme

- [ ] Build and check `./deal --version` reports 0.12.0, and that diagnostics show `Theme: wallpaper (...colors.conf)` with `[theme] source = wallpaper`.
- [ ] Check the background, foreground, cursor and selection match `wall --colors`.
- [ ] Run `for i in $(seq 0 15); do printf '\e[48;5;%dm  ' $i; done; printf '\e[0m\n'` and `git diff` or `ls --color`. Every color is readable against the background, and red, green and blue still look like themselves.
- [ ] Change the wallpaper with Super+W. Open deal windows recolor within a second, the shell keeps running, and colored text already on screen changes to the new palette.
- [ ] Set `2 = #ff0000` under `[palette]` and reload. ANSI 1 (red) stays `#ff0000` after a wallpaper change.
- [ ] Set `source = manual` and reload. The colors from `deal.conf` come back.

### 0.11.1 Remote copy, synchronized output and new windows

- [ ] Build and check `./deal --version` reports 0.11.1.
- [ ] Run `printf '\e]52;c;%s\a' "$(printf 'hello from osc52' | base64)"`, then paste with Ctrl+Shift+V and into another application. Repeat with `p` instead of `c` and paste with middle click.
- [ ] Over ssh to any host, run the same command and check it reaches the local clipboard. In Neovim over ssh, yank with `"+y` and paste locally.
- [ ] Set `osc52 = false`, reload, and check the command changes nothing. Check `printf '\e]52;c;?\a'` never prints the clipboard back.
- [ ] Scroll quickly through a large file in Neovim and check it redraws without tearing or half-drawn screens. Run `printf '\e[?2026h'; sleep 2; echo after; printf '\e[?2026l'` and check the window keeps updating.
- [ ] `cd /tmp`, press Ctrl+Shift+N, and run `pwd` in the new window. Repeat after `cd` into a directory with a space in its name. Close the first window and check the second keeps running.
- [ ] Inside `ssh host`, `cd /` there and press Ctrl+Shift+N. The new window opens in the local shell's directory, not `/` on the remote host.
- [ ] Repeat the 0.10.3 clipboard checks for regressions.

### 0.11.0 Compose and urgency

Compose checks are not run on this setup because no Compose key or dead-key layout is configured.

- [x] Build and check `./deal --version` reports 0.11.0. Diagnostics show a Compose locale and `Urgency (xdg-activation): disabled` by default, `enabled` with `urgency = true`.
- [ ] With a Compose key, type Compose `'` `e`, Compose `o` `c`, and Compose `-` `-` `.`, then check `é`, `©` and `–`. Type a sequence that doesn't exist and check nothing is inserted.
- [ ] With a dead-key layout such as `us(intl)`, check `'` `e` gives `é`, `'` Space gives `'`, and `"` `u` gives `ü`.
- [ ] Hold the final key of a sequence and check it doesn't repeat. Start a sequence, change focus, come back and type a letter: the letter appears without composition.
- [x] With `urgency = true`, run `sleep 3; printf '\a'`, then focus another window. Check the deal window becomes urgent in dwl and the bar, and focusing it clears the urgency. A bell in the focused window does nothing.
- [x] Run `for i in $(seq 50); do printf '\a'; sleep 0.05; done` in an unfocused window and check dwl stays responsive. Set `urgency = false`, reload, and check the bell no longer marks the window.
- [x] Repeat the 0.10.5 and 0.10.3 keyboard checks for regressions.

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
