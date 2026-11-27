# Desktop Plan

Roadmap for the custom Arch Linux + dwl desktop in `~/Projects/desktop`.

Ground rules that apply to every phase:

- One `.c` and one `.h` per app, with as few external libraries as possible.
- Shared code lives in `desk/` (one `.c`/`.h` pair) once Phase 3 starts.
- Each stage gets built and manually tested before the next one starts.
- Build on working code. Don't reset it.

---

## Phase 1: Finish deal and replace foot

Goal: deal is the only terminal on the system.

### 1.1 Verify 0.10.3 (Clipboard, Selection and Mouse)

The code is already in the repo. What's left is the manual checklist in `deal/README.md`.

- [x] Work through every item in the "Manual verification on dwl" checklist
- [x] Fix whatever fails
- [x] Tick the checklist and tag 0.10.3

### 1.2 0.10.4: Scaling and rendering polish

dwl already supports `wp_fractional_scale_v1` and `wp_viewporter`.

- [ ] Integer scale with `wl_surface.preferred_buffer_scale` and `wl_surface_set_buffer_scale`
- [ ] Fractional scale with `wp_fractional_scale_v1` (scale = N/120) and `wp_viewport_set_destination`
- [ ] Rasterize glyphs at `font_size × scale`
- [ ] Compute the cell size in physical pixels first, then the grid, so there are no seams at 1.25× or 1.5×
- [ ] Re-rasterize when the window moves to an output with a different scale

### 1.3 0.11.0: Wayland feature parity

- [ ] Compose and dead keys with `xkb_compose_*` (part of libxkbcommon, no new dependency)
- [ ] Urgency bell (`xdg_activation_v1` or dwl urgency)
- [ ] Decide what to do about IME: implement it or defer it to after 1.0

### 1.4 Make deal the default terminal

- [x] `dwl/config.def.h`: `termcmd` → `deal`
- [x] `bar.conf`: the network `command` → `deal --app-id net --columns 84 --rows 26 -e net`
- [ ] dwl floating rule: confirm it matches app ID `net` when launched from deal
- [x] `run`: desktop entries with `Terminal=true` launch in deal (`terminal = deal -e`)
- [ ] Use only deal for a week, then uninstall foot

### Deferred deal milestones

- **0.12.0 (wallpaper-derived theming):** after Phase 3 step 1, so it uses `desk_palette_load` instead of adding a 6th palette parser
- **0.13.0 (optimization):** after Phase 3
- **1.0.0 (daily driver):** once 0.12 and 0.13 are done and nothing has needed foot for a while

**Phase 1 is done when:** foot is uninstalled and nothing in the repo references it.

---

## Phase 2: Bug fixing

Goal: fix existing problems before refactoring, so the migration to desk starts from known-good behavior.

### Known issues

- [x] **dwl config mismatch:** `config.def.h` still uses `MODKEY = Alt` and `Alt+Shift+Enter` for the terminal. The README documents a Super-based layout (Super+Enter, Super+Q) that only exists in the gitignored `config.h`. Commit the real layout as `config.def.h`. `config.h` is tracked too, so the running layout is in git.
- [ ] **Remaining foot references:** sweep the repo after Phase 1.4 (`grep -rn foot`)
- [ ] **dwl TODO/FIXME:** review the 4 markers in `dwl.c`

### My own list

- [ ] …
- [ ] …

**Phase 2 is done when:** every app builds without warnings, and its README checklist passes on the current session.

---

## Phase 3: Design desk and migrate to it

Goal: one copy of shared code instead of five or six.

### Shape

```text
desk/
├── desk.h
├── desk.c
└── README.md
```

- Each app compiles `../desk/desk.c` into its own binary. No `.so`, nothing extra to install.
- Feature macros keep dependencies opt-in:
  - **Layer 0** (always): paths, atomic write, INI config, palette, inotify watch, argv split and spawn, UTF-8
  - **Layer 1** (`-DDESK_WAYLAND`): shm buffer pool, layer-shell setup, scale tracking, keyboard (xkb, compose, repeat timerfd)
  - **Layer 2** (`-DDESK_FONT`): Fontconfig/FreeType loading, glyph cache, text measuring, drawing into ARGB buffers
- Rules: no event loop, no global state, no widgets, no printing (return errors), nothing app-specific.

### Shared configuration

Precedence order, implemented once in `desk_conf_load`:

1. Command-line options
2. `~/.config/<app>/<app>.conf`
3. `~/.config/desktop/desktop.conf` (shared `[font]`, `[theme]`)
4. Built-in defaults

Opacity is configured per application today: `[window] opacity` in deal and `[appearance] opacity` in wall's browser. Both apply it to the background only. Move it to `[theme] opacity` in `desktop.conf` in step 3, so every app reads one value. A compositor-wide setting in dwl (`wlr_scene_buffer_set_opacity`) would be simpler, but it fades text and images too, so the shared app setting is the better fit.

### Migration order

Migrate one step at a time. Behavior must stay identical, verified with each app's README checklist.

| Step | Code | Duplication today | Apps |
|---|---|---|---|
| 1 | Palette load, save and watch | 5 copies | net, dwl, then bar, run, wall |
| 2 | INI config parser | 6 copies | all except lead |
| 3 | Shared `desktop.conf` | n/a | all |
| 4 | shm buffers and layer-shell | 3 copies | bar, run, wall |
| 5 | Font rasterizing and glyph cache | 4 copies | bar, run, wall, deal (rasterizer only; GL atlas stays in deal) |
| 6 | Keyboard, repeat and compose | 2 copies | run, deal |

deal 0.12.0 started `desk/` early with `desk_palette_load` and `desk_watch_*`. deal already uses them, so step 1 now means moving net, dwl, bar, run and wall onto the same functions. The palette has no `ansi[16]`, so deal derives its ANSI colors from the roles.

**Phase 3 is done when:** no app contains its own palette parser, config parser, shm buffer code or font loader.

---

## Phase 4: shot

Built on desk from its first commit, so it's also a test of whether the desk API is pleasant to use. The detailed plan is in [docs/shot-roadmap.md](docs/shot-roadmap.md).

dwl already supports `ext-image-copy-capture-v1`, `ext-image-capture-source-v1` and `ext-foreign-toplevel-image-capture-source`.

| Version | Milestone |
|---|---|
| 0.1 | Full-monitor capture to PNG (libpng, already used by wall) |
| 0.2 | Region capture: freeze all outputs first, select on a layer-shell overlay, crop from the frozen frame |
| 0.3 | Clipboard (`image/png`). The process stays in the background until the selection is cancelled |
| 0.4 | Skipped: focused-window capture, not needed for now |
| 0.5 | Config, Super+Shift+S for region capture, save to `~/Pictures/Screenshots/%Y-%m-%d_%H-%M-%S.png` and copy |

Notes:
- Handle XRGB8888 and XBGR8888 shm formats
- For v1, limit a region to the output where the drag started
- Write to a temp file, then rename

---

## Phase 5: Remaining core apps

Rough order, based on what gets touched most often day to day:

1. `lock`
2. `notify`
3. `clip`
4. `vol`
5. `power`
6. `mount`
7. `view`
8. `files`
9. `ctl`
10. Then everything else from the project list below

---

## Phase 6: Gaps (last)

These are the pieces a laptop daily driver needs that aren't one of the 25 projects. They come after the core apps exist.

| Gap | Current state | Route |
|---|---|---|
| Volume keys | **Not working** (dwl config only has `volume-up_EXAMPLE`) | `vol` + XF86Audio bindings in dwl. Optional stopgap: bind `wpctl` in `config.h` |
| Brightness keys + OSD | Missing | backlight via logind `SetBrightness`, layer-shell popup |
| Idle dim/lock/suspend | Missing | `ext-idle-notify-v1` (dwl supports it) → `lock` |
| Polkit agent | Missing (net's README notes one may be needed) | polkit over D-Bus (sd-bus) |
| Portals | Missing | `xdg-desktop-portal-wlr` for screen sharing; later `files` as the FileChooser backend |
| Display configuration | Missing | `wlr-output-management` (dwl supports it) |
| Night light | Missing | `wlr-gamma-control` (dwl supports it) |
| Battery in bar | Missing | `/sys/class/power_supply` |

---

## The 25 projects

### Existing (7)

| # | Project | What it does | Status |
|---|---|---|---|
| 1 | `dwl` | Wayland compositor: tiling, directional focus and swap, gaps, IPC, wallpaper-derived borders | Daily driver |
| 2 | `bar` | Status bar: workspaces, network status, clock, live palette | Daily driver |
| 3 | `run` | Launcher for desktop entries and `$PATH` commands, with fuzzy search | Daily driver |
| 4 | `wall` | Wallpaper renderer and browser, transitions, palette generation | Daily driver |
| 5 | `net` | Wi-Fi manager TUI over NetworkManager | Daily driver |
| 6 | `deal` | Terminal emulator, migrating from X11 to native Wayland | 0.10.3, Phase 1 |
| 7 | `lead` | Modal terminal text editor (installed as `editor`) | Early (Phase 0.3) |

### Planned (18)

| # | Project | What it does |
|---|---|---|
| 8 | `desk` | Shared library: config, palette, paths, buffers, fonts, keyboard |
| 9 | `shot` | Screenshots: region, monitor, window; to file and clipboard |
| 10 | `lock` | Screen locker on `ext-session-lock-v1` |
| 11 | `notify` | Notification daemon (`org.freedesktop.Notifications`) |
| 12 | `clip` | Clipboard manager and history |
| 13 | `vol` | Audio and volume control |
| 14 | `power` | Session and power controls: logout, suspend, reboot, shutdown |
| 15 | `mount` | Storage and removable-device manager (udisks2) |
| 16 | `view` | Image viewer |
| 17 | `files` | Graphical file manager with H/J/K/L navigation |
| 18 | `ctl` | Desktop configuration tool for fonts, theme and shared settings |
| 19 | `music` | Local music player |
| 20 | `video` | Video frontend, with mpv as the playback backend |
| 21 | `read` | PDF and document reader |
| 22 | shell | Lightweight Fish-inspired command interpreter |
| 23 | `login` | Login screen, as a greetd greeter |
| 24 | `pkg` | Build and install tool for the desktop repo |
| 25 | toolkit | Shared GUI widgets, extracted from `desk` once `files` shows what is really common |
