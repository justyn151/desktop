# deal 0.9.0

deal is a C11 terminal emulator for Linux X11. It uses GLX/OpenGL for drawing, FreeType and Fontconfig for fonts, Lua 5.4 for configuration, and a Linux PTY for the shell or command.

## Build and install

Install a C compiler, pkg-config, and the development packages for Xlib, OpenGL/GLX, Lua 5.4, FreeType, and Fontconfig. The build also uses Linux libc and libutil. The optional ncurses `tic` tool compiles the custom terminfo, and `xdg-open` handles activated links.

```sh
./build.sh
./deal
./build.sh install
./build.sh uninstall
```

The default prefix is `$HOME/.local`. Installation adds `bin/deal`, a desktop launcher, an Xfce preferred-terminal helper, and an example configuration under `share/deal`. The desktop launcher uses the installation's executable path. Window class `deal` lets desktop launchers match the running application. Select **deal** in Xfce's preferred applications when the desktop discovers the helper.

For a system-wide install, build first and run `sudo env PREFIX=/usr/local ./install.sh install`. Set `DESTDIR` for package staging. Reuse the same prefix and staging root to uninstall. Uninstallation removes unmodified files recorded by the installer and leaves user configuration and other files in place.

## Command line

| Argument | Effect |
|---|---|
| `--help`, `--version` | Print usage or version and exit. |
| `--config PATH` | Use a different Lua configuration file, including on reload. |
| `--working-directory PATH` | Set the child's starting directory. |
| `--title TITLE`, `--class CLASS` | Set the initial title or X11 class. OSC titles can still change the title when enabled. |
| `--columns N`, `--rows N` | Set the initial grid size. |
| `--diagnostics` | Report configuration, terminal type, font, X11, OpenGL, and compositor availability without opening a shell. |
| `-e COMMAND [ARGS...]` | Pass the remaining arguments directly to a program and return its exit status. Place this option last. |

The window options override Lua. For example:

```sh
deal --working-directory "$PWD" --title Build --columns 100 --rows 30 -e make -j4
```

## Configuration

deal creates `$XDG_CONFIG_HOME/deal/init.lua` on first launch, using `~/.config/deal/init.lua` when `XDG_CONFIG_HOME` is unset. Existing files are preserved. See [init.example.lua](init.example.lua) for the supported settings and their defaults.

```lua
return {
    window = { padding = { horizontal = 10, vertical = 6 }, opacity = 0.90 },
    font = { family = "monospace", size = 14, line_spacing = 2 },
    cursor = { style = "bar", thickness = 2, blink = false },
    scrollback = { capacity = 10000, scroll_on_input = true },
    bell = { audible = false, visual = false, urgency = false },
}
```

All bell modes default to off. Enable them individually in Lua. Reload applies visual settings, bell modes, shortcuts, and scrollback settings. Initial grid dimensions, shell, and terminal type apply to new sessions.

| Shortcut | Action |
|---|---|
| Ctrl+Shift+C / Ctrl+Shift+V | Copy selection / paste clipboard |
| Middle click | Paste X11 PRIMARY |
| Shift+Page Up / Page Down, Shift+Home / End | Browse scrollback |
| Shift+mouse | Select text while an application uses mouse reporting |
| Ctrl+Left Click | Open an OSC 8 HTTP or HTTPS link |
| Ctrl+Shift+= / Ctrl+Shift+- | Zoom in / out |
| Ctrl+Shift+R | Reload Lua configuration |
| F11 | Toggle fullscreen through the window manager |

## Terminal compatibility and troubleshooting

deal supports UTF-8 cells, common ANSI and DEC editing controls, 256 and RGB colors, alternate screens, scroll regions, cursor modes, SGR and legacy mouse reporting, focus events, bracketed paste, OSC titles and links, and the X11 clipboard. Width changes reflow the primary transcript in its normal scrolling mode. Alternate-screen layouts and primary layouts using a custom scroll region resize by position. Reflow can clear the current selection highlight. Scrollback is bounded by its line count and a 64 MiB cell-data budget.

The default `terminal.term` is `"xterm-color"`, which is suitable for remote systems with the standard entry installed. Use `"deal"` to opt into the [custom terminfo](deal.terminfo). Installation compiles it when `tic` is available. deal falls back to `xterm-color` when it cannot find the custom entry. A remote host also needs the custom entry before it can use it. For a one-off connection use `TERM=xterm-color ssh host`.

- If the window fails to open, run `deal --diagnostics` from an X11 session. GLX and a usable OpenGL context are required.
- If the background is opaque, check compositor and ARGB visual availability in diagnostics. Fullscreen and decorations depend on window-manager support.
- If reload fails, check the process's standard error for Lua errors or invalid values. The running configuration stays active.
- Glyph coverage depends on installed fonts. Complex shaping, graphics protocols, and full xterm emulation are outside the supported subset.
