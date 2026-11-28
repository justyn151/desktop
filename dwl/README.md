# dwl - dwm for Wayland

Join us on our [Discord server]  
Or Matrix: [#dwl-official:matrix.org]  
Or on our IRC channel: [#dwl on Libera Chat]

dwl is a compact, hackable compositor for [Wayland] based on [wlroots]. It is
intended to fill the same space in the Wayland world that [dwm] does in X11,
primarily in terms of functionality, and secondarily in terms of
philosophy. Like [dwm], dwl is:

- Easy to understand, hack on, and extend with patches
- One C source file configurable via `config.h`
- Tied to as few external dependencies as possible

## Getting Started:

### Latest semi-stable [release]
This is probably where you want to start. This builds against the [wlroots]
versions currently shipping in major distributions. If your
distribution's `wlroots` version is older, use an earlier dwl [release].
The `wlroots` version against which a given `dwl` release builds is specified
with each release on the [release] page

### Development branch [main]
Active development progresses on the `main` branch. The `main` branch is built
against the latest release of [wlroots]. PRs should target this branch unless they
depend on functionality that is not in the current release of `wlroots`.

### Preview branch [wlroots-next]
The `wlroots-next` branch is built against the git version of [wlroots], which
is unstable and changes frequently. PRs requiring functionality from the git
version of `wlroots` should target this branch.

### Building dwl
dwl has the following dependencies:
- libinput
- wayland
- wlroots (compiled with the libinput backend)
- xkbcommon
- wayland-protocols (compile-time only)
- pkg-config (compile-time only)

dwl has the following additional dependencies if XWayland support is enabled:
- libxcb
- libxcb-wm
- wlroots (compiled with X11 support)
- Xwayland (runtime only)

Install these (and their `-devel` versions if your distro has separate
development packages) and run `make`. If you wish to build against a released
version of wlroots (*you probably do*), use a [release] or a [0.x branch]. If
you want to use the unstable development `main` branch, you need to use the git
version of [wlroots].

To enable XWayland, you should uncomment its flags in `config.mk`.

## Configuration

All configuration is done by editing `config.h` and recompiling, in the same
manner as [dwm]. There is no way to separately restart the window manager in
Wayland without restarting the entire display server, so any changes will take
effect the next time dwl is executed.

As in the [dwm] community, we encourage users to share patches they have
created. Check out the [dwl-patches] repository!

## Running dwl

dwl can be run on any of the backends supported by wlroots. This means you can
run it as a separate window inside either an X11 or Wayland session, as well as
directly from a VT console. Depending on your distro's setup, you may need to
add your user to the `video` and `input` groups before you can run dwl on a
VT. If you are using `elogind` or `systemd-logind` you need to install polkit;
otherwise you need to add yourself in the `seat` group and enable/start the
seatd daemon.

When dwl is run with no arguments, it launches the server, enables the
shortcuts configured in `config.h`, and starts the wallpaper renderer when
available. The status bar can still be started with `-s`. The background
behind the wallpaper uses the compile-time `rootcolor`.

If you would like to run a script or command automatically at startup, you can
specify the command using the `-s` option. This command will be executed as a
shell command using `/bin/sh -c`.  It serves a similar function to `.xinitrc`,
but differs in that the display server will not shut down when this process
terminates. Instead, dwl will send this process a SIGTERM at shutdown and wait
for it to terminate (if it hasn't already). This makes it ideal for execing into
a user service manager like [s6], [anopa], [runit], [dinit], or [`systemd
--user`].

Note: The `-s` command is run as a *child process* of dwl, which means that it
does not have the ability to affect the environment of dwl or of any processes
that it spawns. If you need to set environment variables that affect the entire
dwl session, these must be set prior to running dwl. For example, Wayland
requires a valid `XDG_RUNTIME_DIR`, which is usually set up by a session manager
such as `elogind` or `systemd-logind`.  If your system doesn't do this
automatically, you will need to configure it prior to launching `dwl`, e.g.:

    export XDG_RUNTIME_DIR=/tmp/xdg-runtime-$(id -u)
    mkdir -p $XDG_RUNTIME_DIR
    dwl

### Wallpaper session

Build and install `wall`, `bar`, `run`, and `dwl` using their Makefiles.
With the installed programs available to the session, start the compositor
with the bar:

```sh
dwl -s 'exec bar'
```

The default key table binds **Super+D** to `run` on the session's `PATH`
and **Super+W** to the wallpaper browser. The browser opens centered and
floating on the current monitor, like the Wi-Fi selector, and can be resized.

The bar keeps its existing `-s` status input and IPC connection. dwl
starts the renderer separately after creating the session’s Wayland
socket. The renderer restores the saved wallpaper using its existing
startup precedence. Launch the compositor as above to start both the
bar and wallpaper, and leave the wallpaper renderer out of the `-s`
command.

dwl looks for `/usr/local/bin/wall`, `$HOME/.local/bin/wall`, and the
local build at `$HOME/Projects/desktop/wall/wall` in that order. Set
`DWL_WALL` to the absolute executable path for a different install prefix
or checkout location. The renderer
and browser each use a session-specific lock. dwl also tracks the
processes it starts and sends them `SIGTERM` on exit. It allows a bounded
grace period for cleanup before terminating any of those children that
remain.

### Dynamic border colors

Runtime compositor settings are in
`$XDG_CONFIG_HOME/dwl/dwl.conf`, or `$HOME/.config/dwl/dwl.conf` when
`XDG_CONFIG_HOME` is unset, empty, or relative. The supplied
`dwl.conf` defaults to `source = manual`. To enable wallpaper colors,
use the following setting.

```ini
[theme]
source = wallpaper
```

The compositor reads `border_active` for focused windows,
`border_inactive` for unfocused windows, and `urgent` for urgent windows
from `$XDG_CACHE_HOME/wall/colors.conf`, falling back to
`$HOME/.cache/wall/colors.conf` when the cache environment variable is
unset, empty, or relative. The compositor watches the configuration and
palette directories, so valid replacements recolor existing borders
while dwl is running. The configured border width and opacity still
apply.

If no valid palette has been read, dwl uses the compile-time border
colors. A missing or invalid palette update leaves the last valid colors
in place. Set `source = manual` to restore the compile-time colors live.
If wallpaper colors are unavailable, check that the renderer has
`[colors] enabled = true`, that the processes share a cache directory,
and that the wallpaper utility’s `--colors` command prints the palette.

### Window controls

| Shortcut | Action |
| --- | --- |
| Super + H/J/K/L | Focus the nearest window left, down, up, or right on the current monitor and visible tags |
| Super + Shift + H/J/K/L | Swap the focused tiled window left, down, up, or right |
| Super + Ctrl + H/L | Decrease or increase the master area width |
| Super + Space | Toggle floating for the focused window |
| Super + Shift + F | Toggle maximization for the focused window |
| Super + F | Toggle fullscreen for the focused window |
| Super + Enter | Open the terminal |
| Super + Q | Close the focused window |
| Super + Shift + Q | Exit dwl |
| Super + Minus/Equal | Decrease or increase each gap by 2 pixels |
| Super + Shift + G | Toggle gaps |
| Super + Shift + R | Restore the configured gap sizes and enabled setting |
| Volume Up/Down | Raise or lower the default output by `vol`'s step, 5% by default, stopping at 100%; held keys repeat |
| Mute | Toggle mute on the default output |
| Mic Mute | Toggle mute on the default input |

Directional focus includes tiled and floating windows. It prefers windows that
overlap along the perpendicular axis and stops at the current monitor and tags.
Directional swapping uses the same search among visible tiled windows, skipping
floating, maximized, and fullscreen windows. The swap exchanges their tiling
positions and keeps keyboard focus on the same window.
Normal arrangement uses master-stack tiling. The IPC layout list keeps its
existing indices and the compositor stays in the tiled layout on layout requests.
A maximized window sits above tiled windows and uses the monitor area reserved for
windows, with outer gaps and borders. Restoring it returns it to tiling or to its
previous floating geometry. Fullscreen uses the whole monitor with no gaps or
borders. Leaving fullscreen returns to maximization if it is still enabled.

The master-window promotion shortcut is Super + Ctrl + Enter. Super + left or right drag still moves or resizes
windows, floating tiled windows as needed. Super + middle click toggles floating.

### Gaps and borders

Add these settings to the same `dwl.conf` used for the theme. These are the
defaults when the settings are omitted.

```ini
[gaps]
enabled = true
inner_horizontal = 8
inner_vertical = 8
outer_horizontal = 8
outer_vertical = 8
smart = false

[appearance]
border_width = 2
```

All sizes are in logical pixels. Horizontal gaps separate columns and inset the
left and right edges. Vertical gaps separate rows and inset the top and bottom
edges. Outer gaps start inside the area left by the status bar and other
exclusive layers. With `smart = true`, a monitor with a single tiled window
suppresses that window's outer gaps. Maximized windows keep the configured outer
gaps whenever gaps are enabled. Floating geometry is unaffected by the gap
settings.

Gap sizes accept integers from 0 to 1024 and border width accepts integers from
0 to 64. The two gap flags accept `true` or `false`. Save or atomically replace
`dwl.conf` to apply changes while dwl is running. Invalid settings keep the last
valid configuration. Shortcut adjustments are session-only, and saving different
gap values establishes new defaults.

## Status information

Information about selected layouts, current window title, app-id, and
selected/occupied/urgent tags is written to the stdin of the `-s` command (see
the `STATUS INFORMATION` section in `_dwl_(1)`).  This information can be used to
populate an external status bar with a script that parses the
information. Failing to read this information will cause dwl to block, so if you
do want to run a startup command that does not consume the status information,
you can close standard input with the `<&-` shell redirection, for example:

    dwl -s 'foot --server <&-'

If your startup command is a shell script, you can achieve the same inside the
script with the line

    exec <&-

To get a list of status bars that work with dwl consult our [wiki].

### (Known) Java nonreparenting WM issue
Certain IDEs don't display correctly unless an environmental variable for Java AWT
indicates that the WM is nonreparenting.

For some Java AWT-based IDEs, such as Xilinx Vivado and Microchip MPLAB X, the
following environment variable needs to be set before running the IDE or dwl:

    export _JAVA_AWT_WM_NONREPARENTING=1

## Replacements for X applications

You can find a [list of useful resources on our wiki].

## Background

dwl is not meant to provide every feature under the sun. Instead, like [dwm], it
sticks to features which are necessary, simple, and straightforward to implement
given the base on which it is built. Implemented default features are:

- Any features provided by [dwm]/Xlib: simple window borders, tags, keybindings,
  client rules, mouse move/resize. Providing a built-in status bar is an
  exception to this goal, to avoid dependencies on font rendering and/or drawing
  libraries when an external bar could work well.
- Configurable multi-monitor layout support, including position and rotation
- Configurable HiDPI/multi-DPI support
- Idle-inhibit protocol which lets applications such as mpv disable idle
  monitoring
- Provide information to external status bars via stdout/stdin
- Urgency hints via xdg-activate protocol
- Support screen lockers via ext-session-lock-v1 protocol
- Various Wayland protocols
- XWayland support as provided by wlroots (can be enabled in `config.mk`)
- Zero flickering - Wayland users naturally expect that "every frame is perfect"
- Layer shell popups (used by Waybar)
- Damage tracking provided by scenegraph API

Given the Wayland architecture, dwl has to implement features from [dwm] **and**
the xorg-server. Because of this, it is impossible to maintain the original
project goal of 2000 SLOC and have a reasonably complete compositor with
features comparable to [dwm]. However, this does not mean that the code will grow
indiscriminately. We will try to keep the code as small as possible.

Features under consideration (possibly as patches) are:

- Protocols made trivial by wlroots

Feature *non-goals* for the main codebase include:

- Client-side decoration (any more than is necessary to tell the clients not to)
- Client-initiated window management, such as move, resize, and close, which can
  be done through the compositor
- Animations and visual effects

## Acknowledgements

dwl began by extending the TinyWL example provided (CC0) by the sway/wlroots
developers. This was made possible in many cases by looking at how sway
accomplished something, then trying to do the same in as suckless a way as
possible.

Many thanks to suckless.org and the [dwm] developers and community for the
inspiration, and to the various contributors to the project, including:

- **Devin J. Pohly for creating and nurturing the fledgling project**
- Alexander Courtis for the XWayland implementation
- Guido Cella for the layer-shell protocol implementation, patch maintenance,
  and for helping to keep the project running
- Stivvo for output management and fullscreen support, and patch maintenance

Also, thanks to our current lead developers:

- @thanatos
- @guidocella

[wlroots]: https://gitlab.freedesktop.org/wlroots
[dwm]: https://dwm.suckless.org/
[`systemd --user`]: https://wiki.archlinux.org/title/Systemd/User
[#dwl on Libera Chat]: https://web.libera.chat/?channels=#dwl
[0.7-rc1]: https://codeberg.org/dwl/dwl/releases/tag/v0.7-rc1
[0.x branch]: https://codeberg.org/dwl/dwl/branches
[anopa]: https://jjacky.com/anopa/
[dinit]: https://davmac.org/projects/dinit/
[dwl-patches]: https://codeberg.org/dwl/dwl-patches
[list of useful resources on our wiki]: https://codeberg.org/dwl/dwl/wiki/Home#migrating-from-x
[main]: https://codeberg.org/dwl/dwl/src/branch/main
[wlroots-next]: https://codeberg.org/dwl/dwl/src/branch/wlroots-next
[release]: https://codeberg.org/dwl/dwl/releases
[runit]: http://smarden.org/runit/faq.html#userservices
[s6]: https://skarnet.org/software/s6/
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots/
[wiki]: https://codeberg.org/dwl/dwl/wiki/Home#compatible-status-bars
[Discord server]: https://discord.gg/jJxZnrGPWN
[Wayland]: https://wayland.freedesktop.org/
[#dwl-official:matrix.org]: https://matrix.to/#/#dwl-official:matrix.org
