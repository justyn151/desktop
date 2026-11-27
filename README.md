# Desktop

This repository brings together the custom dwl Wayland session and its desktop
applications. Each application has its own Makefile, documentation, and version.

Build from the repository root with the command in the table. To install, add
`install` to the same command. Every build needs a C compiler and Make. The
applications that link external libraries also use `pkg-config`. The Wayland
builds generate bindings with `wayland-scanner`.

| Application | Purpose | Build | Additional dependencies |
| --- | --- | --- | --- |
| [dwl](dwl/README.md) | Tiling Wayland compositor with desktop IPC and wallpaper lifecycle management | `make -C dwl` | wlroots 0.20, Wayland server, wayland-protocols, libinput, xkbcommon. Optional XWayland support uses xcb and xcb-icccm. |
| [bar](bar/README.md) | Wayland status bar with workspaces, network status, and a clock | `make -C bar` | Wayland client, wayland-protocols, wlr-protocols, FreeType, Fontconfig, libsystemd. The network shortcut uses deal. |
| [run](run/README.md) | Wayland launcher for desktop entries and shell commands | `make -C run` | Wayland client, wayland-protocols, wlr-protocols, xkbcommon, FreeType, Noto Sans Mono (`noto-fonts`) by default. |
| [wall](wall/README.md) | Wayland wallpaper renderer, browser, and color palette generator | `make -C wall` | Wayland client and cursor libraries, wayland-protocols, wlr-protocols, libpng, libjpeg, FreeType, Fontconfig, xkbcommon. |
| [net](net/README.md) | Terminal interface for Wi-Fi and network management | `make -C net` | libsystemd. NetworkManager must be running for network operations. |
| [deal](deal/README.md) | Native Wayland terminal emulator | `make -C deal` | Wayland client and EGL libraries, wayland-protocols, EGL, desktop OpenGL, xkbcommon, FreeType, Fontconfig, libutil. `tic` and `xdg-open` are optional. |
| [lead](lead/README.md) | Modal terminal text editor installed as `editor` | `make -C lead` | C11 compiler and the system C library. |
| [view](view/README.md) | Wayland image viewer for PNG, JPEG, GIF and WebP | `make -C view` | Wayland client, wayland-protocols, xkbcommon, libpng, libjpeg, libwebp. |
| [mark](mark/README.md) | Markdown viewer for the terminal | `make -C mark` | C11 compiler and the system C library. `make check` uses `python3`. |

`dwl`, `bar`, `run`, `wall`, and `net` install under `/usr/local` by default.
`deal` and `lead` use `$HOME/.local`. Pass `PREFIX="$HOME/.local"` to install any
of the applications for your user. Put the selected executable directory on the
session's `PATH` before launching dwl.

```sh
dwl -s 'exec bar'
```

dwl starts the wallpaper renderer and opens its browser with **Super+W**.
**Super+D** launches `run`, and clicking the bar's network indicator opens
`net` in deal. If another `net` is on the session's `PATH`, set the full path
to this repository's installed `net` in the bar configuration. For a custom
wallpaper executable location, set `DWL_WALL` to its absolute path. The
existing XDG configuration locations and dwl's local `config.h` still apply.

The customized compositor is based on upstream dwl commit `f590014`. Its
upstream documentation, licenses, and attribution are in [dwl/](dwl/).
